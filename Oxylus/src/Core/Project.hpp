#pragma once

#include "Core/UUID.hpp"
namespace ox {
struct ProjectConfig {
  std::string name = "Untitled";

  std::string start_scene = {};
  std::string asset_directory = {};
  std::string module_name = {};
};

struct AssetDirectory {
  ::fs::path path = {};

  AssetDirectory* parent = nullptr;
  std::deque<std::unique_ptr<AssetDirectory>> subdirs = {};
  ankerl::unordered_dense::set<UUID> asset_uuids = {};

  AssetDirectory(::fs::path path_, AssetDirectory* parent_);

  ~AssetDirectory();

  auto add_subdir(this AssetDirectory& self, const ::fs::path& path) -> AssetDirectory*;

  auto add_subdir(this AssetDirectory& self, std::unique_ptr<AssetDirectory>&& directory) -> AssetDirectory*;

  auto add_asset(this AssetDirectory& self, const ::fs::path& path) -> UUID;

  auto refresh(this AssetDirectory& self) -> void;
};

class Project {
public:
  Project() = default;

  auto new_project(this Project& self,
                   const std::string& project_dir,
                   const std::string& project_name,
                   const std::string& project_asset_dir) -> bool;
  auto load(this Project& self, const std::string& path) -> bool;
  auto save(this Project& self, const std::string& path) -> bool;

  auto get_config() -> ProjectConfig& { return project_config; }

  auto get_project_directory() -> std::string& { return project_directory; }
  auto set_project_dir(const std::string& dir) -> void { project_directory = dir; }
  auto get_project_file_path() const -> const std::string& { return project_file_path; }

  auto get_asset_directory() -> const std::unique_ptr<AssetDirectory>& { return asset_directory; }

  auto register_assets(const std::string& path) -> void;

  auto load_module() -> void;
  auto unload_module() const -> void;
  auto check_module() -> void;

private:
  ProjectConfig project_config = {};
  std::string project_directory = {};
  std::string project_file_path = {};
  ::fs::file_time_type last_module_write_time = {};
  std::unique_ptr<AssetDirectory> asset_directory = nullptr;
};
} // namespace ox
