#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include "Asset/AssetFile.hpp"
#include "Core/Option.hpp"
#include "Core/Types.hpp"

namespace ox {
// the asset registry a shipped game can't build for itself, since reading `.oxasset` sidecars is editor-only. The
// editor's export writes it into `VFS::COOKED_SUBDIR` next to the cooked packs, `AssetManager::init` reads it
struct AssetManifest {
  static constexpr auto FILE_NAME = "assets.oxmanifest";

  struct Header {
    static constexpr auto SIGNATURE = 0x464D584F_u32;
    static constexpr auto VERSION = 1_u16;

    u32 magic = SIGNATURE; // "OXMF"
    u16 version = VERSION;
  };

  struct Entry {
    PackedUUID uuid = {};
    AssetType type = AssetType::None;
    // virtual paths, see `Asset::path` and `Asset::source_path`
    std::string path = {};
    std::string source_path = {};
  };

  // a material's definition lives in its editor sidecar, so it travels here instead
  struct MaterialEntry {
    PackedUUID uuid = {};
    std::array<f32, 4> albedo_color = {1.0f, 1.0f, 1.0f, 1.0f};
    std::array<f32, 2> uv_size = {1.0f, 1.0f};
    std::array<f32, 2> uv_offset = {0.0f, 0.0f};
    std::array<f32, 3> emissive_color = {0.0f, 0.0f, 0.0f};
    f32 roughness_factor = 1.0f;
    f32 metallic_factor = 0.0f;
    f32 normal_scale = 1.0f;
    f32 occlusion_strength = 1.0f;
    AlphaMode alpha_mode = AlphaMode::Opaque;
    f32 alpha_cutoff = 0.1f;
    SamplingMode sampling_mode = SamplingMode::LinearRepeated;
    bool flip_normal_y = false;
    PackedUUID albedo_texture = {};
    PackedUUID normal_texture = {};
    PackedUUID emissive_texture = {};
    PackedUUID metallic_roughness_texture = {};
    PackedUUID occlusion_texture = {};

    static auto pack(const UUID& uuid, const Material& material) -> MaterialEntry;
    auto unpack(this const MaterialEntry& self) -> Material;
  };

  std::vector<Entry> assets = {};
  std::vector<MaterialEntry> materials = {};

  static auto read(const std::filesystem::path& path) -> option<AssetManifest>;
  auto write(this const AssetManifest& self, const std::filesystem::path& path) -> bool;
};
} // namespace ox
