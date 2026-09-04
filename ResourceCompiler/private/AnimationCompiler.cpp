#include "AnimationCompiler.hpp"

#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <fastgltf/tools.hpp>
#include <fmt/format.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <ranges>

#include "Animation/AnimationClip.hpp"
#include "GltfElementTraits.hpp"

namespace ox::rc {
constexpr static auto ANIMATION_DEFAULT_FPS = 30.0f;
constexpr static auto ANIMATION_MAX_FPS = 120.0f;

auto gltf_node_local_transform(const fastgltf::Node& node) -> BoneTransform {
  if (const auto* trs = std::get_if<fastgltf::TRS>(&node.transform)) {
    const auto scale = glm::vec3(trs->scale[0], trs->scale[1], trs->scale[2]);
    return BoneTransform::from_trs(
      glm::vec3(trs->translation[0], trs->translation[1], trs->translation[2]),
      glm::quat::wxyz(trs->rotation[3], trs->rotation[0], trs->rotation[1], trs->rotation[2]),
      (scale.x + scale.y + scale.z) / 3.0f
    );
  }

  const auto* matrix = std::get_if<fastgltf::math::fmat4x4>(&node.transform);
  return matrix ? BoneTransform::from_mat4(glm::make_mat4(matrix->data())) : BoneTransform{};
}

auto build_gltf_node_parents(const fastgltf::Asset& gltf_asset) -> std::vector<usize> {
  auto parents = std::vector<usize>(gltf_asset.nodes.size(), ~0_sz);
  for (const auto& [node, node_index] : std::views::zip(gltf_asset.nodes, std::views::iota(0_sz))) {
    for (const auto child : node.children) {
      parents[child] = node_index;
    }
  }

  return parents;
}

auto build_gltf_skeleton(Session& session, const fastgltf::Asset& gltf_asset, const fastgltf::Skin& gltf_skin)
  -> option<SkinBuildData> {
  ZoneScoped;

  const auto joint_count = gltf_skin.joints.size();
  if (joint_count == 0) {
    return nullopt;
  }

  const auto node_parents = build_gltf_node_parents(gltf_asset);

  auto node_to_joint = ankerl::unordered_dense::map<usize, u32>();
  node_to_joint.reserve(joint_count);
  for (const auto& [node_index, joint_slot] : std::views::zip(gltf_skin.joints, std::views::iota(0_u32))) {
    node_to_joint.emplace(static_cast<usize>(node_index), joint_slot);
  }

  // depth-first from every joint whose parent is outside the skin, which emits parents before
  // children, the invariant the whole model-space accumulation relies on
  auto build = SkinBuildData{};
  build.joint_to_bone.assign(joint_count, 0_u32);
  build.bone_to_node.reserve(joint_count);

  auto joint_parent_bone = std::vector<i32>();
  joint_parent_bone.reserve(joint_count);

  auto visited = std::vector<bool>(joint_count, false);
  auto stack = std::vector<std::pair<u32, i32>>(); // joint slot, parent bone index

  for (auto slot = 0_u32; slot < joint_count; ++slot) {
    const auto parent_node = node_parents[gltf_skin.joints[slot]];
    if (parent_node == ~0_sz || !node_to_joint.contains(parent_node)) {
      stack.emplace_back(slot, -1);
    }
  }

  if (stack.empty()) {
    session.push_error(fmt::format("Skin '{}' has no root joint; its joint hierarchy is cyclic.", gltf_skin.name));
    return nullopt;
  }

  while (!stack.empty()) {
    const auto [slot, parent_bone] = stack.back();
    stack.pop_back();

    if (visited[slot]) {
      continue;
    }
    visited[slot] = true;

    const auto bone = static_cast<u32>(build.bone_to_node.size());
    build.joint_to_bone[slot] = bone;
    build.bone_to_node.push_back(gltf_skin.joints[slot]);
    joint_parent_bone.push_back(parent_bone);

    const auto& node = gltf_asset.nodes[gltf_skin.joints[slot]];
    for (const auto child : std::views::reverse(node.children)) {
      if (const auto it = node_to_joint.find(static_cast<usize>(child)); it != node_to_joint.end()) {
        stack.emplace_back(it->second, static_cast<i32>(bone));
      }
    }
  }

  if (build.bone_to_node.size() != joint_count) {
    session.push_error(fmt::format("Skin '{}' has joints unreachable from any root joint.", gltf_skin.name));
    return nullopt;
  }

  // ancestors above the first root joint, which every root joint is assumed to share because a rig
  // with roots under different ancestors is malformed
  {
    auto prefix_chain = std::vector<usize>();
    auto node = node_parents[build.bone_to_node[0]];
    while (node != ~0_sz) {
      prefix_chain.push_back(node);
      node = node_parents[node];
    }

    for (const auto ancestor : std::views::reverse(prefix_chain)) {
      build.root_prefix = build.root_prefix * gltf_node_local_transform(gltf_asset.nodes[ancestor]);
    }
  }

  auto& skeleton = build.skeleton;
  skeleton.parent_indices = std::move(joint_parent_bone);
  skeleton.bone_names.reserve(joint_count);
  skeleton.parent_space_reference_pose.reserve(joint_count);
  skeleton.inverse_bind_pose.assign(joint_count, BoneTransform{});

  for (auto bone = 0_u32; bone < joint_count; ++bone) {
    const auto& node = gltf_asset.nodes[build.bone_to_node[bone]];
    auto name = std::string(node.name);
    skeleton.bone_names.emplace_back(name.empty() ? fmt::format("bone_{}", bone) : std::move(name));

    auto local = gltf_node_local_transform(node);
    if (skeleton.parent_indices[bone] < 0) {
      local = build.root_prefix * local;
    }
    skeleton.parent_space_reference_pose.emplace_back(local);
  }

  if (gltf_skin.inverseBindMatrices.has_value()) {
    const auto& accessor = gltf_asset.accessors[gltf_skin.inverseBindMatrices.value()];
    fastgltf::iterateAccessorWithIndex<fastgltf::math::fmat4x4>(
      gltf_asset,
      accessor,
      [&](const fastgltf::math::fmat4x4& matrix, const usize slot) {
        if (slot < joint_count) {
          skeleton.inverse_bind_pose[build.joint_to_bone[slot]] = BoneTransform::from_mat4(
            glm::make_mat4(matrix.data())
          );
        }
      }
    );
  }

  if (!skeleton.finalize()) {
    session.push_error(fmt::format("Skin '{}' produced an invalid skeleton.", gltf_skin.name));
    return nullopt;
  }

  return build;
}

struct GltfAnimationSampler {
  std::vector<f32> times = {};
  // vec3 channels use xyz, rotation uses all four
  std::vector<glm::vec4> values = {};
  fastgltf::AnimationInterpolation interpolation = fastgltf::AnimationInterpolation::Linear;

