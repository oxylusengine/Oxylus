#pragma once

#include <array>
#include <filesystem>
#include <span>
#include <zpp_bits.h>

#include "Asset/Material.hpp"
#include "Asset/ShaderFeature.hpp"
#include "Core/Option.hpp"
#include "Core/Types.hpp"
#include "Core/UUID.hpp"
#include "Scene/MeshGPU.hpp"

namespace ox {
struct PackedUUID {
  std::array<u8, 16> bytes = {};

  static auto pack(const UUID& uuid) -> PackedUUID;
  auto unpack(this const PackedUUID& self) -> UUID;
};

enum class AssetType : u32 {
  None = 0,
  Shader,
  Model,
  Texture,
  Material,
  Font,
  Scene,
  Audio,
  Script,
  Terrain,
  ParticleSystem,
};

struct NoneAsset {
  using serialize_id = zpp::bits::serialization_id<AssetType::None>;
};

enum class ShaderStage : u32 {
  None = 0,
  Vertex,
  Hull,
  Domain,
  Geometry,
  Fragment,
  Compute,
  RayGeneration,
  Intersection,
  AnyHit,
  ClosestHit,
  Miss,
  Callable,
  Mesh,
  Amplification,
};

struct ShaderEntryPointData {
  std::string name = {};
  ShaderStage shader_stage = ShaderStage::None;
  std::vector<u32> spirv = {};
};

struct ShaderPipelineData {
  using serialize_id = zpp::bits::serialization_id<AssetType::Shader>;

  std::string module_name = "";
  std::vector<ShaderEntryPointData> entry_points = {};
  ShaderFeatureFlag required_features = ShaderFeatureFlag::None;
};

struct TextureMipData {
  u32 width = 0;
  u32 height = 0;
  std::vector<u8> pixels = {};
};

struct TextureData {
  using serialize_id = zpp::bits::serialization_id<AssetType::Texture>;

  std::string name = {};
  u32 vk_format = 0;
  u32 width = 0;
  u32 height = 0;
  u32 layer_count = 1;
  std::vector<TextureMipData> mips = {};
};

enum class ModelLightType : u32 {
  Directional = 0,
  Spot,
  Point,
};

struct ModelData {
  using serialize_id = zpp::bits::serialization_id<AssetType::Model>;

  constexpr static auto INVALID_INDEX = ~0_u32;

  struct Mesh {
    std::string name = {};
    u64 vertex_positions_offset = 0;
    u64 vertex_normals_offset = 0;
    u64 texture_coords_offset = 0;
    u32 vertex_count = 0;
    u32 lod_count = 0;
    u64 lod_metadata_offset = 0;
    bool has_texture_coords = false;
    std::array<f32, 3> bounds_center = {};
    std::array<f32, 3> bounds_extent = {};
    std::array<GPU::MeshLOD, GPU::Mesh::MAX_LODS> lods = {};
    std::vector<u8> blob = {};
    // LOD0 triangles, un-quantized, so physics can build a shape. Positions are xyz triples.
    std::vector<f32> collision_positions = {};
    std::vector<u32> collision_indices = {};
    u32 material_index = INVALID_INDEX;
  };

  struct Material {
    std::string name = {};
    PackedUUID uuid = {};
    std::array<f32, 4> albedo_color = {1.0f, 1.0f, 1.0f, 1.0f};
    std::array<f32, 3> emissive_color = {0.0f, 0.0f, 0.0f};
    std::array<f32, 2> uv_size = {1.0f, 1.0f};
    std::array<f32, 2> uv_offset = {0.0f, 0.0f};
    f32 roughness_factor = 1.0f;
    f32 metallic_factor = 0.0f;
    f32 alpha_cutoff = 0.1f;
    f32 normal_scale = 1.0f;
    f32 occlusion_strength = 1.0f;
    AlphaMode alpha_mode = AlphaMode::Opaque;
    SamplingMode sampling_mode = SamplingMode::LinearRepeated;
    bool flip_normal_y = false;
    u32 albedo_texture_index = INVALID_INDEX;
    u32 normal_texture_index = INVALID_INDEX;
    u32 emissive_texture_index = INVALID_INDEX;
    u32 metallic_roughness_texture_index = INVALID_INDEX;
    u32 occlusion_texture_index = INVALID_INDEX;
  };

