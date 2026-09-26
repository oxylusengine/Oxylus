#pragma once

#include <ankerl/unordered_dense.h>
#include <filesystem>

namespace ox {
class VFS {
public:
  static constexpr auto APP_DIR = "app_dir";

  // only used by OxylusEditor, virtual directory registered for projects
  static constexpr auto PROJECT_DIR = "project_dir";

  auto is_mounted_dir(this const VFS& self, const std::filesystem::path& virtual_dir) -> bool;

  // remounting an already mounted virtual dir replaces its physical dir
  auto mount_dir(this VFS& self, const std::filesystem::path& virtual_dir, const std::filesystem::path& physical_dir)
    -> void;
  auto unmount_dir(this VFS& self, const std::filesystem::path& virtual_dir) -> void;

  auto resolve_physical_dir(
    this const VFS& self, const std::filesystem::path& virtual_dir, const std::filesystem::path& file_path
  ) -> std::filesystem::path;
  auto resolve_virtual_dir(this const VFS& self, const std::filesystem::path& file_path) -> std::filesystem::path;

private:
  ankerl::unordered_dense::map<std::filesystem::path, std::filesystem::path> mapped_dirs = {};
};
} // namespace ox
