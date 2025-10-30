#pragma once

#include <deque>
#include <filesystem>

#include "Core/UUID.hpp"

namespace ox {
struct ProjectConfig {
  std::string name = "Untitled";

  std::string start_scene = {};
  std::filesystem::path asset_directory = {};
};

struct AssetDirectory {
  std::filesystem::path path = {};

  AssetDirectory* parent = nullptr;
  std::deque<std::unique_ptr<AssetDirectory>> subdirs = {};
  ankerl::unordered_dense::set<UUID> asset_uuids = {};

  AssetDirectory(std::filesystem::path path_, AssetDirectory* parent_);

  ~AssetDirectory();

  auto add_subdir(this AssetDirectory& self, const std::filesystem::path& dir_path) -> AssetDirectory*;

  auto add_subdir(this AssetDirectory& self, std::unique_ptr<AssetDirectory>&& directory) -> AssetDirectory*;

  auto add_asset(this AssetDirectory& self, const std::filesystem::path& dir_path) -> UUID;

  auto refresh(this AssetDirectory& self) -> void;
};

class Project {
public:
  Project() = default;

  auto new_project(
    this Project& self,
    const std::filesystem::path& project_dir,
    std::string_view project_name,
    const std::filesystem::path& project_asset_dir
  ) -> bool;
  auto load(this Project& self, const std::filesystem::path& path) -> bool;
  auto save(this Project& self, const std::filesystem::path& path) -> bool;

  auto get_config() -> ProjectConfig& { return project_config; }

  auto get_project_directory() -> const std::filesystem::path& { return project_directory; }
  auto set_project_dir(const std::filesystem::path& dir) -> void { project_directory = dir; }
  auto get_project_file_path() const -> const std::filesystem::path& { return project_file_path; }

  auto get_asset_directory() -> const std::unique_ptr<AssetDirectory>& { return asset_directory; }

  auto register_assets(const std::filesystem::path& path) -> void;

private:
  ProjectConfig project_config = {};
  std::filesystem::path project_directory = {};
  std::filesystem::path project_file_path = {};
  std::filesystem::file_time_type last_module_write_time = {};
  std::unique_ptr<AssetDirectory> asset_directory = nullptr;
};
} // namespace ox
