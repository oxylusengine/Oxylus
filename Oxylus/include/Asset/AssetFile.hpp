#pragma once

#include <filesystem>
#include <vuk/Types.hpp>
#include <vuk/runtime/vk/VkTypes.hpp>
#include <zpp_bits.h>

#include "Core/Option.hpp"
#include "Core/Types.hpp"
#include "Core/UUID.hpp"

namespace ox {
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
};
auto AssetType_to_string_view(AssetType type) -> std::string_view;

// List of file extensions supported by Engine.
enum class AssetFileType : u32 {
  None = 0,
  Binary,
  Meta,
  GLB,
  GLTF,
  PNG,
  JPEG,
  DDS,
  JSON,
  KTX2,
  LUA,
};

struct NoneAsset {
  using serialize_id = zpp::bits::serialization_id<AssetType::None>;
};

struct ShaderEntryPointData {
  std::string name = {};
  u32 shader_stage = {};
  std::vector<u32> spirv = {};
};

struct ShaderPipelineData {
  using serialize_id = zpp::bits::serialization_id<AssetType::Shader>;

  std::string module_name = "";
  std::vector<ShaderEntryPointData> entry_points = {};
  bool bindless = false;
};

struct TextureMipData {
  u32 width = 0;
  u32 height = 0;
  std::vector<u8> pixels = {};
};

struct TextureData {
  using serialize_id = zpp::bits::serialization_id<AssetType::Texture>;

  std::string name = {};
  vuk::Format format = {};
  u32 width = 0;
  u32 height = 0;
  u32 layer_count = 1;
  std::vector<TextureMipData> mips = {};
};

struct ModelData {
  using serialize_id = zpp::bits::serialization_id<AssetType::Model>;

  struct Meshlet {
    u32 indirect_vertex_index_offset = 0;
    u32 local_triangle_index_offset = 0;
    u32 vertex_count = 0;
    u32 triangle_count = 0;
  };

  struct MeshletBounds {
    u16 aabb_center[3] = {}; // quantized half-float
    u16 aabb_extent[3] = {}; // quantized half-float
    i8 cone_axis_xy[2] = {};
    i8 cone_axis_z = 0;
    i8 cone_cutoff = 0;
  };

  struct MeshLOD {
    f32 error = 0.f;
    std::vector<u8> indices = {};                 // raw u32[]
    std::vector<u8> meshlets = {};                // raw Meshlet[]
    std::vector<u8> meshlet_bounds = {};          // raw MeshletBounds[]
    std::vector<u8> local_triangle_indices = {};  // raw u8[]
    std::vector<u8> indirect_vertex_indices = {}; // raw u32[]
  };

  struct Mesh {
    std::string name = {};
    std::vector<u8> quantized_positions = {}; // raw glm::u16vec4[vertex_count] (half-float xyz, w unused)
    std::vector<u8> quantized_normals = {};   // raw u32[vertex_count] (packed 10:10:10:2 snorm)
    std::vector<u8> quantized_texcoords = {}; // raw glm::u16vec2[vertex_count] (half-float)
    std::vector<MeshLOD> lods = {};
    f32 bounds_center[3] = {};
    f32 bounds_extent[3] = {};
    option<u32> material_index = nullopt;
  };

  struct Material {
    std::string name = {};
    f32 albedo_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    f32 emissive_color[3] = {0.0f, 0.0f, 0.0f};
    f32 roughness_factor = 1.0f;
    f32 metallic_factor = 0.0f;
    f32 alpha_cutoff = 0.5f;
    u32 alpha_mode = 0;
    f32 uv_offset[2] = {0.0f, 0.0f};
    f32 uv_scale[2] = {1.0f, 1.0f};
    option<u32> albedo_texture_index = nullopt;
    option<u32> normal_texture_index = nullopt;
    option<u32> emissive_texture_index = nullopt;
    option<u32> metallic_roughness_texture_index = nullopt;
    option<u32> occlusion_texture_index = nullopt;
  };

  struct Light {
    std::string name = {};
    u32 type = 0;
    f32 color[3] = {1.0f, 1.0f, 1.0f};
    f32 intensity = 1.0f;
    option<f32> range = nullopt;
    option<f32> inner_cone_angle = nullopt;
    option<f32> outer_cone_angle = nullopt;
  };

  struct MeshGroup {
    std::string name = {};
    f32 translation[3] = {0.0f, 0.0f, 0.0f};
    f32 rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    f32 scale[3] = {1.0f, 1.0f, 1.0f};
    std::vector<u32> child_indices = {};
    std::vector<u32> light_indices = {};
    std::vector<u32> mesh_indices = {};
  };

  std::string name = {};
  std::vector<Mesh> meshes = {};
  std::vector<Material> materials = {};
  std::vector<TextureData> textures = {};
  std::vector<Light> lights = {};
  std::vector<MeshGroup> mesh_groups = {};
};

struct AssetFileEntry {
  UUID uuid = {};
  AssetType type = AssetType::None;
  std::variant<NoneAsset, ShaderPipelineData, TextureData, ModelData> data;

  constexpr static auto serialize(auto& archive, auto& self)
    requires(std::remove_cvref_t<decltype(archive)>::kind() == zpp::bits::kind::in)
  {
    if (auto err = archive(self.type); zpp::bits::failure(err))
      return err;
    return archive(zpp::bits::known_id(self.type, self.data));
  }

  constexpr static auto serialize(auto& archive, auto& self)
    requires(std::remove_cvref_t<decltype(archive)>::kind() == zpp::bits::kind::out)
  {
    auto _ = archive(self.type);
    return std::visit([&](auto& v) { return archive(v); }, self.data);
  }
};

enum class AssetFileFlags : u32 {
  None = 0,
};
consteval void enable_bitmask(AssetFileFlags);

struct AssetFileHeader {
  static constexpr auto SIGNATURE = 0x4352584F_u32;
  static constexpr auto VERSION = 1_u16;

  u32 magic = SIGNATURE; // "OXRC"
  u16 version = VERSION;
  AssetFileFlags flags = AssetFileFlags::None;
  u32 entry_count = 0;
};

struct AssetFile {
  AssetFileFlags flags = AssetFileFlags::None;
  std::vector<AssetFileEntry> entries = {};

  static auto unpack(const std::filesystem::path& path) -> option<AssetFile>;
  auto pack(this AssetFile& self, const std::filesystem::path& path) -> bool;
  auto add_entry(this AssetFile& self, ShaderPipelineData&& entry) -> void;
  auto add_entry(this AssetFile& self, TextureData&& entry) -> void;
  auto add_entry(this AssetFile& self, ModelData&& entry) -> void;
};
} // namespace ox
