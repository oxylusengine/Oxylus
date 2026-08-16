#pragma once

#include <expected>
#include <vector>

#include "Asset/Texture.hpp"
#include "Scene/SceneGPU.hpp"

namespace ox {
class RenderContext;

// Jolt culls the height field one block of `block * block` quads at a time, and wants at least two
// blocks per side.
constexpr u32 TERRAIN_COLLISION_BLOCK_SIZE = 4;
constexpr u32 TERRAIN_COLLISION_MIN_SAMPLES = 2 * TERRAIN_COLLISION_BLOCK_SIZE;
constexpr u32 TERRAIN_COLLISION_MAX_SAMPLES = 4096;

// Rounds a requested collider resolution to a sample count Jolt accepts.
auto terrain_collision_sample_count(u32 requested) -> u32;

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

auto terrain_generate_pass(TerrainMaps& maps, const GPU::TerrainGenerate& settings, glm::uvec2 dispatch_texels) -> void;
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
  // Raise and Noise displace the surface by this many world units per second under the cursor.
  f32 height_rate = 4.0f;
  // Smooth, Flatten and Paint Layer converge on their target by this fraction per second.
  f32 blend_rate = 3.0f;
  f32 falloff = 1.0f;
  // World-space height Flatten pulls toward.
  f32 flatten_height_world = 0.0f;
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
  glm::uvec4 layer_material_indices = glm::uvec4(GPU::TERRAIN_INVALID_LAYER_MATERIAL);

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

  // The heightmap only ever exists on the GPU, so the collider is built from a CPU mirror
  // of it, resampled down to `collision_resolution`. `Scene` owns the Jolt body made out of it.
  bool collision_enabled = true;
  u32 collision_resolution = 256;
  f32 collision_friction = 0.5f;
  f32 collision_restitution = 0.0f;

  // World-space heights in row major order, `collision_sample_count` per side.
  std::vector<f32> collision_heights = {};
  u32 collision_sample_count = 0;
  bool collision_dirty = true;

  auto create(this Terrain& self) -> std::expected<void, std::string>;
  auto destroy(this Terrain& self) -> void;

  auto bake(this Terrain& self, RenderContext& render_context) -> void;

  auto clone_edits_from(this Terrain& self, const Terrain& src, RenderContext& render_context) -> void;

  // Stalls on the GPU: it submits a copy of the whole heightmap and waits for it.
  auto download_collision_heights(this Terrain& self, RenderContext& render_context) -> void;

  auto clear_edits(this Terrain& self) -> void { self.edits_uninitialized = true; }

  auto is_baked(this const Terrain& self) -> bool { return static_cast<bool>(self.heightmap); }

  auto texel_world_size(this const Terrain& self) -> glm::vec2 { return self.world_size / glm::vec2(self.resolution); }

  auto world_min(this const Terrain& self) -> glm::vec2 {
    return glm::vec2(self.world_origin.x, self.world_origin.z) - self.world_size * 0.5f;
  }

  auto base_height(this const Terrain& self) -> f32 { return self.world_origin.y + self.height_range.x; }

  auto height_scale(this const Terrain& self) -> f32 { return self.height_range.y - self.height_range.x; }
};
} // namespace ox
