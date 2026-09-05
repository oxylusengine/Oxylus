#pragma once

#include <filesystem>
#include <memory>
#include <simdjson.h>

#include "Asset/AssetFile.hpp"
#include "Core/UUID.hpp"

namespace ox {
class AssetManager;
struct JsonWriter;
struct Material;

// Project file extensions the editor knows how to turn into assets.
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
  OXTERRAIN,
  OXPARTICLE,
  WAV,
  MP3,
  FLAC,
  OGG,
};

// `doc` borrows `contents` and `parser`, so the declaration order is the destruction order.
struct AssetMetaFile {
  simdjson::padded_string contents;
  simdjson::ondemand::parser parser;
  simdjson::simdjson_result<simdjson::ondemand::document> doc;
};

auto to_asset_file_type(const std::filesystem::path& path) -> AssetFileType;
auto meta_file_path(const std::filesystem::path& path) -> std::filesystem::path;
auto owns_meta_file(const std::filesystem::path& path) -> bool;
auto read_meta_file(const std::filesystem::path& path) -> std::unique_ptr<AssetMetaFile>;
auto read_meta_file_from_asset(const std::filesystem::path& path) -> std::unique_ptr<AssetMetaFile>;

auto register_asset_from_meta(AssetManager& asset_man, const std::filesystem::path& meta_path) -> UUID;
auto export_asset(AssetManager& asset_man, const UUID& uuid, const std::filesystem::path& path) -> bool;

// Sidecar primitives, shared with AssetImporter. `begin_asset_meta` writes `uuid` then `type`, and
// the document is read back with simdjson ondemand, so anything appended must be read in the same
// order it is written.
auto begin_asset_meta(JsonWriter& writer, const UUID& uuid, AssetType type) -> void;
auto end_asset_meta(JsonWriter& writer, const std::filesystem::path& path) -> bool;
auto write_material_asset_meta(JsonWriter& writer, const UUID& uuid, const Material& material) -> bool;
auto read_material_asset_meta(simdjson::ondemand::value json, Material& material) -> bool;
} // namespace ox
