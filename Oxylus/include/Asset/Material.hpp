#pragma once
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "Core/UUID.hpp"

namespace ox {
enum class AlphaMode : u32 {
  Opaque = 0,
  Mask,
  Blend,
};

enum class ShadingModel : u32 {
  Standard = 0,
  Water = 1,
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
  f32 roughness_factor = 1.0f;
  f32 metallic_factor = 0.0f;
  // glTF's normalTexture.scale and occlusionTexture.strength, both a no-op at 1.0
  f32 normal_scale = 1.0f;
  f32 occlusion_strength = 1.0f;
  AlphaMode alpha_mode = AlphaMode::Opaque;
  f32 alpha_cutoff = 0.1f;
  SamplingMode sampling_mode = SamplingMode::LinearRepeated;
  bool flip_normal_y = false;
  bool double_sided = false;
  f32 transmission_factor = 0.0f;
  f32 ior = 1.5f;
  f32 thickness_factor = 0.0f;
  glm::vec3 attenuation_color = {1.0f, 1.0f, 1.0f};
  // 0 is infinite
  f32 attenuation_distance = 0.0f;
  ShadingModel shading_model = ShadingModel::Standard;
  glm::vec3 water_scatter_color = {0.02f, 0.09f, 0.08f};
  glm::vec2 water_flow = {0.03f, 0.01f};
  f32 water_detail_scale = 0.37f;
  f32 water_shore_fade = 0.5f;
  UUID albedo_texture = {};
  UUID normal_texture = {};
  UUID emissive_texture = {};
  UUID metallic_roughness_texture = {};
  UUID occlusion_texture = {};
  // factor multiplies the R channel, linear
  UUID transmission_texture = {};
  // factor multiplies the G channel, linear
  UUID thickness_texture = {};
};

} // namespace ox
