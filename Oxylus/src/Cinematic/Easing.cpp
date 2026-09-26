#include "Cinematic/Easing.hpp"

#include <cmath>
#include <glm/common.hpp>

namespace ox {
constexpr static auto BACK_C1 = 1.70158f;
constexpr static auto BACK_C2 = BACK_C1 * 1.525f;
constexpr static auto BACK_C3 = BACK_C1 + 1.0f;

auto ease(const Easing kind, f32 t) -> f32 {
  t = glm::clamp(t, 0.0f, 1.0f);

  switch (kind) {
    case Easing::Linear    : return t;
    case Easing::Step      : return t < 1.0f ? 0.0f : 1.0f;
    case Easing::SmoothStep: return t * t * (3.0f - 2.0f * t);
    case Easing::InQuad    : return t * t;
    case Easing::OutQuad   : return 1.0f - (1.0f - t) * (1.0f - t);
    case Easing::InOutQuad : return t < 0.5f ? 2.0f * t * t : 1.0f - 0.5f * (-2.0f * t + 2.0f) * (-2.0f * t + 2.0f);
    case Easing::InCubic   : return t * t * t;
    case Easing::OutCubic  : {
      const auto u = 1.0f - t;
      return 1.0f - u * u * u;
    }
    case Easing::InOutCubic: {
      if (t < 0.5f) {
        return 4.0f * t * t * t;
      }
      const auto u = -2.0f * t + 2.0f;
      return 1.0f - 0.5f * u * u * u;
    }
    case Easing::InExpo   : return t <= 0.0f ? 0.0f : std::pow(2.0f, 10.0f * t - 10.0f);
    case Easing::OutExpo  : return t >= 1.0f ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * t);
    case Easing::InOutExpo: {
      if (t <= 0.0f) {
        return 0.0f;
      }
      if (t >= 1.0f) {
        return 1.0f;
      }
      return t < 0.5f ? 0.5f * std::pow(2.0f, 20.0f * t - 10.0f) : 1.0f - 0.5f * std::pow(2.0f, -20.0f * t + 10.0f);
    }
    case Easing::InBack : return BACK_C3 * t * t * t - BACK_C1 * t * t;
    case Easing::OutBack: {
      const auto u = t - 1.0f;
      return 1.0f + BACK_C3 * u * u * u + BACK_C1 * u * u;
    }
    case Easing::InOutBack: {
      if (t < 0.5f) {
        const auto u = 2.0f * t;
        return 0.5f * (u * u * ((BACK_C2 + 1.0f) * u - BACK_C2));
      }
      const auto u = 2.0f * t - 2.0f;
      return 0.5f * (u * u * ((BACK_C2 + 1.0f) * u + BACK_C2) + 2.0f);
    }
    case Easing::Count:;
  }

  return t;
}

auto easing_name(const Easing kind) -> std::string_view {
  switch (kind) {
    case Easing::Linear    : return "Linear";
    case Easing::Step      : return "Step";
    case Easing::SmoothStep: return "Smooth Step";
    case Easing::InQuad    : return "In Quad";
    case Easing::OutQuad   : return "Out Quad";
    case Easing::InOutQuad : return "In Out Quad";
    case Easing::InCubic   : return "In Cubic";
    case Easing::OutCubic  : return "Out Cubic";
    case Easing::InOutCubic: return "In Out Cubic";
    case Easing::InExpo    : return "In Expo";
    case Easing::OutExpo   : return "Out Expo";
    case Easing::InOutExpo : return "In Out Expo";
    case Easing::InBack    : return "In Back";
    case Easing::OutBack   : return "Out Back";
    case Easing::InOutBack : return "In Out Back";
    case Easing::Count     : return {};
  }

  return {};
}
} // namespace ox
