#pragma once

#include <span>

#include "Animation/Fwd.hpp"

namespace ox::animation {
// `bone_mask` is per-bone weight multipliers, empty for none
auto blend(const Pose& source, const Pose& target, f32 weight, std::span<const f32> bone_mask, Pose& out) -> void;
} // namespace ox::animation
