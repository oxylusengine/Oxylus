#include "Utils/AnimationAssets.hpp"

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"

namespace ox {
auto find_source_model(const UUID& asset_uuid) -> UUID {
  ZoneScoped;

  auto& asset_man = App::mod<AssetManager>();

  auto asset_path = std::filesystem::path();
  if (auto asset = asset_man.get_asset(asset_uuid)) {
    if (asset->type == AssetType::Model) {
      return asset_uuid;
    }

    asset_path = asset->path;
  }

  if (asset_path.empty()) {
    return UUID(nullptr);
  }

  for (const auto& registered : asset_man.get_registry_snapshot()) {
    if (registered.type == AssetType::Model && registered.path == asset_path) {
      return registered.uuid;
    }
  }

  return UUID(nullptr);
}

auto model_animation_clips(const UUID& model_uuid) -> ankerl::svector<UUID, 8> {
  ZoneScoped;

  auto clips = ankerl::svector<UUID, 8>();
  if (!model_uuid) {
    return clips;
  }

  auto& asset_man = App::mod<AssetManager>();
  if (auto model = asset_man.get_model(model_uuid)) {
    clips.assign(model->animations.begin(), model->animations.end());
  }

  return clips;
}

auto sibling_animation_clips(const UUID& asset_uuid) -> ankerl::svector<UUID, 8> {
  ZoneScoped;

  auto& asset_man = App::mod<AssetManager>();

  auto asset_path = std::filesystem::path();
  if (auto asset = asset_man.get_asset(asset_uuid)) {
    asset_path = asset->path;
  }

  auto clips = ankerl::svector<UUID, 8>();
  if (asset_path.empty()) {
    return clips;
  }

  for (const auto& registered : asset_man.get_registry_snapshot()) {
    if (registered.type == AssetType::Animation && registered.path == asset_path) {
      clips.emplace_back(registered.uuid);
    }
  }

  return clips;
}
} // namespace ox
