#include "Core/Project.hpp"

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Core/ProjectSerializer.hpp"
#include "Core/UUID.hpp"
#include "Core/VFS.hpp"

namespace ox {
struct AssetDirectoryCallbacks {
  void* user_data = nullptr;
  void (*on_new_directory)(void* user_data, AssetDirectory* directory) = nullptr;
  void (*on_new_asset)(void* user_data, UUID& asset_uuid) = nullptr;
};

auto populate_directory(AssetDirectory* dir, const AssetDirectoryCallbacks& callbacks) -> void {
  for (const auto& entry : std::filesystem::directory_iterator(dir->path)) {
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

auto AssetDirectory::add_asset(this AssetDirectory& self, const std::filesystem::path& dir_path) -> UUID {
  auto& asset_man = App::mod<AssetManager>();
  auto asset_uuid = asset_man.import_asset(dir_path);
  if (!asset_uuid) {
    return UUID(nullptr);
  }

  self.asset_uuids.emplace(asset_uuid);

  return asset_uuid;
}

auto AssetDirectory::refresh(this AssetDirectory& self) -> void { populate_directory(&self, {}); }

auto Project::register_assets(const std::filesystem::path& path) -> void {
  this->asset_directory = std::make_unique<AssetDirectory>(path, nullptr);
  populate_directory(this->asset_directory.get(), {});
}

auto Project::new_project(
  this Project& self,
  const std::filesystem::path& project_dir,
  std::string_view project_name,
  const std::filesystem::path& project_asset_dir
) -> bool {
  self.project_config.name = project_name;
  self.project_config.asset_directory = project_asset_dir;

  self.set_project_dir(project_dir);

  if (project_dir.empty())
    return false;

  std::filesystem::create_directory(project_dir);

  const auto asset_folder_dir = project_dir / project_asset_dir;
  std::filesystem::create_directory(asset_folder_dir);

  self.project_file_path = project_dir / project_name;
  self.project_file_path.replace_extension(".oxproj");

  const ProjectSerializer serializer(&self);
  serializer.serialize(self.project_file_path);

  const auto asset_dir_path = self.project_file_path.parent_path() / self.project_config.asset_directory;
  App::get_vfs().mount_dir(VFS::PROJECT_DIR, asset_dir_path);

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
