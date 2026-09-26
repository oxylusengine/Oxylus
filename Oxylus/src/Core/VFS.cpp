#include "Core/VFS.hpp"

#include "Utils/Log.hpp"

namespace ox {
auto VFS::is_mounted_dir(this const VFS& self, const std::filesystem::path& virtual_dir) -> bool {
  ZoneScoped;
  return self.mapped_dirs.contains(virtual_dir);
}

auto VFS::mount_dir(this VFS& self, const std::filesystem::path& virtual_dir, const std::filesystem::path& physical_dir)
  -> void {
  ZoneScoped;
  self.mapped_dirs.insert_or_assign(virtual_dir, physical_dir);
}

auto VFS::unmount_dir(this VFS& self, const std::filesystem::path& virtual_dir) -> void {
  ZoneScoped;
  self.mapped_dirs.erase(virtual_dir);
}

auto VFS::resolve_physical_dir(
  this const VFS& self, const std::filesystem::path& virtual_dir, const std::filesystem::path& file_path
) -> std::filesystem::path {
  ZoneScoped;
  const auto it = self.mapped_dirs.find(virtual_dir);
  if (it == self.mapped_dirs.end()) {
    OX_LOG_ERROR("Not a mounted virtual dir: {}", virtual_dir);
    return {};
  }

  return it->second / file_path;
}

auto VFS::resolve_virtual_dir(this const VFS& self, const std::filesystem::path& file_path) -> std::filesystem::path {
  ZoneScoped;

  const auto normalized_path = file_path.lexically_normal();
  for (const auto& [virtual_dir, physical_dir] : self.mapped_dirs) {
    // component-wise, so "Assets2/x" doesn't match a mount at "Assets"
    const auto relative_path = normalized_path.lexically_relative(physical_dir.lexically_normal());
    if (!relative_path.empty() && *relative_path.begin() != "..")
      return virtual_dir / relative_path;
  }

  OX_LOG_ERROR("Could not resolve virtual dir for: {}", file_path);
  return {};
}
} // namespace ox
