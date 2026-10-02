#pragma once

#include <filesystem>
#include <memory>
#include <simdjson.h>

#include "Asset/AssetFile.hpp"
#include "Asset/AssetImporter.hpp"
#include "Core/UUID.hpp"

namespace ox {
class AssetManager;

// the sidecar format is ResourceCompiler's, so what the editor saves is what the importer reads
using rc::begin_asset_meta;
using rc::end_asset_meta;
using rc::write_material_asset_meta;

// `doc` borrows `contents` and `parser`, so the declaration order is the destruction order.
struct AssetMetaFile {
  simdjson::padded_string contents;
  simdjson::ondemand::parser parser;
  simdjson::simdjson_result<simdjson::ondemand::document> doc;
};

auto read_meta_file(const std::filesystem::path& path) -> std::unique_ptr<AssetMetaFile>;
auto read_meta_file_from_asset(const std::filesystem::path& path) -> std::unique_ptr<AssetMetaFile>;

// writes a live asset back to disk along with its sidecar
auto export_asset(AssetManager& asset_man, const UUID& uuid, const std::filesystem::path& path) -> bool;
} // namespace ox