  // The compiler leaves `uuid` null; the editor stamps it before the pack is written, so the engine
  // never has to resolve a sub-asset by path or index.
  struct Texture {
    std::string name = {};
    PackedUUID uuid = {};
    bool is_srgb = true;
  };

  struct Light {
    std::string name = {};
    ModelLightType type = ModelLightType::Directional;
    std::array<f32, 3> color = {1.0f, 1.0f, 1.0f};
    f32 intensity = 1.0f;
    bool has_range = false;
    f32 range = 0.0f;
    bool has_inner_cone_angle = false;
    f32 inner_cone_angle = 0.0f;
    bool has_outer_cone_angle = false;
    f32 outer_cone_angle = 0.0f;
  };

  struct MeshGroup {
    std::string name = {};
    std::array<f32, 3> translation = {0.0f, 0.0f, 0.0f};
    std::array<f32, 4> rotation = {0.0f, 0.0f, 0.0f, 1.0f};
    std::array<f32, 3> scale = {1.0f, 1.0f, 1.0f};
    std::vector<u32> child_indices = {};
    std::vector<u32> mesh_indices = {};
    std::vector<u32> light_indices = {};
  };

  std::string name = {};
  std::vector<Mesh> meshes = {};
  std::vector<Material> materials = {};
  std::vector<Texture> textures = {};
  std::vector<Light> lights = {};
  std::vector<MeshGroup> mesh_groups = {};
  u32 default_scene_index = 0;
};

// Resolves a compiled material's texture indices against the model's texture table. Shared, so the
// engine's model load and the editor's sidecar writer cannot drift apart on what a material means.
auto to_material(const ModelData::Material& src, std::span<const UUID> textures) -> Material;

static_assert(sizeof(GPU::Mesh) == 64);
static_assert(sizeof(GPU::MeshLOD) == 64);
static_assert(sizeof(GPU::Meshlet) == 16);
static_assert(sizeof(GPU::MeshletBounds) == 16);
static_assert(GPU::Mesh::MAX_LODS == 8);

struct AssetFileEntry {
  PackedUUID uuid = {};
  AssetType type = AssetType::None;
  std::variant<NoneAsset, ShaderPipelineData, TextureData, ModelData> data;

  constexpr static auto serialize(auto& archive, auto& self) -> zpp::bits::errc {
    if constexpr (std::remove_cvref_t<decltype(archive)>::kind() == zpp::bits::kind::out) {
      if (auto err = archive(self.uuid, self.type); zpp::bits::failure(err))
        return err;
      return std::visit([&](auto& v) { return archive(v); }, self.data);
    } else {
      if (auto err = archive(self.uuid, self.type); zpp::bits::failure(err))
        return err;
      return archive(zpp::bits::known_id(self.type, self.data));
    }
  }
};

enum class AssetFileFlags : u32 {
  None = 0,
};
consteval void enable_bitmask(AssetFileFlags);

struct AssetFileHeader {
  static constexpr auto SIGNATURE = 0x4352584F_u32;
  static constexpr auto VERSION = 3_u16;

  u32 magic = SIGNATURE; // "OXRC"
  u16 version = VERSION;
  AssetFileFlags flags = AssetFileFlags::None;
};

struct AssetFile {
  AssetFileFlags flags = AssetFileFlags::None;
  std::vector<AssetFileEntry> entries = {};

  static auto unpack(const std::filesystem::path& path) -> option<AssetFile>;
  auto pack(this AssetFile& self, const std::filesystem::path& path) -> bool;
  auto add_entry(this AssetFile& self, ShaderPipelineData&& entry, const PackedUUID& uuid = {}) -> void;
  auto add_entry(this AssetFile& self, TextureData&& entry, const PackedUUID& uuid = {}) -> void;
  auto add_entry(this AssetFile& self, ModelData&& entry, const PackedUUID& uuid = {}) -> void;
};
} // namespace ox
