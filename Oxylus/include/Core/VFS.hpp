#pragma once

#include <ankerl/unordered_dense.h>
#include <string>

namespace ox {
class VFS {
public:
  static constexpr auto APP_DIR = "app_dir";

  // Only used by OxylusEditor. Virtual directory registered for projects.
  static constexpr auto PROJECT_DIR = "project_dir";

  auto is_mounted_dir(const std::string& virtual_dir) -> bool;

  auto mount_dir(const std::string& virtual_dir, const std::string& physical_dir) -> void;
  auto unmount_dir(const std::string& virtual_dir) -> void;

  auto resolve_physical_dir(const std::string& virtual_dir, const std::string& file_path) -> std::string;
  auto resolve_virtual_dir(const std::string& file_path) -> std::string;

private:
  ankerl::unordered_dense::map<std::string, std::string> mapped_dirs = {};
};
} // namespace ox
