#pragma once

#include <glm/vec2.hpp>

#include "Core/Types.hpp"

namespace ox {
// Which temporal upscaler drives the pass. FSR4 and the other ML upscalers ship as DX12-only signed
// binaries with no shader source, so there is nothing a Vulkan backend can slot in here yet.
enum class UpscalerBackend : u8 {
  None = 0,
  FSR3,
  Count,
};

// Presets from `ffxFsr3UpscalerGetUpscaleRatioFromQualityMode`. NativeAA renders at display
// resolution and runs the upscaler purely as an antialiaser, which is what replaces FXAA.
enum class UpscalerQuality : u8 {
  NativeAA = 0,
  Quality,
  Balanced,
  Performance,
  UltraPerformance,
  Count,
};

struct UpscalerSettings {
  UpscalerBackend backend = UpscalerBackend::None;
  UpscalerQuality quality = UpscalerQuality::Quality;
  // RCAS sharpening strength, 0 skips the sharpen pass entirely
  f32 sharpness = 0.0f;
};

// Per-dimension scaling factor display/render.
auto upscaler_ratio(UpscalerQuality quality) -> f32;

auto upscaler_render_extent(glm::uvec2 display_extent, UpscalerQuality quality) -> glm::uvec2;

// Texture LOD bias the material samplers need so upscaled detail is not lost to undersampling.
auto upscaler_mip_bias(glm::uvec2 render_extent, glm::uvec2 display_extent) -> f32;

// Length of the jitter sequence, `8 * (display / render)^2` truncated, matching the SDK.
auto upscaler_jitter_phase_count(u32 render_width, u32 display_width) -> u32;

// Halton(2,3) offset in [-0.5, 0.5] pixels for a frame index.
auto upscaler_jitter_offset(u32 frame_index, u32 phase_count) -> glm::vec2;
} // namespace ox
