#include "Animation/Blender.hpp"

#include "Animation/Pose.hpp"
#include "Animation/Skeleton.hpp"

namespace ox::animation {
auto blend(const Pose& source, const Pose& target, const f32 weight, const std::span<const f32> bone_mask, Pose& out)
  -> void {
  ZoneScoped;

  const auto count = ox::min(source.bone_count(), target.bone_count());
  if (count == 0) {
    return;
  }

  out.skeleton = source.skeleton;
  out.parent_space_transforms.resize(count);
  out.clear_model_space_transforms();
  out.state = Pose::State::ParentSpace;

  for (auto i = 0_u32; i < count; ++i) {
    const auto bone_weight = bone_mask.empty() ? weight : weight * bone_mask[i];

    if (bone_weight <= 0.f) {
      out.parent_space_transforms[i] = source.parent_space_transforms[i];
    } else if (bone_weight >= 1.f) {
      out.parent_space_transforms[i] = target.parent_space_transforms[i];
    } else {
      out.parent_space_transforms[i] = slerp(
        source.parent_space_transforms[i],
        target.parent_space_transforms[i],
        bone_weight
      );
    }
  }
}
} // namespace ox::animation
