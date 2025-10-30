#include "Scripting/LuaVFSBindings.hpp"

#include <sol/state.hpp>

#include "Core/VFS.hpp"

namespace ox {
auto VFSBinding::bind(sol::state* state) -> void {
  auto vfs_type = state->new_usertype<VFS>(
    "VFS",

    "APP_DIR",
    []() { return VFS::APP_DIR; },

    "PROJECT_DIR",
    []() { return VFS::PROJECT_DIR; },

    "is_mounted_dir",
    [](VFS* vfs, const std::string& virtual_dir) -> bool { return vfs->is_mounted_dir(virtual_dir); },

    "mount_dir",
    [](VFS* vfs, const std::string& virtual_dir, const std::filesystem::path& physical_dir) -> void {
      vfs->mount_dir(virtual_dir, physical_dir);
    },

    "unmount_dir",
    [](VFS* vfs, const std::string& virtual_dir) { vfs->unmount_dir(virtual_dir); },

    "resolve_physical_dir",
    [](VFS* vfs, const std::string& virtual_dir, const std::string& file_path) -> std::string {
      return vfs->resolve_physical_dir(virtual_dir, file_path).string();
    },

    "resolve_virtual_dir",
    [](VFS* vfs, const std::string& file_path) -> std::string { return vfs->resolve_virtual_dir(file_path).string(); }
  );
}
} // namespace ox