  auto sample(this const GltfAnimationSampler& self, f32 time, const glm::vec4& fallback) -> glm::vec4;
};

auto GltfAnimationSampler::sample(this const GltfAnimationSampler& self, const f32 time, const glm::vec4& fallback)
  -> glm::vec4 {
  const auto is_cubic = self.interpolation == fastgltf::AnimationInterpolation::CubicSpline;
  const auto key_count = self.times.size();
  const auto value_at = [&](const usize key) {
    return self.values[is_cubic ? key * 3 + 1 : key];
  };

  if (key_count == 0 || self.values.size() < (is_cubic ? key_count * 3 : key_count)) {
    return fallback;
  }
  if (key_count == 1 || time <= self.times.front()) {
    return value_at(0);
  }
  if (time >= self.times.back()) {
    return value_at(key_count - 1);
  }

  const auto upper = static_cast<usize>(
    std::upper_bound(self.times.begin(), self.times.end(), time) - self.times.begin()
  );
  const auto lower = upper - 1;
  const auto span = self.times[upper] - self.times[lower];
  const auto t = span > glm::epsilon<f32>() ? (time - self.times[lower]) / span : 0.0f;

  switch (self.interpolation) {
    case fastgltf::AnimationInterpolation::Step: {
      return value_at(lower);
    }
    case fastgltf::AnimationInterpolation::CubicSpline: {
      const auto p0 = self.values[lower * 3 + 1];
      const auto m0 = self.values[lower * 3 + 2] * span;
      const auto p1 = self.values[upper * 3 + 1];
      const auto m1 = self.values[upper * 3 + 0] * span;
      const auto t2 = t * t;
      const auto t3 = t2 * t;
      return (2.0f * t3 - 3.0f * t2 + 1.0f) * p0 + (t3 - 2.0f * t2 + t) * m0 + (-2.0f * t3 + 3.0f * t2) * p1 +
             (t3 - t2) * m1;
    }
    default: {
      return glm::mix(value_at(lower), value_at(upper), t);
    }
  }
}

auto read_gltf_animation_sampler(const fastgltf::Asset& gltf_asset, const fastgltf::AnimationSampler& gltf_sampler)
  -> GltfAnimationSampler {
  ZoneScoped;

  auto sampler = GltfAnimationSampler{.interpolation = gltf_sampler.interpolation};

  const auto& input = gltf_asset.accessors[gltf_sampler.inputAccessor];
  sampler.times.resize(input.count);
  fastgltf::iterateAccessorWithIndex<f32>(gltf_asset, input, [&](const f32 t, const usize i) { sampler.times[i] = t; });

  const auto& output = gltf_asset.accessors[gltf_sampler.outputAccessor];
  sampler.values.resize(output.count);
  if (output.type == fastgltf::AccessorType::Vec4) {
    fastgltf::iterateAccessorWithIndex<glm::vec4>(gltf_asset, output, [&](const glm::vec4 v, const usize i) {
      sampler.values[i] = v;
    });
  } else if (output.type == fastgltf::AccessorType::Vec3) {
    fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf_asset, output, [&](const glm::vec3 v, const usize i) {
      sampler.values[i] = glm::vec4(v, 0.0f);
    });
  } else {
    sampler.values.clear();
  }

