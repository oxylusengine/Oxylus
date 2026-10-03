#pragma once

#include <vector>

#include "Animation/BoneTransform.hpp"

namespace ox {
struct Skeleton;

struct Pose {
  enum class State : u8 {
    Unset,
    ParentSpace,
    ReferencePose,
  };

  const Skeleton* skeleton = nullptr;
  std::vector<BoneTransform> parent_space_transforms = {};
  // derived on demand, and empty means not built for the current parent-space values
  std::vector<BoneTransform> model_space_transforms = {};
  State state = State::Unset;

  auto reset(this Pose& self, const Skeleton* skeleton, State init_state = State::Unset) -> void;

  auto bone_count(this const Pose& self) -> u32 { return static_cast<u32>(self.parent_space_transforms.size()); }

  auto clear_model_space_transforms(this Pose& self) -> void { self.model_space_transforms.clear(); }
  auto has_model_space_transforms(this const Pose& self) -> bool { return !self.model_space_transforms.empty(); }
  auto calculate_model_space_transforms(this Pose& self) -> void;
};
} // namespace ox
