#pragma once

#include <expected>

#include "Asset/Texture.hpp"
#include "Scene/SceneGPU.hpp"

namespace ox {
class RenderContext;
struct Terrain {
  glm::vec3 world_origin = {};
  glm::vec2 world_size = {1024.0f, 1024.0f};
  glm::vec2 height_range = {0.0f, 400.0f};
  glm::uvec2 resolution = {2048, 2048};
  glm::uvec2 patch_count = {64, 64};

  f32 target_edge_pixels = 16.0f;
  f32 max_tessellation = 64.0f;
  f32 layer_tiling = 8.0f;
  f32 triplanar_begin = 0.5f;
  glm::uvec4 layer_material_indices = {};

  GPU::TerrainGenerate generate_settings = {};
  GPU::TerrainDerive derive_settings = {};

  Texture heightmap = {};
  Texture ridgemap = {};
  Texture normalmap = {};
  Texture splatmap = {};
  Texture patch_minmax = {};

  auto create(this Terrain& self) -> std::expected<void, std::string>;
  auto destroy(this Terrain& self) -> void;

  auto bake(this Terrain& self, RenderContext& render_context) -> void;

  auto is_baked(this const Terrain& self) -> bool { return static_cast<bool>(self.heightmap); }

  auto texel_world_size(this const Terrain& self) -> glm::vec2 { return self.world_size / glm::vec2(self.resolution); }
};
} // namespace ox
