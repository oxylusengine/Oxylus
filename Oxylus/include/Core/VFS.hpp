#pragma once

#include <ankerl/unordered_dense.h>
#include <filesystem>
#include <shared_mutex>

namespace ox {
// A virtual path is `<virtual dir>/<relative path>`, e.g. `assets_dir/Audio/engine.wav`, and means the same file in
// the editor and in a shipped game. Absolute paths pass through both conversions untouched, for files outside every
// mount.
class VFS {
public:
  // the running program's own resources: the editor's fonts and shaders, or the game's when shipped
  static constexpr auto APP_DIR = "app_dir";

  // game content: the loaded project's assets in the editor, the shipped assets directory otherwise
  static constexpr auto ASSETS_DIR = "assets_dir";

  // compiled asset payloads and the asset manifest: the editor's asset cache while editing, and in a shipped game
  // `COOKED_SUBDIR` inside the assets directory, so the game's build copies it along with the sources
  static constexpr auto COOKED_DIR = "cooked_dir";
  static constexpr auto COOKED_SUBDIR = ".cooked";

  auto is_mounted_dir(this const VFS& self, const std::filesystem::path& virtual_dir) -> bool;

  // remounting an already mounted virtual dir replaces its physical dir
  auto mount_dir(this VFS& self, const std::filesystem::path& virtual_dir, const std::filesystem::path& physical_dir)
    -> void;
  auto unmount_dir(this VFS& self, const std::filesystem::path& virtual_dir) -> void;

  auto resolve_physical_dir(
    this const VFS& self, const std::filesystem::path& virtual_dir, const std::filesystem::path& file_path
  ) -> std::filesystem::path;

  auto to_physical(this const VFS& self, const std::filesystem::path& virtual_path) -> std::filesystem::path;
  // accepts a virtual path too, which comes back unchanged
  auto to_virtual(this const VFS& self, const std::filesystem::path& path) -> std::filesystem::path;

private:
  mutable std::shared_mutex mutex = {};
  ankerl::unordered_dense::map<std::filesystem::path, std::filesystem::path> mapped_dirs = {};
};
} // namespace ox
