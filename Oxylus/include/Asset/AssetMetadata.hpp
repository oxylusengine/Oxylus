#pragma once

#include <filesystem>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "Core/UUID.hpp"

namespace ox {
// List of file extensions supported by Engine.
enum class FileFormat : u32 {
  Unknown = 0,
  Binary,
  Meta,
  GLB,
  GLTF,
  PNG,
  JPEG,
  DDS,
  JSON,
  KTX2,
  Lua,
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
};

struct ShaderMetadata {};

struct ModelMetadata {
  std::vector<UUID> embedded_texture_uuids = {};
  std::vector<UUID> material_uuids = {};
};

struct TextureMetadata {};

struct MaterialMetadata {
  u32 sampling_mode = 0;
  glm::vec4 albedo_color = {};
  glm::vec3 emissive_color = {};
  f32 roughness_factor = 0.0f;
  f32 metallic_factor = 0.0f;
  u32 alpha_mode = 0;
  f32 alpha_cutoff = 0.1f;
  UUID albedo_texture = {};
  UUID normal_texture = {};
  UUID emissive_texture = {};
  UUID metallic_roughness_texture = {};
  UUID occlusion_texture = {};
};

struct FontMetadata {};

struct AudioMetadata {};

struct ScriptMetadata {};

struct AssetMetadata {
  UUID uuid = {};
  AssetType type = AssetType::None;
  std::variant<
    ShaderMetadata,
    ModelMetadata,
    TextureMetadata,
    MaterialMetadata,
    FontMetadata,
    AudioMetadata,
    ScriptMetadata>
    kind;

  static auto from_file(std::filesystem::path& path) -> option<AssetMetadata>;
  static auto from_string(std::string_view str) -> option<AssetMetadata>;
};

} // namespace ox
