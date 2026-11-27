#include "Asset/AssetHandle.hpp"

#include <type_traits>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"

namespace ox {
// a ref made where no asset manager runs, like a component reflected by a tool, keeps only its UUID
static auto running_asset_manager() -> AssetManager* {
  return App::get() != nullptr && App::has_mod<AssetManager>() ? &App::mod<AssetManager>() : nullptr;
}

template <typename Id>
static auto payload_as(const Asset* record, const AssetType type) -> option<Id> {
  if (record == nullptr || record->type != type) {
    return nullopt;
  }

  const auto id = static_cast<Id>(record->payload_id());
  if (id == Id::Invalid) {
    return nullopt;
  }

  return id;
}

template <typename T>
auto resolve_asset(const Asset* record) -> AssetView<T> {
  auto& asset_man = App::mod<AssetManager>();
  const auto resolve = [&asset_man](auto id) -> AssetView<T> {
    if (!id.has_value()) {
      return {};
    }

    if constexpr (std::is_same_v<T, Model>) {
      return asset_man.get_model(*id);
    } else if constexpr (std::is_same_v<T, Texture>) {
      return asset_man.get_texture(*id);
    } else if constexpr (std::is_same_v<T, Material>) {
      return asset_man.get_material(*id);
    } else if constexpr (std::is_same_v<T, Scene>) {
      return asset_man.get_scene(*id);
    } else if constexpr (std::is_same_v<T, AudioSource>) {
      return asset_man.get_audio(*id);
    } else if constexpr (std::is_same_v<T, TerrainEdits>) {
      return asset_man.get_terrain_edits(*id);
    } else if constexpr (std::is_same_v<T, ParticleSystem>) {
      return asset_man.get_particle_system(*id);
    } else if constexpr (std::is_same_v<T, Skeleton>) {
      return asset_man.get_skeleton(*id);
    } else if constexpr (std::is_same_v<T, AnimationClip>) {
      return asset_man.get_animation(*id);
    } else {
      static_assert(std::is_same_v<T, Cinematic>, "not an asset payload type");
      return asset_man.get_cinematic(*id);
    }
  };

  if constexpr (std::is_same_v<T, Model>) {
    return resolve(payload_as<ModelID>(record, AssetType::Model));
  } else if constexpr (std::is_same_v<T, Texture>) {
    return resolve(payload_as<TextureID>(record, AssetType::Texture));
  } else if constexpr (std::is_same_v<T, Material>) {
    return resolve(payload_as<MaterialID>(record, AssetType::Material));
  } else if constexpr (std::is_same_v<T, Scene>) {
    return resolve(payload_as<SceneID>(record, AssetType::Scene));
  } else if constexpr (std::is_same_v<T, AudioSource>) {
    return resolve(payload_as<AudioID>(record, AssetType::Audio));
  } else if constexpr (std::is_same_v<T, TerrainEdits>) {
    return resolve(payload_as<TerrainEditsID>(record, AssetType::Terrain));
  } else if constexpr (std::is_same_v<T, ParticleSystem>) {
    return resolve(payload_as<ParticleSystemID>(record, AssetType::ParticleSystem));
  } else if constexpr (std::is_same_v<T, Skeleton>) {
    return resolve(payload_as<SkeletonID>(record, AssetType::Skeleton));
  } else if constexpr (std::is_same_v<T, AnimationClip>) {
    return resolve(payload_as<AnimationID>(record, AssetType::Animation));
  } else {
    return resolve(payload_as<CinematicID>(record, AssetType::Cinematic));
  }
}

template auto resolve_asset<Model>(const Asset*) -> AssetView<Model>;
template auto resolve_asset<Texture>(const Asset*) -> AssetView<Texture>;
template auto resolve_asset<Material>(const Asset*) -> AssetView<Material>;
template auto resolve_asset<Scene>(const Asset*) -> AssetView<Scene>;
template auto resolve_asset<AudioSource>(const Asset*) -> AssetView<AudioSource>;
template auto resolve_asset<TerrainEdits>(const Asset*) -> AssetView<TerrainEdits>;
template auto resolve_asset<ParticleSystem>(const Asset*) -> AssetView<ParticleSystem>;
template auto resolve_asset<Skeleton>(const Asset*) -> AssetView<Skeleton>;
template auto resolve_asset<AnimationClip>(const Asset*) -> AssetView<AnimationClip>;
template auto resolve_asset<Cinematic>(const Asset*) -> AssetView<Cinematic>;

AssetRef::AssetRef(const AssetRef& other) : uuid_(other.uuid_) { point_at_loaded(); }

AssetRef::AssetRef(AssetRef&& other) noexcept
    : uuid_(std::exchange(other.uuid_, UUID(nullptr))),
      record_(std::exchange(other.record_, nullptr)) {}

AssetRef::~AssetRef() { release(); }

auto AssetRef::operator=(const AssetRef& other) -> AssetRef& {
  if (this != &other) {
    auto copy = AssetRef(other);
    *this = std::move(copy);
  }

  return *this;
}

auto AssetRef::operator=(AssetRef&& other) noexcept -> AssetRef& {
  if (this != &other) {
    release();
    uuid_ = std::exchange(other.uuid_, UUID(nullptr));
    record_ = std::exchange(other.record_, nullptr);
  }

  return *this;
}

auto AssetRef::acquire(const UUID& uuid) -> AssetRef {
  ZoneScoped;

  auto ref = AssetRef();
  ref.uuid_ = uuid;
  if (!uuid) {
    return ref;
  }

  // a successful load takes the reference this ref then holds
  auto* asset_man = running_asset_manager();
  if (asset_man == nullptr || !asset_man->load_asset(uuid)) {
    return ref;
  }

  if (const auto asset = asset_man->get_asset(uuid); asset && asset->is_loaded()) {
    ref.record_ = asset.value;
  }

  return ref;
}

auto AssetRef::from_loaded(const UUID& uuid) -> AssetRef {
  auto ref = AssetRef();
  ref.uuid_ = uuid;
  ref.point_at_loaded();
  return ref;
}

auto AssetRef::set_unresolved(this AssetRef& self, const UUID& uuid) -> void {
  self.release();
  self.uuid_ = uuid;
}

auto AssetRef::take_over(this AssetRef& self, const UUID& uuid) -> void {
  self.uuid_ = uuid;
  self.record_ = nullptr;
  auto* asset_man = running_asset_manager();
  if (!uuid || asset_man == nullptr) {
    return;
  }

  if (const auto asset = asset_man->get_asset(uuid); asset && asset->is_loaded()) {
    self.record_ = asset.value;
  }
}

auto AssetRef::is_loaded(this const AssetRef& self) -> bool {
  return self.record_ != nullptr && self.record_->is_loaded();
}

auto AssetRef::payload_id(this const AssetRef& self) -> u64 {
  return self.record_ != nullptr ? self.record_->payload_id() : ~0_u64;
}

auto AssetRef::release(this AssetRef& self) -> void {
  if (self.record_ == nullptr) {
    return;
  }

  self.record_ = nullptr;
  App::mod<AssetManager>().unload_asset(self.uuid_);
}

auto AssetRef::point_at_loaded(this AssetRef& self) -> void {
  self.record_ = nullptr;
  auto* asset_man = running_asset_manager();
  if (!self.uuid_ || asset_man == nullptr) {
    return;
  }

  // a copy holds a count only when there is a loaded asset to count, like the ref it copies
  auto asset = asset_man->get_asset(self.uuid_);
  if (!asset || !asset->is_loaded()) {
    return;
  }

  auto* record = asset.value;
  asset_man->acquire_ref(std::move(asset));
  self.record_ = record;
}
} // namespace ox
