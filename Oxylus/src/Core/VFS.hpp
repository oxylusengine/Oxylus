#pragma once

#include "ESystem.hpp"

namespace ox {
class VFS : public ESystem {
public:
  static constexpr auto APP_DIR = "app_dir";

  // Only used by OxylusEditor. Virtual directory registered for projects.
  static constexpr auto PROJECT_DIR = "project_dir";

  auto init() -> std::expected<void, std::string> override;
  auto deinit() -> std::expected<void, std::string> override;

  auto is_mounted_dir(const std::string& virtual_dir) -> bool;

  auto mount_dir(const std::string& virtual_dir, const std::string& physical_dir) -> void;
  auto unmount_dir(const std::string& virtual_dir) -> void;

  auto resolve_physical_dir(const std::string& virtual_dir, const std::string& file_path) -> std::string;
  auto resolve_virtual_dir(const std::string& file_path) -> std::string;

private:
  ankerl::unordered_dense::map<std::string, std::string> mapped_dirs = {};
};
} // namespace ox
