#include "Animation/Pose.hpp"

#include "Animation/Skeleton.hpp"
#include "Utils/Log.hpp"

namespace ox {
auto Pose::reset(this Pose& self, const Skeleton* skeleton, const State init_state) -> void {
  ZoneScoped;

  self.skeleton = skeleton;
  self.model_space_transforms.clear();
  self.state = init_state;

  if (skeleton == nullptr) {
    self.parent_space_transforms.clear();
    return;
  }

  if (init_state == State::ReferencePose) {
    self.parent_space_transforms = skeleton->parent_space_reference_pose;
  } else {
    self.parent_space_transforms.assign(skeleton->bone_count(), BoneTransform{});
  }
}

auto Pose::calculate_model_space_transforms(this Pose& self) -> void {
  ZoneScoped;

  if (self.skeleton == nullptr) {
    return;
  }

  const auto count = self.bone_count();
  self.model_space_transforms.resize(count);
  if (count == 0) {
    return;
  }

  self.model_space_transforms[0] = self.parent_space_transforms[0];
  for (auto i = 1_u32; i < count; ++i) {
    const auto parent = self.skeleton->parent_indices[i];
    OX_ASSERT(parent < static_cast<i32>(i));
    self.model_space_transforms[i] = parent < 0 ? self.parent_space_transforms[i]
                                                : self.model_space_transforms[parent] * self.parent_space_transforms[i];
  }
}

} // namespace ox
