#pragma once

#include <filesystem>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <variant>

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
  Meta,
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

struct MaterialMetadata;
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

struct SceneMetadata {
  std::string name = {};
};

struct AudioMetadata {};

struct ScriptMetadata {};

using AssetVariant = std::variant<
  ShaderMetadata,
  ModelMetadata,
  TextureMetadata,
  MaterialMetadata,
  FontMetadata,
  SceneMetadata,
  AudioMetadata,
  ScriptMetadata>;

struct AssetMetadata {
  UUID uuid = {};
  AssetType type = AssetType::None;
  AssetVariant variant = {};

  static auto to_file_format(const std::filesystem::path& path) -> FileFormat;
  static auto to_asset_type_sv(AssetType type) -> std::string_view;

  static auto from_file(const std::filesystem::path& path) -> option<AssetMetadata>;
  static auto from_string(std::string_view str, usize padded_capacity) -> option<AssetMetadata>;
  auto to_string(this AssetMetadata&) -> std::string;
  auto to_file(this AssetMetadata&, const std::filesystem::path& path) -> void;
};

} // namespace ox
