#include "Scripting/LuaVFSBindings.hpp"

#include <sol/state.hpp>

#include "Core/VFS.hpp"

namespace ox {
auto VFSBinding::bind(sol::state* state) -> void {
  auto vfs_type = state->new_usertype<VFS>(
    "VFS",

    "APP_DIR",
    sol::var(VFS::APP_DIR),

    "ASSETS_DIR",
    sol::var(VFS::ASSETS_DIR),

    "COOKED_DIR",
    sol::var(VFS::COOKED_DIR),

    "is_mounted_dir",
    [](const VFS& vfs, const std::string& virtual_dir) -> bool { return vfs.is_mounted_dir(virtual_dir); },

    "mount_dir",
    [](VFS& vfs, const std::string& virtual_dir, const std::string& physical_dir) -> void {
      vfs.mount_dir(virtual_dir, physical_dir);
    },

    "unmount_dir",
    [](VFS& vfs, const std::string& virtual_dir) -> void { vfs.unmount_dir(virtual_dir); },

    "resolve_physical_dir",
    [](const VFS& vfs, const std::string& virtual_dir, const std::string& file_path) -> std::string {
      return vfs.resolve_physical_dir(virtual_dir, file_path).string();
    },

    "to_physical",
    [](const VFS& vfs, const std::string& virtual_path) -> std::string {
      return vfs.to_physical(virtual_path).string();
    },

    "to_virtual",
    [](const VFS& vfs, const std::string& path) -> std::string { return vfs.to_virtual(path).generic_string(); }
  );
}
} // namespace ox
