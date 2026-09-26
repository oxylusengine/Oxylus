#pragma once

#include <string_view>

#include "Core/Types.hpp"

namespace ox {
enum class Easing : u8 {
  Linear = 0,
  Step,
  SmoothStep,
  InQuad,
  OutQuad,
  InOutQuad,
  InCubic,
  OutCubic,
  InOutCubic,
  InExpo,
  OutExpo,
  InOutExpo,
  InBack,
  OutBack,
  InOutBack,
  Count,
};

auto ease(Easing kind, f32 t) -> f32;
auto easing_name(Easing kind) -> std::string_view;
} // namespace ox
