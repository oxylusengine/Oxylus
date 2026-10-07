#pragma once

#include <AssetImport.hpp>
#include <filesystem>
#include <string>

#include "Core/Option.hpp"
#include "Core/Types.hpp"
#include "Core/UUID.hpp"

namespace ox {
class AssetManager;

// the importer itself lives in ResourceCompiler, where a game build's `rcli --cook-assets` runs the same code
using rc::AssetFileType;
using rc::meta_file_path;
using rc::needs_compiling;
using rc::owns_meta_file;
using rc::to_asset_file_type;
using rc::to_asset_type;

// Alongside the thumbnail cache, and editor-global for the same reason: entries are keyed by UUID,
// so nothing about them is specific to the project that produced them.
auto cache_dir() -> std::filesystem::path;

// The single funnel every editor import goes through: `rc::import_asset` into the editor's cache,
// then registers everything it produced with `asset_man`.
//
// `usage_directive` is how a model oversees its own resources, see `rc::import_asset`.
auto import_asset(
  AssetManager& asset_man,
  rc::Session& session,
  const std::filesystem::path& path,
  option<rc::TextureUsage> usage_directive = nullopt
) -> UUID;

// Same, resolving the compiler module itself.
auto import_asset(
  AssetManager& asset_man, const std::filesystem::path& path, option<rc::TextureUsage> usage_directive = nullopt
) -> UUID;

// The cook a game build runs through `rcli --cook-assets`, for checking its output from the editor. Nothing in
// editing or shipping depends on it. `output_dir` is dedicated to the cook, stale packs in it are removed.
auto cook_project_assets(const std::filesystem::path& assets_dir, const std::filesystem::path& output_dir) -> bool;

// Scripts are cooked like everything else, but they are the asset edited while the editor runs and expected to take
// effect straight away. This cooks again every script whose source moved on and swaps the loaded ones in place, a play
// session or a script reload calls it first.
auto refresh_scripts(AssetManager& asset_man) -> void;

auto remap_path(
  const std::filesystem::path& path, const std::filesystem::path& old_path, const std::filesystem::path& new_path
) -> option<std::filesystem::path>;

// Moves the registry and source-display paths rooted at `old_path` to `new_path`. Compiled assets
// keep their cache path but follow with their source path; direct-to-source assets use the new
// source on their next load or save.
auto relocate_asset_paths(
  AssetManager& asset_man, const std::filesystem::path& old_path, const std::filesystem::path& new_path
) -> void;

// Where a uuid came from on disk. Anything the compiler cooks is registered against the
// `<uuid>.oxpack` in the cache, so `Asset::path` names the pack rather than the file, and the
// sidecar that knows better is keyed by source path -- the import is the only moment both are in
// hand, so it records them here. `name` is what the UI shows: the source file, plus which slot of
// it for the textures and materials a model brings with it. Empty when the uuid was never imported.
struct AssetSource {
  std::filesystem::path path = {};
  std::string name = {};
};

auto asset_source(const UUID& uuid) -> AssetSource;
} // namespace ox
