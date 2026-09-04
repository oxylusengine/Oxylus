#include "Project/Project.hpp"

#include <ResourceCompiler.hpp>
#include <system_error>
#include <thread>

#include "Asset/AssetImporter.hpp"
#include "Asset/AssetManager.hpp"
#include "Asset/AssetMeta.hpp"
#include "Core/App.hpp"
#include "Core/JobManager.hpp"
#include "Core/UUID.hpp"
#include "Core/VFS.hpp"
#include "Project/ProjectSerializer.hpp"

namespace ox {

struct PendingImport {
  AssetDirectory* dir = nullptr;
  std::filesystem::path path = {};
  std::string key = {};
  UUID uuid = UUID(nullptr);
  std::atomic<bool> started = false;
  std::atomic<bool> finished = false;
  bool applied = false;
  // whether the compiler has to cook this one, as opposed to just reading or writing a sidecar
  bool compiled = false;
};

struct AssetScan {
  // a deque because the jobs hold pointers into it; nothing may move once they are submitted
  std::deque<PendingImport> imports = {};
  ankerl::unordered_dense::set<std::string> pending_keys = {};
  AssetDirectoryCallbacks callbacks = {};
  usize applied_count = 0;
  usize compiled_total = 0;
  usize compiled_applied = 0;
};

// Only ever touched from the thread that drives the editor: scans start from a project load and are
// drained by `poll_asset_scans`, both on the main thread. A job writes nothing but its own slot.
static std::vector<std::unique_ptr<AssetScan>> active_scans = {};

// Builds the directory tree and lists what has to be imported, without importing anything. The walk
// mutates `AssetDirectory` and fires `on_new_directory`, so it stays on one thread.
auto collect_directory(AssetDirectory* dir, const AssetDirectoryCallbacks& callbacks, AssetScan& scan) -> void {
  for (const auto& entry : std::filesystem::directory_iterator(dir->path)) {
    const auto& path = entry.path();
    if (entry.is_directory()) {
      // The editor's own caches, not project source. Only reachable when the editor is run from
      // inside the asset directory, but then every cached texture and thumbnail would be imported as
      // an asset of its own.
      if (path == cache_dir().parent_path()) {
        continue;
      }

      AssetDirectory* cur_subdir = nullptr;
      auto dir_it = std::ranges::find_if(dir->subdirs, [&](const auto& v) { return path == v->path; });
      if (dir_it == dir->subdirs.end()) {
        auto* new_dir = dir->add_subdir(path);
        if (callbacks.on_new_directory) {
          callbacks.on_new_directory(callbacks.user_data, new_dir);
        }

        cur_subdir = new_dir;
      } else {
        cur_subdir = dir_it->get();
      }

      collect_directory(cur_subdir, callbacks, scan);
    } else if (entry.is_regular_file()) {
      auto& import = scan.imports.emplace_back();
      import.dir = dir;
      import.path = path;
      import.key = path.lexically_normal().string();
      import.compiled = needs_compiling(path);
      if (import.compiled) {
        scan.compiled_total += 1;
      }
    }
  }
}

// Hands finished imports back to the directory tree. Main thread only.
auto apply_scan(AssetScan& scan) -> void {
  for (auto& import : scan.imports) {
    if (import.applied || !import.finished.load(std::memory_order_acquire)) {
      continue;
    }

    if (import.uuid) {
      import.dir->asset_uuids.emplace(import.uuid);
    }

    if (scan.callbacks.on_new_asset) {
      scan.callbacks.on_new_asset(scan.callbacks.user_data, import.uuid);
    }

    import.applied = true;
    scan.pending_keys.erase(import.key);
    scan.applied_count += 1;
    if (import.compiled) {
      scan.compiled_applied += 1;
    }
  }
}

auto populate_directory(
  AssetDirectory* dir,
  const AssetDirectoryCallbacks& callbacks,
  AssetManager& asset_man,
  rc::Session& session,
  JobManager& job_man
) -> void {
  ZoneScoped;

  auto scan = std::make_unique<AssetScan>();
  scan->callbacks = callbacks;
  collect_directory(dir, callbacks, *scan);
  if (scan->imports.empty()) {
    return;
  }

  // once here, rather than raced for by every job that turns out to need it
  auto error = std::error_code{};
  std::filesystem::create_directories(cache_dir(), error);

  if (job_man.get_thread_count() <= 1) {
    for (auto& import : scan->imports) {
      import.started.store(true, std::memory_order_relaxed);
      import.uuid = import_asset(asset_man, session, import.path);
      import.finished.store(true, std::memory_order_release);
    }

    apply_scan(*scan);
    return;
  }

  for (auto& import : scan->imports) {
    scan->pending_keys.emplace(import.key);
  }

  // a cold first open compiles every model in the project, and one import is independent of the
  // next -- `import_asset` gates itself per path so two of them cannot cook the same file. nothing
  // is waited on here: `poll_asset_scans` picks the results up over the following frames, so the
  // editor is usable while its assets cook.
  for (auto& import : scan->imports) {
    job_man.submit(Job::create([&asset_man, &session, &import] {
      import.started.store(true, std::memory_order_relaxed);
      import.uuid = import_asset(asset_man, session, import.path);
      import.finished.store(true, std::memory_order_release);
    }));
  }

  active_scans.push_back(std::move(scan));
}

auto populate_directory(AssetDirectory* dir, const AssetDirectoryCallbacks& callbacks) -> void {
  populate_directory(
    dir,
    callbacks,
    App::mod<AssetManager>(),
    App::mod<rc::ResourceCompiler>(),
    App::get_job_manager()
  );
}

auto poll_asset_scans() -> void {
  ZoneScoped;

  for (auto& scan : active_scans) {
    apply_scan(*scan);
  }

  std::erase_if(active_scans, [](const auto& scan) { return scan->applied_count == scan->imports.size(); });
}

auto wait_for_asset_scans() -> void {
  ZoneScoped;

  // the jobs are already queued on the pool, so this only has to outlast them -- running them here
  // would pull unrelated work onto the main thread
  for (auto& scan : active_scans) {
    for (auto& import : scan->imports) {
      while (!import.finished.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    }
  }

  poll_asset_scans();
}

auto asset_scan_active() -> bool { return !active_scans.empty(); }

auto asset_scan_progress() -> AssetScanProgress {
  ZoneScoped;

  auto progress = AssetScanProgress();
  for (const auto& scan : active_scans) {
    progress.compiled_completed += scan->compiled_applied;
    progress.compiled_total += scan->compiled_total;
    progress.registered_completed += scan->applied_count - scan->compiled_applied;
    progress.registered_total += scan->imports.size() - scan->compiled_total;

    if (!progress.current_file.empty()) {
      continue;
    }

    // whichever worker got there first, rather than a file that is already done
    for (const auto& import : scan->imports) {
      if (import.started.load(std::memory_order_relaxed) && !import.finished.load(std::memory_order_acquire)) {
        progress.current_file = import.path.filename().string();
        break;
      }
    }
  }

  return progress;
}

auto asset_scan_pending(const std::filesystem::path& path) -> bool {
  if (active_scans.empty()) {
    return false;
  }

  const auto key = path.lexically_normal().string();

  return std::ranges::any_of(active_scans, [&](const auto& scan) { return scan->pending_keys.contains(key); });
}

AssetDirectory::AssetDirectory(std::filesystem::path path_, AssetDirectory* parent_)
    : path(std::move(path_)),
      parent(parent_) {}

AssetDirectory::~AssetDirectory() {
  auto& asset_man = App::mod<AssetManager>();
  for (const auto& asset_uuid : this->asset_uuids) {
    if (asset_man.get_asset(asset_uuid)) {
      asset_man.delete_asset(asset_uuid);
    }
  }
}

auto AssetDirectory::add_subdir(this AssetDirectory& self, const std::filesystem::path& dir_path) -> AssetDirectory* {
  auto dir = std::make_unique<AssetDirectory>(dir_path, &self);

  return self.add_subdir(std::move(dir));
}

auto AssetDirectory::add_subdir(this AssetDirectory& self, std::unique_ptr<AssetDirectory>&& directory)
  -> AssetDirectory* {
  auto* ptr = directory.get();
  self.subdirs.push_back(std::move(directory));

  return ptr;
}

auto AssetDirectory::refresh(this AssetDirectory& self) -> void { populate_directory(&self, {}); }

auto Project::register_assets(const std::filesystem::path& path) -> void {
  // a scan still in flight holds `AssetDirectory*` slots into the tree we are about to replace
  wait_for_asset_scans();

  this->asset_directory = std::make_unique<AssetDirectory>(path, nullptr);
  populate_directory(this->asset_directory.get(), {});
}

auto Project::new_project(
  this Project& self,
  const std::filesystem::path& project_dir,
  std::string_view project_name,
  const std::filesystem::path& project_asset_dir
) -> bool {
  if (project_dir.empty() || project_name.empty() || project_asset_dir.empty()) {
    return false;
  }

  std::error_code error;
  std::filesystem::create_directories(project_dir, error);
  if (error) {
    OX_LOG_ERROR("Couldn't create project directory {}: {}", project_dir, error.message());
    return false;
  }

  const auto asset_folder_dir = project_dir / project_asset_dir;
  std::filesystem::create_directories(asset_folder_dir, error);
  if (error) {
    OX_LOG_ERROR("Couldn't create project asset directory {}: {}", asset_folder_dir, error.message());
    return false;
  }

  self.project_config.name = project_name;
  self.project_config.asset_directory = project_asset_dir;

  self.set_project_dir(project_dir);

  self.project_file_path = project_dir / project_name;
  self.project_file_path.replace_extension(".oxproj");

  const ProjectSerializer serializer(&self);
  if (!serializer.serialize(self.project_file_path)) {
    OX_LOG_ERROR("Couldn't write project file: {}", self.project_file_path);
    return false;
  }

  const auto asset_dir_path = self.project_file_path.parent_path() / self.project_config.asset_directory;
  auto& vfs = App::get_vfs();
  if (vfs.is_mounted_dir(VFS::PROJECT_DIR)) {
    vfs.unmount_dir(VFS::PROJECT_DIR);
  }
  vfs.mount_dir(VFS::PROJECT_DIR, asset_dir_path);

  self.register_assets(asset_dir_path);

  return true;
}

auto Project::load(this Project& self, const std::filesystem::path& path) -> bool {
  const ProjectSerializer serializer(&self);
  if (serializer.deserialize(path)) {
    self.set_project_dir(path.parent_path());
    self.project_file_path = std::filesystem::absolute(path);

    auto project_root_path = self.project_file_path.parent_path();
    const auto asset_dir_path = project_root_path / self.project_config.asset_directory;

    auto& vfs = App::get_vfs();
    if (vfs.is_mounted_dir(VFS::PROJECT_DIR))
      vfs.unmount_dir(VFS::PROJECT_DIR);
    vfs.mount_dir(VFS::PROJECT_DIR, asset_dir_path);

    wait_for_asset_scans();
    self.asset_directory.reset();
    self.register_assets(asset_dir_path);

    OX_LOG_INFO("Project loaded: {0}", self.project_config.name);
    return true;
  }

  return false;
}

auto Project::save(this Project& self, const std::filesystem::path& path) -> bool {
  const ProjectSerializer serializer(&self);
  if (serializer.serialize(path)) {
    self.set_project_dir(path.parent_path());
    return true;
  }
  return false;
}
} // namespace ox
