#include "Animation/AnimationClip.hpp"

#include <glm/gtc/constants.hpp>

#include "Animation/BoneTransform.hpp"
#include "Animation/Pose.hpp"
#include "Memory/Stack.hpp"

namespace ox {
auto AnimationClip::fps(this const AnimationClip& self) -> f32 {
  if (self.frame_count < 2 || self.duration <= 0.f) {
    return 0.f;
  }

  return static_cast<f32>(self.frame_count - 1) / self.duration;
}

auto AnimationClip::read_compressed_pose(
  this const AnimationClip& self, const u32 frame, const std::span<BoneTransform> out
) -> void {
  ZoneScoped;

  if (frame >= self.compressed_pose_offsets.size()) {
    return;
  }

  const auto* block = self.compressed_pose_data.data() + self.compressed_pose_offsets[frame];
  const auto count = ox::min(static_cast<u32>(out.size()), self.bone_count());

  for (auto i = 0_u32; i < count; ++i) {
    const auto& track = self.track_defs[i];
    const auto* cursor = block + track.track_read_offset;

    auto rotation = glm::quat{};
    if (track.is_rotation_static) {
      rotation = track.constant_rotation.decode();
    } else {
      rotation = animation::EncodedQuaternion{cursor[0], cursor[1], cursor[2]}.decode();
      cursor += 3;
    }

    auto translation = glm::vec3{};
    if (track.is_translation_static) {
      translation = track.static_translation();
    } else {
      translation = {
        track.translation_range_x.decode(cursor[0]),
        track.translation_range_y.decode(cursor[1]),
        track.translation_range_z.decode(cursor[2]),
      };
      cursor += 3;
    }

    auto scale = 1.f;
    if (track.is_scale_static) {
      scale = track.static_scale();
    } else {
      scale = track.scale_range.decode(cursor[0]);
    }

    out[i] = BoneTransform::from_trs(translation, rotation, scale);
  }
}

auto AnimationClip::sample(this const AnimationClip& self, const f32 time, Pose& out) -> void {
  ZoneScoped;

  if (self.frame_count == 0 || out.bone_count() == 0) {
    return;
  }

  out.clear_model_space_transforms();

  const auto clamped = glm::clamp(time, 0.f, self.duration);
  const auto frame_position = clamped * self.fps();
  const auto lower = ox::min(static_cast<u32>(frame_position), self.frame_count - 1);
  const auto upper = ox::min(lower + 1, self.frame_count - 1);
  const auto fraction = frame_position - static_cast<f32>(lower);

  self.read_compressed_pose(lower, out.parent_space_transforms);

  if (upper != lower && fraction > glm::epsilon<f32>()) {
    memory::ScopedStack stack;
    auto scratch = stack.alloc<BoneTransform>(out.bone_count());
    self.read_compressed_pose(upper, scratch);

    for (auto i = 0_u32; i < out.bone_count(); ++i) {
      out.parent_space_transforms[i] = slerp(out.parent_space_transforms[i], scratch[i], fraction);
    }
  }

  out.state = Pose::State::ParentSpace;
}

namespace animation {
auto compress_tracks(
  AnimationClip& clip, const std::span<const BoneTransform> sampled, const u32 bone_count, const u32 frame_count
) -> void {
  ZoneScoped;

  const auto bone_frame = [&](const u32 bone, const u32 frame) -> const BoneTransform& {
    return sampled[static_cast<usize>(frame) * bone_count + bone];
  };

  clip.track_defs.assign(bone_count, TrackDefinition{});

  auto frame_u16_count = 0_u32;
  for (auto bone = 0_u32; bone < bone_count; ++bone) {
    auto& track = clip.track_defs[bone];

    auto translation_min = glm::vec3(std::numeric_limits<f32>::max());
    auto translation_max = glm::vec3(std::numeric_limits<f32>::lowest());
    auto scale_min = std::numeric_limits<f32>::max();
    auto scale_max = std::numeric_limits<f32>::lowest();
    auto rotation_is_static = true;

    const auto& first = bone_frame(bone, 0);
    for (auto frame = 0_u32; frame < frame_count; ++frame) {
      const auto& transform = bone_frame(bone, frame);
      translation_min = glm::min(translation_min, transform.translation());
      translation_max = glm::max(translation_max, transform.translation());
      scale_min = glm::min(scale_min, transform.scale());
      scale_max = glm::max(scale_max, transform.scale());
      rotation_is_static = rotation_is_static && glm::abs(glm::dot(first.rotation, transform.rotation)) > 0.99999f;
    }

    constexpr auto STATIC_EPSILON = 1e-6f;
    const auto translation_extent = translation_max - translation_min;
    track.is_translation_static = glm::all(glm::lessThanEqual(translation_extent, glm::vec3(STATIC_EPSILON)));
    track.is_scale_static = (scale_max - scale_min) <= STATIC_EPSILON;
    track.is_rotation_static = rotation_is_static;

    if (track.is_translation_static) {
      track.translation_range_x = {.start = translation_min.x, .length = 0.f};
      track.translation_range_y = {.start = translation_min.y, .length = 0.f};
      track.translation_range_z = {.start = translation_min.z, .length = 0.f};
    } else {
      track.translation_range_x = FloatRange::from_bounds(translation_min.x, translation_max.x);
      track.translation_range_y = FloatRange::from_bounds(translation_min.y, translation_max.y);
      track.translation_range_z = FloatRange::from_bounds(translation_min.z, translation_max.z);
    }

    track.scale_range = track.is_scale_static ? FloatRange{.start = scale_min, .length = 0.f}
                                              : FloatRange::from_bounds(scale_min, scale_max);

    if (track.is_rotation_static) {
      track.constant_rotation = EncodedQuaternion::encode(first.rotation);
    }

    track.track_read_offset = frame_u16_count;
    frame_u16_count += track.per_frame_u16_count();
  }

  clip.compressed_pose_offsets.resize(frame_count);
  clip.compressed_pose_data.assign(static_cast<usize>(frame_count) * frame_u16_count, 0_u16);

  for (auto frame = 0_u32; frame < frame_count; ++frame) {
    const auto block_offset = static_cast<usize>(frame) * frame_u16_count;
    clip.compressed_pose_offsets[frame] = static_cast<u32>(block_offset);

    for (auto bone = 0_u32; bone < bone_count; ++bone) {
      const auto& track = clip.track_defs[bone];
      const auto& transform = bone_frame(bone, frame);
      auto* cursor = clip.compressed_pose_data.data() + block_offset + track.track_read_offset;

      if (!track.is_rotation_static) {
        const auto encoded = EncodedQuaternion::encode(transform.rotation);
        cursor[0] = encoded.data0;
        cursor[1] = encoded.data1;
        cursor[2] = encoded.data2;
        cursor += 3;
      }

      if (!track.is_translation_static) {
        const auto translation = transform.translation();
        cursor[0] = track.translation_range_x.encode(translation.x);
        cursor[1] = track.translation_range_y.encode(translation.y);
        cursor[2] = track.translation_range_z.encode(translation.z);
        cursor += 3;
      }

      if (!track.is_scale_static) {
        cursor[0] = track.scale_range.encode(transform.scale());
      }
    }
  }
}
} // namespace animation
} // namespace ox