  return sampler;
}

struct BoneChannels {
  option<usize> translation = nullopt;
  option<usize> rotation = nullopt;
  option<usize> scale = nullopt;
};

auto build_gltf_animation(
  const fastgltf::Asset& gltf_asset, const fastgltf::Animation& gltf_animation, const SkinBuildData& skin
) -> option<ModelData::Animation> {
  ZoneScoped;

  const auto bone_count = skin.skeleton.bone_count();

  auto node_to_bone = ankerl::unordered_dense::map<usize, u32>();
  node_to_bone.reserve(bone_count);
  for (const auto& [node_index, bone] : std::views::zip(skin.bone_to_node, std::views::iota(0_u32))) {
    node_to_bone.emplace(node_index, bone);
  }

  auto samplers = std::vector<GltfAnimationSampler>();
  samplers.reserve(gltf_animation.samplers.size());
  for (const auto& gltf_sampler : gltf_animation.samplers) {
    samplers.emplace_back(read_gltf_animation_sampler(gltf_asset, gltf_sampler));
  }

  auto bone_channels = std::vector<BoneChannels>(bone_count);
  auto duration = 0.0f;
  auto max_key_count = 0_sz;
  auto touched_bones = false;

  for (const auto& channel : gltf_animation.channels) {
    if (!channel.nodeIndex.has_value() || channel.samplerIndex >= samplers.size()) {
      continue;
    }

    const auto bone_it = node_to_bone.find(static_cast<usize>(channel.nodeIndex.value()));
    if (bone_it == node_to_bone.end()) {
      continue;
    }

    auto& channels = bone_channels[bone_it->second];
    switch (channel.path) {
      case fastgltf::AnimationPath::Translation: channels.translation = channel.samplerIndex; break;
      case fastgltf::AnimationPath::Rotation   : channels.rotation = channel.samplerIndex; break;
      case fastgltf::AnimationPath::Scale      : channels.scale = channel.samplerIndex; break;
      default                                  : continue;
    }

    const auto& sampler = samplers[channel.samplerIndex];
    if (!sampler.times.empty()) {
      duration = glm::max(duration, sampler.times.back());
      max_key_count = ox::max(max_key_count, sampler.times.size());
    }
    touched_bones = true;
  }

  if (!touched_bones || duration <= 0.0f) {
    return nullopt;
  }

  // resample onto a fixed rate, which is what makes the frame-major layout possible at all, with
  // enough frames that a densely keyed clip does not lose detail
  auto fps = ANIMATION_DEFAULT_FPS;
  if (max_key_count > 1) {
    fps = glm::clamp(static_cast<f32>(max_key_count - 1) / duration, ANIMATION_DEFAULT_FPS, ANIMATION_MAX_FPS);
  }

  const auto frame_count = ox::max(2_u32, static_cast<u32>(glm::round(duration * fps)) + 1_u32);

  auto sampled = std::vector<BoneTransform>(static_cast<usize>(frame_count) * bone_count);
  for (auto frame = 0_u32; frame < frame_count; ++frame) {
    const auto time = duration * static_cast<f32>(frame) / static_cast<f32>(frame_count - 1);

    for (auto bone = 0_u32; bone < bone_count; ++bone) {
      const auto& reference = skin.skeleton.parent_space_reference_pose[bone];
      const auto is_root = skin.skeleton.parent_indices[bone] < 0;
      // glTF channels drive node-local values and the reference pose already carries the root
      // prefix, so strip it before sampling and put it back afterwards
      const auto local_reference = is_root ? skin.root_prefix.inverse() * reference : reference;

      auto& channels = bone_channels[bone];
      auto translation = local_reference.translation();
      auto rotation = local_reference.rotation;
      auto scale = local_reference.scale();

      if (channels.translation.has_value()) {
        translation = glm::vec3(samplers[channels.translation.value()].sample(time, glm::vec4(translation, 0.0f)));
      }
      if (channels.rotation.has_value()) {
        const auto sampled_rotation = samplers[channels.rotation.value()].sample(
          time,
          glm::vec4(rotation.x, rotation.y, rotation.z, rotation.w)
        );
        rotation = glm::normalize(
          glm::quat::wxyz(sampled_rotation.w, sampled_rotation.x, sampled_rotation.y, sampled_rotation.z)
        );
      }
      if (channels.scale.has_value()) {
        const auto sampled_scale = glm::vec3(samplers[channels.scale.value()].sample(time, glm::vec4(scale)));
        scale = (sampled_scale.x + sampled_scale.y + sampled_scale.z) / 3.0f;
      }

      auto local = BoneTransform::from_trs(translation, rotation, scale);
      sampled[static_cast<usize>(frame) * bone_count + bone] = is_root ? skin.root_prefix * local : local;
    }
  }

  auto clip = AnimationClip{};
  clip.name = std::string(gltf_animation.name);
  clip.frame_count = frame_count;
  clip.duration = duration;
  animation::compress_tracks(clip, sampled, bone_count, frame_count);

  return to_model_animation(clip);
}
} // namespace ox::rc
