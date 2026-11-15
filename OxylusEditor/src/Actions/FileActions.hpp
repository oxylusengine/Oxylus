#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "Asset/AssetFile.hpp"
#include "Core/Types.hpp"
#include "Core/UUID.hpp"

namespace ox {
class Editor;

#ifdef OX_DOTNET
enum class CSharpScriptKind : u8;
#endif

struct CreatedAsset {
  UUID uuid = UUID(nullptr);
  std::filesystem::path path = {};
};

struct DeletedEntry {
  std::filesystem::path path = {};
  std::vector<UUID> unregistered = {};
  std::vector<UUID> kept = {};
};

auto is_in_asset_directory(const std::filesystem::path& path) -> bool;

// empty when the name is fine, otherwise what is wrong with it
auto entry_name_error(std::string_view name) -> std::string_view;

// `to_dir` and the entry must sit inside the asset directory. A sidecar travels with its file, the registry follows.
// Returns the entry's new path, which is the old one when it already lives in `to_dir`
auto move_content_entry(Editor& editor, const std::filesystem::path& from, const std::filesystem::path& to_dir)
  -> std::expected<std::filesystem::path, std::string>;
// Returns the new path
auto rename_content_entry(Editor& editor, const std::filesystem::path& path, std::string_view new_name)
  -> std::expected<std::filesystem::path, std::string>;
// Removes the entry, its sidecar and every registered asset that came from it. Assets that are still loaded make this
// refuse unless `is_forced`, which unloads them the way delete_asset does
auto delete_content_entry(Editor& editor, const std::filesystem::path& path, bool is_forced)
  -> std::expected<DeletedEntry, std::string>;
auto create_folder(Editor& editor, const std::filesystem::path& parent, std::string_view name)
  -> std::expected<std::filesystem::path, std::string>;
// Material, ParticleSystem and Cinematic, `name` gets the type's extension when it has none
auto create_asset_file(Editor& editor, const std::filesystem::path& dir, AssetType type, std::string_view name)
  -> std::expected<CreatedAsset, std::string>;
#ifdef OX_DOTNET
auto create_csharp_script(
  Editor& editor, const std::filesystem::path& dir, CSharpScriptKind kind, std::string_view name
) -> std::expected<std::filesystem::path, std::string>;
#endif
// what double-clicking the file in the content panel does, returns a sentence saying what happened
auto open_file(Editor& editor, const std::filesystem::path& path) -> std::expected<std::string, std::string>;
} // namespace ox
