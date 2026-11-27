#pragma once

#include <utility>

#include "Core/UUID.hpp"
#include "Memory/ReadGuard.hpp"

namespace ox {
struct Asset;

// marks the flecs type of every `AssetHandle<T>`, so reflection can treat them all as an `AssetRef`
struct AssetRefTag {};

// one reference on an asset, given back when the ref goes away. It holds a count only while it
// points at a record, which happens when the asset was loaded at the time the ref was made. It
// keeps its uuid either way, so a scene saves a reference that failed to load. Records never move
// and are never freed before the asset manager, so reading through one skips the registry lookup
class AssetRef {
public:
  AssetRef() = default;
  AssetRef(const AssetRef& other);
  AssetRef(AssetRef&& other) noexcept;
  ~AssetRef();

  auto operator=(const AssetRef& other) -> AssetRef&;
  auto operator=(AssetRef&& other) noexcept -> AssetRef&;

  // loads the asset first when it is not loaded
  static auto acquire(const UUID& uuid) -> AssetRef;
  // never loads: holds a reference only when the asset is loaded already, for code that may not
  // re-enter a load, like spawning from a model whose read guard it still holds
  static auto from_loaded(const UUID& uuid) -> AssetRef;

  // names `uuid` and holds nothing yet, for the scene loader, which loads assets in its own order and
  // then hands each such ref the reference its load took with `take_over`
  auto set_unresolved(this AssetRef& self, const UUID& uuid) -> void;

  // for code that already moved the counts itself: the caller holds one count on `uuid` that this
  // ref takes over, and already gave back the one this ref held
  // TODO: remove once the inspector's asset field works on refs instead of uuids
  auto take_over(this AssetRef& self, const UUID& uuid) -> void;

  auto uuid(this const AssetRef& self) -> const UUID& { return self.uuid_; }
  auto is_loaded(this const AssetRef& self) -> bool;
  // the loaded payload's slot map id as raw bits, ~0 when not loaded. For a material its slot index
  // is the GPU material index
  auto payload_id(this const AssetRef& self) -> u64;
  explicit operator bool() const { return static_cast<bool>(uuid_); }

protected:
  auto record(this const AssetRef& self) -> const Asset* { return self.record_; }

private:
  auto release(this AssetRef& self) -> void;
  auto point_at_loaded(this AssetRef& self) -> void;

  // first, so a reflected field reads as a uuid from C# and the scene format
  UUID uuid_ = {};
  Asset* record_ = nullptr;
};

template <typename T>
auto resolve_asset(const Asset* record) -> ReadGuard<T>;

template <typename T>
class AssetHandle : public AssetRef {
public:
  AssetHandle() = default;
  explicit AssetHandle(AssetRef ref) : AssetRef(std::move(ref)) {}

  static auto acquire(const UUID& uuid) -> AssetHandle { return AssetHandle(AssetRef::acquire(uuid)); }
  static auto from_loaded(const UUID& uuid) -> AssetHandle { return AssetHandle(AssetRef::from_loaded(uuid)); }

  // holds the payload's lock like every asset accessor, null when the asset is not loaded or is not a `T`
  auto get(this const AssetHandle& self) -> ReadGuard<T> { return resolve_asset<T>(self.record()); }
};
} // namespace ox
