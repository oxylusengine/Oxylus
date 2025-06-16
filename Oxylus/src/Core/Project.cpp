#include "Project.hpp"

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Core/FileSystem.hpp"
#include "Core/UUID.hpp"
#include "Modules/ModuleRegistry.hpp"
#include "Modules/ModuleUtil.hpp"
#include "ProjectSerializer.hpp"
#include "VFS.hpp"

namespace ox {
struct AssetDirectoryCallbacks {
  void* user_data = nullptr;
  void (*on_new_directory)(void* user_data, AssetDirectory* directory) = nullptr;
  void (*on_new_asset)(void* user_data, UUID& asset_uuid) = nullptr;
};

auto populate_directory(AssetDirectory* dir, const AssetDirectoryCallbacks& callbacks) -> void {
  for (const auto& entry : ::fs::directory_iterator(dir->path)) {
    const auto& path = entry.path();
    if (entry.is_directory()) {
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

      populate_directory(cur_subdir, callbacks);
    } else if (entry.is_regular_file()) {
      auto new_asset_uuid = dir->add_asset(path);
      if (callbacks.on_new_asset) {
        callbacks.on_new_asset(callbacks.user_data, new_asset_uuid);
      }
    }
  }
}

AssetDirectory::AssetDirectory(::fs::path path_, AssetDirectory* parent_) : path(std::move(path_)), parent(parent_) {}

AssetDirectory::~AssetDirectory() {
  auto* asset_man = App::get_asset_manager();
  for (const auto& asset_uuid : this->asset_uuids) {
    if (asset_man->get_asset(asset_uuid)) {
      asset_man->delete_asset(asset_uuid);
    }
  }
}

auto AssetDirectory::add_subdir(this AssetDirectory& self, const ::fs::path& path) -> AssetDirectory* {
  auto dir = std::make_unique<AssetDirectory>(path, &self);

  return self.add_subdir(std::move(dir));
}

auto AssetDirectory::add_subdir(this AssetDirectory& self, std::unique_ptr<AssetDirectory>&& directory)
    -> AssetDirectory* {
  auto* ptr = directory.get();
  self.subdirs.push_back(std::move(directory));

  return ptr;
}

auto AssetDirectory::add_asset(this AssetDirectory& self, const ::fs::path& path) -> UUID {
  auto* asset_man = App::get_asset_manager();
  auto asset_uuid = asset_man->import_asset(path.string());
  if (!asset_uuid) {
    return UUID(nullptr);
  }

  self.asset_uuids.emplace(asset_uuid);

  return asset_uuid;
}

auto AssetDirectory::refresh(this AssetDirectory& self) -> void { populate_directory(&self, {}); }

auto Project::register_assets(const std::string& path) -> void {
  this->asset_directory = std::make_unique<AssetDirectory>(path, nullptr);
  populate_directory(this->asset_directory.get(), {});
}

void Project::load_module() {
  if (get_config().module_name.empty())
    return;

  const auto module_path = fs::append_paths(get_project_directory(), project_config.module_name);
  ModuleUtil::load_module(project_config.module_name, module_path);
  auto* module_registry = App::get_system<ModuleRegistry>(EngineSystems::ModuleRegistry);
  if (auto* module = module_registry->get_lib(project_config.module_name))
    last_module_write_time = std::filesystem::last_write_time(module->path + dylib::filename_components::suffix);
}

void Project::unload_module() const {
  if (!project_config.module_name.empty())
    ModuleUtil::unload_module(project_config.module_name);
}

void Project::check_module() {
  if (get_config().module_name.empty())
    return;

  auto* module_registry = App::get_system<ModuleRegistry>(EngineSystems::ModuleRegistry);
  if (auto* module = module_registry->get_lib(project_config.module_name)) {
    const auto& module_path = module->path + dylib::filename_components::suffix;
    if (std::filesystem::last_write_time(module_path).time_since_epoch().count() !=
        last_module_write_time.time_since_epoch().count()) {
      ModuleUtil::unload_module(project_config.module_name);
      ModuleUtil::load_module(project_config.module_name, module->path);
      last_module_write_time = std::filesystem::last_write_time(module_path);
      OX_LOG_INFO("Reloaded {} module.", project_config.module_name);
    }
  }
}

auto Project::new_project(this Project& self,
                          const std::string& project_dir,
                          const std::string& project_name,
                          const std::string& project_asset_dir) -> bool {
  self.project_config.name = project_name;
  self.project_config.asset_directory = project_asset_dir;

  self.set_project_dir(project_dir);

  if (project_dir.empty())
    return false;

  ::fs::create_directory(project_dir);

  const auto asset_folder_dir = fs::append_paths(project_dir, project_asset_dir);
  ::fs::create_directory(asset_folder_dir);

  self.project_file_path = fs::append_paths(project_dir, project_name + ".oxproj");

  const ProjectSerializer serializer(&self);
  serializer.serialize(fs::append_paths(project_dir, project_name + ".oxproj"));

  const auto asset_dir_path = fs::append_paths(fs::get_directory(self.project_file_path),
                                               self.project_config.asset_directory);
  App::get_vfs()->mount_dir(VFS::PROJECT_DIR, asset_dir_path);

  self.register_assets(asset_dir_path);

  return true;
}

auto Project::load(this Project& self, const std::string& path) -> bool {
  const ProjectSerializer serializer(&self);
  if (serializer.deserialize(path)) {
    self.set_project_dir(std::filesystem::path(path).parent_path().string());
    self.project_file_path = std::filesystem::absolute(path).string();

    const auto asset_dir_path = fs::append_paths(fs::get_directory(self.project_file_path),
                                                 self.project_config.asset_directory);
    App::get_system<VFS>(EngineSystems::VFS)->mount_dir(VFS::PROJECT_DIR, asset_dir_path);

    self.register_assets(asset_dir_path);

    self.load_module();

    OX_LOG_INFO("Project loaded: {0}", self.project_config.name);
    return true;
  }

  return false;
}

auto Project::save(this Project& self, const std::string& path) -> bool {
  const ProjectSerializer serializer(&self);
  if (serializer.serialize(path)) {
    self.set_project_dir(std::filesystem::path(path).parent_path().string());
    return true;
  }
  return false;
}
} // namespace ox
