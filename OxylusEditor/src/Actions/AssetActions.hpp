#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

#include "Asset/AssetManager.hpp"
#include "Core/UUID.hpp"

namespace ox {
class Editor;
struct CreatedAsset;

auto expected_asset_type(std::string_view field_name) -> AssetType;

// the file is imported into the editor's cache and registered before this returns, which can take a while for models
auto import_file(Editor& editor, const std::filesystem::path& path) -> std::expected<UUID, std::string>;
// `path` is a file inside the asset directory, the type's extension is added when it has none
auto create_asset(Editor& editor, AssetType type, const std::filesystem::path& path)
  -> std::expected<CreatedAsset, std::string>;
// writes the asset and its sidecar to the file it was created or imported from, returns that file
auto save_asset(Editor& editor, const UUID& uuid) -> std::expected<std::filesystem::path, std::string>;

// the material as the sidecar stores it, on one line
auto material_json(const UUID& uuid, const Material& material) -> std::string;
// the material must be loaded, new texture slots are loaded and the old ones released by set_material
auto update_material(Editor& editor, const UUID& uuid, const Material& material) -> std::expected<void, std::string>;

// particle systems and cinematics as the document their file holds, on one line
auto asset_document(const UUID& uuid) -> std::expected<std::string, std::string>;
// replaces the whole document of a loaded particle system or cinematic, nothing changes when it does not parse
auto set_asset_document(Editor& editor, const UUID& uuid, std::string_view document)
  -> std::expected<void, std::string>;
} // namespace ox
