#include "Asset/AssetImporter.hpp"

#include <ResourceCompiler.hpp>
#include <ankerl/unordered_dense.h>
#include <shared_mutex>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Core/VFS.hpp"
#include "Utils/Log.hpp"

namespace ox {
// Filled as imports go through, read by the UI on the main thread, so it is shared even though
// nothing here is hot.
struct AssetSourceRegistry {
  std::shared_mutex mutex = {};
  ankerl::unordered_dense::map<UUID, AssetSource> sources = {};
};

static AssetSourceRegistry asset_sources = {};

auto remap_path(
  const std::filesystem::path& path, const std::filesystem::path& old_path, const std::filesystem::path& new_path
) -> option<std::filesystem::path> {
  if (path.empty() || old_path.empty() || new_path.empty()) {
    return nullopt;
  }

  const auto normalized_path = path.lexically_normal();
  const auto normalized_old_path = old_path.lexically_normal();
  const auto normalized_new_path = new_path.lexically_normal();
  if (normalized_path == normalized_old_path) {
    return normalized_new_path;
  }

  const auto relative_path = normalized_path.lexically_relative(normalized_old_path);
  if (relative_path.empty() || relative_path.is_absolute()) {
    return nullopt;
  }

  for (const auto& component : relative_path) {
    if (component == "..") {
      return nullopt;
    }
  }

  return normalized_new_path / relative_path;
}

static auto record_asset_source(const UUID& uuid, const std::filesystem::path& path, std::string name) -> void {
  if (!uuid) {
    return;
  }

  auto lock = std::unique_lock(asset_sources.mutex);
  asset_sources.sources.insert_or_assign(uuid, AssetSource{.path = path, .name = std::move(name)});
}

auto asset_source(const UUID& uuid) -> AssetSource {
  auto lock = std::shared_lock(asset_sources.mutex);
  const auto it = asset_sources.sources.find(uuid);

  return it != asset_sources.sources.end() ? it->second : AssetSource{};
}

auto relocate_asset_paths(
  AssetManager& asset_man, const std::filesystem::path& old_path, const std::filesystem::path& new_path
) -> void {
  ZoneScoped;

  // the registry stores virtual paths, so the move has to be expressed in the same terms
  auto& vfs = App::get_vfs();
  const auto old_virtual_path = vfs.to_virtual(old_path);
  const auto new_virtual_path = vfs.to_virtual(new_path);
  for (const auto& asset : asset_man.get_registry_snapshot()) {
    const auto relocated_path = remap_path(asset.path, old_virtual_path, new_virtual_path);
    const auto relocated_source_path = remap_path(asset.source_path, old_virtual_path, new_virtual_path);
    if (relocated_path || relocated_source_path) {
      asset_man.update_asset_path(
        asset.uuid,
        relocated_path.value_or(asset.path),
        relocated_source_path.value_or(asset.source_path)
      );
    }
  }

  auto lock = std::unique_lock(asset_sources.mutex);
  for (auto& [uuid, source] : asset_sources.sources) {
    if (const auto relocated_path = remap_path(source.path, old_path, new_path)) {
      source.path = std::move(*relocated_path);
    }
  }
}

auto cache_dir() -> std::filesystem::path { return std::filesystem::current_path() / ".oxeditor/assets"; }

// `Session` accumulates messages for its whole lifetime, and imports run concurrently, so an import
// drains what is there rather than slicing by offset -- with several importers pushing at once an
// offset reports another thread's messages too, and reports them again from its own log. Draining
// makes attribution approximate but prints every message exactly once.
static auto log_diagnostics(rc::Session& session) -> void {
  const auto diagnostics = session.take_diagnostics();
  for (const auto& message : diagnostics.messages) {
    OX_LOG_INFO("{}", message);
  }
  for (const auto& error : diagnostics.errors) {
    OX_LOG_ERROR("{}", error);
  }
}

auto import_asset(
  AssetManager& asset_man, rc::Session& session, const std::filesystem::path& path, option<bool> srgb_directive
) -> UUID {
  ZoneScoped;

  const auto result = rc::import_asset(session, cache_dir(), path, srgb_directive);
  log_diagnostics(session);

  for (const auto& asset : result.assets) {
    asset_man.register_asset(asset.uuid, asset.type, asset.path, asset.source_path);
    if (asset.material.has_value()) {
      asset_man.set_pending_load_info(asset.uuid, *asset.material);
    }
    record_asset_source(asset.uuid, asset.origin, asset.name);
  }

  return result.uuid;
}

auto import_asset(AssetManager& asset_man, const std::filesystem::path& path, option<bool> srgb_directive) -> UUID {
  ZoneScoped;

  return import_asset(asset_man, App::mod<rc::ResourceCompiler>(), path, srgb_directive);
}
} // namespace ox
