#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

#include "Asset/Material.hpp"
#include "Core/UUID.hpp"

namespace ox {
class Editor;

// the file is imported into the editor's cache and registered before this returns, which can take a while for models
auto import_file(Editor& editor, const std::filesystem::path& path) -> std::expected<UUID, std::string>;
// writes the asset and its sidecar to the file it was created or imported from, returns that file
auto save_asset(Editor& editor, const UUID& uuid) -> std::expected<std::filesystem::path, std::string>;

// the material must be loaded, new texture slots are loaded and the old ones released by set_material
auto update_material(Editor& editor, const UUID& uuid, const Material& material) -> std::expected<void, std::string>;

// replaces the whole document of a loaded particle system or cinematic, nothing changes when it does not parse
auto set_asset_document(const UUID& uuid, std::string_view document) -> std::expected<void, std::string>;
} // namespace ox
