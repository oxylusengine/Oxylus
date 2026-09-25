#include "Core/VFS.hpp"

#include "Core/Types.hpp"
#include "Utils/Log.hpp"

namespace ox {
// `dir / "."` would leave a trailing separator, and paths that differ only by one compare unequal
static auto join(const std::filesystem::path& dir, const std::filesystem::path& relative_path)
  -> std::filesystem::path {
  return relative_path == "." ? dir : dir / relative_path;
}

auto VFS::is_mounted_dir(this const VFS& self, const std::filesystem::path& virtual_dir) -> bool {
  ZoneScoped;
  auto read_lock = std::shared_lock(self.mutex);
  return self.mapped_dirs.contains(virtual_dir);
}

auto VFS::mount_dir(this VFS& self, const std::filesystem::path& virtual_dir, const std::filesystem::path& physical_dir)
  -> void {
  ZoneScoped;
  auto write_lock = std::unique_lock(self.mutex);
  self.mapped_dirs.insert_or_assign(virtual_dir, std::filesystem::absolute(physical_dir).lexically_normal());
}

auto VFS::unmount_dir(this VFS& self, const std::filesystem::path& virtual_dir) -> void {
  ZoneScoped;
  auto write_lock = std::unique_lock(self.mutex);
  self.mapped_dirs.erase(virtual_dir);
}

auto VFS::resolve_physical_dir(
  this const VFS& self, const std::filesystem::path& virtual_dir, const std::filesystem::path& file_path
) -> std::filesystem::path {
  ZoneScoped;
  auto read_lock = std::shared_lock(self.mutex);
  const auto it = self.mapped_dirs.find(virtual_dir);
  if (it == self.mapped_dirs.end()) {
    OX_LOG_ERROR("Not a mounted virtual dir: {}", virtual_dir);
    return {};
  }

  return it->second / file_path;
}

auto VFS::to_physical(this const VFS& self, const std::filesystem::path& virtual_path) -> std::filesystem::path {
  ZoneScoped;
  if (virtual_path.empty() || virtual_path.is_absolute()) {
    return virtual_path;
  }

  auto read_lock = std::shared_lock(self.mutex);
  const auto it = self.mapped_dirs.find(*virtual_path.begin());
  if (it == self.mapped_dirs.end()) {
    OX_LOG_ERROR("Not a virtual path: {}", virtual_path);
    return {};
  }

  return join(it->second, virtual_path.lexically_normal().lexically_relative(*virtual_path.begin()));
}

auto VFS::to_virtual(this const VFS& self, const std::filesystem::path& path) -> std::filesystem::path {
  ZoneScoped;
  if (path.empty()) {
    return path;
  }

  auto read_lock = std::shared_lock(self.mutex);
  if (path.is_relative() && self.mapped_dirs.contains(*path.begin())) {
    return path.lexically_normal();
  }

  const auto physical_path = std::filesystem::absolute(path).lexically_normal();
  auto best_dir = std::filesystem::path{};
  auto best_relative = std::filesystem::path{};
  auto best_depth = 0_sz;
  for (const auto& [virtual_dir, physical_dir] : self.mapped_dirs) {
    // component-wise, so "Assets2/x" doesn't match a mount at "Assets"
    auto relative_path = physical_path.lexically_relative(physical_dir);
    if (relative_path.empty() || *relative_path.begin() == "..") {
      continue;
    }

    // game content wins outright: a shipped game mounts APP_DIR on the same folder, and assets must stay portable
    if (virtual_dir == ASSETS_DIR) {
      return join(virtual_dir, relative_path);
    }

    // otherwise the most specific mount
    const auto depth = static_cast<usize>(std::distance(physical_dir.begin(), physical_dir.end()));
    if (best_dir.empty() || depth > best_depth) {
      best_dir = virtual_dir;
      best_relative = std::move(relative_path);
      best_depth = depth;
    }
  }

  return best_dir.empty() ? physical_path : join(best_dir, best_relative);
}
} // namespace ox
