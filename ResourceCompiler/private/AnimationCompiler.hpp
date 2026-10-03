#pragma once

#include <fastgltf/types.hpp>

#include "Animation/BoneTransform.hpp"
#include "Animation/Skeleton.hpp"
#include "ResourceCompiler.hpp"

namespace ox::rc {
struct SkinBuildData {
  Skeleton skeleton = {};
  // glTF skin joint slot -> bone index
  std::vector<u32> joint_to_bone = {};
  // bone index -> glTF node index
  std::vector<usize> bone_to_node = {};
  // chain from the scene root down to the parent of the skin's root joints, because glTF ignores
  // the skinned mesh node's own transform yet the joints still live under whatever ancestors the
  // exporter emitted, and those have to be folded back in somewhere
  BoneTransform root_prefix = {};
};

auto build_gltf_skeleton(Session& session, const fastgltf::Asset& gltf_asset, const fastgltf::Skin& gltf_skin)
  -> option<SkinBuildData>;

auto build_gltf_animation(
  const fastgltf::Asset& gltf_asset, const fastgltf::Animation& gltf_animation, const SkinBuildData& skin
) -> option<ModelData::Animation>;
} // namespace ox::rc
