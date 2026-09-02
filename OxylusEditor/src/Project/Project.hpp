#pragma once

#include <deque>
#include <filesystem>
#include <string>

#include "Core/Types.hpp"
#include "Core/UUID.hpp"

namespace ox {
class AssetManager;
class JobManager;
struct AssetDirectory;

namespace rc {
struct Session;
}

struct AssetDirectoryCallbacks {
  void* user_data = nullptr;
  void (*on_new_directory)(void* user_data, AssetDirectory* directory) = nullptr;
  void (*on_new_asset)(void* user_data, UUID& asset_uuid) = nullptr;
};

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

  auto refresh(this AssetDirectory& self) -> void;
};

// A project scan imports on the job manager and reports its results back one file at a time, so the
// editor stays live while a cold project cooks. `poll_asset_scans` has to run once a frame, and only
// on the thread that started the scan -- it is what applies UUIDs to the directory tree.
// Starts a scan: walks the tree on this thread, then queues every import on the job manager.
// Returns without waiting -- `poll_asset_scans` collects the results.
auto populate_directory(
  AssetDirectory* dir,
  const AssetDirectoryCallbacks& callbacks,
  AssetManager& asset_man,
  rc::Session& session,
  JobManager& job_man
) -> void;

// Same, resolving the asset manager, compiler and job manager itself.
auto populate_directory(AssetDirectory* dir, const AssetDirectoryCallbacks& callbacks) -> void;

// A snapshot of everything the active scans still owe, split by how expensive the work is: a model
// or a texture goes through the compiler, while the rest only reads or writes a sidecar.
struct AssetScanProgress {
  usize compiled_completed = 0;
  usize compiled_total = 0;
  usize registered_completed = 0;
  usize registered_total = 0;
  std::string current_file = {};
};

auto poll_asset_scans() -> void;
auto wait_for_asset_scans() -> void;
auto asset_scan_active() -> bool;
auto asset_scan_pending(const std::filesystem::path& path) -> bool;
auto asset_scan_progress() -> AssetScanProgress;

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

  auto get_project_directory() const -> const std::filesystem::path& { return project_directory; }
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
