#pragma once

#include <ankerl/unordered_dense.h>
#include <span>
#include <string>
#include <vector>

#include "Animation/BoneTransform.hpp"
#include "Core/Option.hpp"

namespace ox {
struct Skeleton {
  std::vector<std::string> bone_names = {};
  std::vector<u64> bone_name_hashes = {};
  // -1 for the root, and `parent_indices[i] < i` always holds, which is what lets model-space
  // accumulation be a single forward loop with no recursion
  std::vector<i32> parent_indices = {};
  std::vector<BoneTransform> parent_space_reference_pose = {};
  std::vector<BoneTransform> model_space_reference_pose = {};
  std::vector<BoneTransform> inverse_bind_pose = {};
  ankerl::unordered_dense::map<u64, u32> bone_index_lut = {};

  auto bone_count(this const Skeleton& self) -> u32 { return static_cast<u32>(self.parent_indices.size()); }

  auto find_bone(this const Skeleton& self, std::string_view name) -> option<u32>;

  // false if the parents-before-children invariant is violated
  auto finalize(this Skeleton& self) -> bool;

  auto is_valid(this const Skeleton& self) -> bool;
};
} // namespace ox
