#include "VFS.hpp"

#include "Core/FileSystem.hpp"

namespace ox {
auto VFS::init() -> std::expected<void, std::string> { return {}; }

auto VFS::deinit() -> std::expected<void, std::string> { return {}; }

auto VFS::is_mounted_dir(const std::string& virtual_dir) -> bool {
  ZoneScoped;
  return mapped_dirs.contains(virtual_dir);
}

auto VFS::mount_dir(const std::string& virtual_dir, const std::string& physical_dir) -> void {
  ZoneScoped;
  mapped_dirs.emplace(virtual_dir, physical_dir);
}

auto VFS::unmount_dir(const std::string& virtual_dir) -> void {
  ZoneScoped;
  mapped_dirs.erase(virtual_dir);
}

auto VFS::resolve_physical_dir(const std::string& virtual_dir, const std::string& file_path) -> std::string {
  ZoneScoped;
  if (!mapped_dirs.contains(virtual_dir)) {
    OX_LOG_ERROR("Not a mounted virtual dir: {}", virtual_dir);
    return {};
  }

  const auto physical_dir = mapped_dirs[virtual_dir];

  return fs::append_paths(physical_dir, file_path);
}

auto VFS::resolve_virtual_dir(const std::string& file_path) -> std::string {
  ZoneScoped;

  for (const auto& [virtual_dir, physical_dir] : mapped_dirs) {
    if (file_path.starts_with(physical_dir)) {
      const std::string relative_path = file_path.substr(physical_dir.length() + 1);
      return fs::preferred_path(fs::append_paths(fs::get_last_component(physical_dir), relative_path));
    }
  }

  OX_LOG_ERROR("Could not resolve virtual dir for: {}", file_path);
  return {};
}
} // namespace ox
