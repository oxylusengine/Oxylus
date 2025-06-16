#pragma once
#include "Core/UUID.hpp"

namespace ox {
enum class AlphaMode : u32 {
  Opaque = 0,
  Mask,
  Blend,
};

enum class SamplingMode : u32 {
  LinearRepeated = 0,
  LinearClamped = 1,

  NearestRepeated = 2,
  NearestClamped = 3,

  LinearRepeatedAnisotropy = 4,
};

enum class MaterialID : u64 { Invalid = std::numeric_limits<u64>::max() };
struct Material {
  glm::vec4 albedo_color = {1.0f, 1.0f, 1.0f, 1.0f};
  glm::vec2 uv_size = {1.0f, 1.0f};
  glm::vec2 uv_offset = {0.0f, 0.0f};
  glm::vec3 emissive_color = {0.0f, 0.0f, 0.0f};
  f32 roughness_factor = 0.0f;
  f32 metallic_factor = 0.0f;
  AlphaMode alpha_mode = AlphaMode::Opaque;
  f32 alpha_cutoff = 0.0f;
  SamplingMode sampling_mode = SamplingMode::LinearRepeated;
  UUID albedo_texture = {};
  UUID normal_texture = {};
  UUID emissive_texture = {};
  UUID metallic_roughness_texture = {};
  UUID occlusion_texture = {};
};
} // namespace ox
