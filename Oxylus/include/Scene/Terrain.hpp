#pragma once

#include <expected>

#include "Asset/Texture.hpp"
#include "Scene/SceneGPU.hpp"

namespace ox {
class RenderContext;

struct TerrainMaps {
  vuk::Value<vuk::ImageAttachment> heightmap = {};
  vuk::Value<vuk::ImageAttachment> ridgemap = {};
  vuk::Value<vuk::ImageAttachment> normalmap = {};
  vuk::Value<vuk::ImageAttachment> splatmap = {};
  vuk::Value<vuk::ImageAttachment> patch_minmax = {};
  vuk::Value<vuk::ImageAttachment> height_edit = {};
  vuk::Value<vuk::ImageAttachment> splat_edit = {};
  vuk::Value<vuk::Buffer> region = {};
};

auto terrain_derive_pass(TerrainMaps& maps, const GPU::TerrainDerive& settings, glm::uvec2 dispatch_texels) -> void;
auto terrain_minmax_pass(TerrainMaps& maps, const GPU::TerrainMinMax& settings, glm::uvec2 dispatch_patches) -> void;

struct TerrainBrush {
  bool active = false;
  bool painting = false;
  bool invert = false;

  glm::vec3 ray_origin = {};
  glm::vec3 ray_direction = {};

  GPU::TerrainBrushMode mode = GPU::TerrainBrushMode::Raise;
  f32 radius_world = 32.0f;
  f32 strength = 0.25f;
  f32 falloff = 2.0f;
  f32 flatten_height = 0.5f;
  u32 layer = 0;
};

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

  Texture height_edit = {};
  Texture splat_edit = {};
  bool edits_uninitialized = true;

  TerrainBrush brush = {};

  auto create(this Terrain& self) -> std::expected<void, std::string>;
  auto destroy(this Terrain& self) -> void;

  auto bake(this Terrain& self, RenderContext& render_context) -> void;

  auto clear_edits(this Terrain& self) -> void { self.edits_uninitialized = true; }

  auto is_baked(this const Terrain& self) -> bool { return static_cast<bool>(self.heightmap); }

  auto texel_world_size(this const Terrain& self) -> glm::vec2 { return self.world_size / glm::vec2(self.resolution); }
};
} // namespace ox
