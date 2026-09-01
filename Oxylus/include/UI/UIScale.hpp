#pragma once

#include <algorithm>
#include <cmath>

#include "Core/Types.hpp"

namespace ox {
constexpr auto UI_SCALE_MIN_MULTIPLIER = 0.5f;
constexpr auto UI_SCALE_MAX_MULTIPLIER = 2.0f;
constexpr auto UI_SCALE_MULTIPLIER_STEP = 0.05f;
constexpr auto UI_SCALE_DEFAULT_MULTIPLIER = 1.0f;

inline auto normalize_ui_scale_multiplier(f32 multiplier) -> f32 {
  if (!std::isfinite(multiplier)) {
    return UI_SCALE_DEFAULT_MULTIPLIER;
  }

  const auto clamped = std::clamp(multiplier, UI_SCALE_MIN_MULTIPLIER, UI_SCALE_MAX_MULTIPLIER);
  // Keep mathematical half steps rounding up even when chained float operations land a few ULPs below them.
  constexpr auto rounding_epsilon = 0.0001f;
  const auto snapped = std::round(clamped / UI_SCALE_MULTIPLIER_STEP + rounding_epsilon) *
                       UI_SCALE_MULTIPLIER_STEP;
  return std::clamp(snapped, UI_SCALE_MIN_MULTIPLIER, UI_SCALE_MAX_MULTIPLIER);
}

inline auto ui_scale_from_display_scale(f32 display_scale) -> f32 {
  if (!std::isfinite(display_scale) || display_scale <= 0.0f) {
    display_scale = 1.0f;
  }

  return normalize_ui_scale_multiplier(display_scale);
}

inline auto migrate_legacy_ui_scale(f32 display_scale, f32 multiplier) -> f32 {
  return normalize_ui_scale_multiplier(
    ui_scale_from_display_scale(display_scale) * normalize_ui_scale_multiplier(multiplier)
  );
}

inline auto calculate_rml_dpi_ratio(f32 ui_scale, f32 viewport_width, i32 surface_width) -> f32 {
  if (!std::isfinite(ui_scale) || ui_scale <= 0.0f) {
    ui_scale = 1.0f;
  }
  if (!std::isfinite(viewport_width) || viewport_width <= 0.0f || surface_width <= 0) {
    return ui_scale;
  }

  return ui_scale * static_cast<f32>(surface_width) / viewport_width;
}
} // namespace ox
