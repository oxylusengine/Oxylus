#include "Render/Upscaler.hpp"

#include <algorithm>
#include <cmath>

namespace ox {
// van der Corput / Halton radical inverse, kept in the SDK's iterative form so the sequence matches
// the one FSR's accumulation was tuned against
static auto halton(u32 index, u32 base) -> f32 {
  auto fraction = 1.0f;
  auto result = 0.0f;

  for (auto current = index; current > 0; current /= base) {
    fraction /= static_cast<f32>(base);
    result += fraction * static_cast<f32>(current % base);
  }

  return result;
}

auto upscaler_ratio(const UpscalerQuality quality) -> f32 {
  switch (quality) {
    case UpscalerQuality::NativeAA        : return 1.0f;
    case UpscalerQuality::Quality         : return 1.5f;
    case UpscalerQuality::Balanced        : return 1.7f;
    case UpscalerQuality::Performance     : return 2.0f;
    case UpscalerQuality::UltraPerformance: return 3.0f;
    case UpscalerQuality::Count           : break;
  }

  return 1.0f;
}

auto upscaler_render_extent(const glm::uvec2 display_extent, const UpscalerQuality quality) -> glm::uvec2 {
  const auto ratio = upscaler_ratio(quality);

  return {
    std::max(static_cast<u32>(static_cast<f32>(display_extent.x) / ratio), 1u),
    std::max(static_cast<u32>(static_cast<f32>(display_extent.y) / ratio), 1u),
  };
}

auto upscaler_mip_bias(const glm::uvec2 render_extent, const glm::uvec2 display_extent) -> f32 {
  if (render_extent.x == 0 || display_extent.x == 0) {
    return 0.0f;
  }

  return std::log2(static_cast<f32>(render_extent.x) / static_cast<f32>(display_extent.x)) - 1.0f;
}

auto upscaler_jitter_phase_count(const u32 render_width, const u32 display_width) -> u32 {
  if (render_width == 0) {
    return 1;
  }

  const auto ratio = static_cast<f32>(display_width) / static_cast<f32>(render_width);

  return std::max(static_cast<u32>(8.0f * ratio * ratio), 1u);
}

auto upscaler_jitter_offset(const u32 frame_index, const u32 phase_count) -> glm::vec2 {
  if (phase_count == 0) {
    return {};
  }

  const auto index = (frame_index % phase_count) + 1;

  return {halton(index, 2) - 0.5f, halton(index, 3) - 0.5f};
}
} // namespace ox
