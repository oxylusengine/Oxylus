#pragma once

#include <ankerl/svector.h>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <tuple>
#include <variant>

#include "Animation/AnimationClip.hpp"
#include "Animation/Skeleton.hpp"
#include "Asset/AssetFile.hpp"
#include "Asset/AssetHandle.hpp"
#include "Asset/AssetStorage.hpp"
#include "Asset/AssetView.hpp"
#include "Asset/AudioSource.hpp"
#include "Asset/Material.hpp"
#include "Asset/Model.hpp"
#include "Asset/ParticleSystem.hpp"
#include "Asset/TerrainEdits.hpp"
#include "Asset/Texture.hpp"
#include "Cinematic/Cinematic.hpp"
#include "Core/UUID.hpp"
#include "Memory/ReadGuard.hpp"
#include "Memory/SlotMap.hpp"
#include "Scene/Scene.hpp"

namespace ox {
class AudioEngine;
struct StaticBLASBudget;

struct Asset {
  UUID uuid = {};
  // both are VFS virtual paths (see `VFS::to_virtual`), so a registry entry means the same file in the editor and in
  // a shipped game. `path` is where the payload loads from, a cooked asset's `.oxpack` rather than its source
  std::filesystem::path path = {};
  // the file this asset was imported from and what `find_asset` looks up. Empty for assets with no file of their
  // own, like the materials and embedded textures a model brings with it
  std::filesystem::path source_path = {};
  AssetType type = AssetType::None;
  union {
    ModelID model_id = ModelID::Invalid;
    TextureID texture_id;
    MaterialID material_id;
    SceneID scene_id;
    AudioID audio_id;
    TerrainEditsID terrain_edits_id;
    ParticleSystemID particle_system_id;
    SkeletonID skeleton_id;
    AnimationID animation_id;
    CinematicID cinematic_id;
    u64 payload_raw;
  };

  // Reference count of loads
  u64 ref_count = 0;

  auto is_loaded(this const Asset& self) -> bool { return self.payload_id() != ~0_u64; }

  auto acquire_ref() -> void { ++std::atomic_ref(ref_count); }

  auto release_ref() -> bool { return --std::atomic_ref(ref_count) == 0; }

  // written only under the registry's write lock, but an `AssetRef` reads it without the registry
  // lock, so both sides go through an atomic
  auto payload_id(this const Asset& self) -> u64 {
    return std::atomic_ref(const_cast<u64&>(self.payload_raw)).load(std::memory_order_acquire);
  }

  auto set_payload_id(this Asset& self, u64 id) -> void {
    std::atomic_ref(self.payload_raw).store(id, std::memory_order_release);
  }

  // built by a worker and waiting for the next `AssetManager::commit` to publish it, so readers do not
  // see it yet. Only touched under the registry's write lock
  u64 staged_raw = ~0_u64;

  auto is_staged(this const Asset& self) -> bool { return self.staged_raw != ~0_u64; }
};

// boxed, so a record keeps its address however the map rehashes and an `AssetRef` can point at it
using AssetRegistry = ankerl::unordered_dense::map<UUID, std::unique_ptr<Asset>>;

class AssetManager {
public:
  constexpr static auto MODULE_NAME = "AssetManager";
  // audio assets hold sounds of the engine, so it has to outlive them
  using module_dependencies = std::tuple<AudioEngine>;

  // `monostate` is 'nothing supplied', which is what makes the pending map's fallback possible.
  using LoadInfo = std::variant<std::monostate, TextureLoadInfo, Material, ModelData, TextureData>;

  static auto to_asset_type_sv(AssetType type) -> std::string_view;

  auto init(this AssetManager& self) -> std::expected<void, std::string>;
  auto deinit(this AssetManager& self) -> std::expected<void, std::string>;

  auto get_registry_snapshot(this AssetManager& self) -> std::vector<Asset>;

  // paths may be physical or virtual, they are stored virtual
  auto create_asset(this AssetManager& self, AssetType type, const std::filesystem::path& path = {}) -> UUID;
  auto delete_asset(this AssetManager& self, const UUID& uuid) -> void;
  auto register_asset(
    this AssetManager& self,
    const UUID& uuid,
    AssetType type,
    const std::filesystem::path& path,
    const std::filesystem::path& source_path = {}
  ) -> bool;
  auto update_asset_path(
    this AssetManager& self,
    const UUID& uuid,
    const std::filesystem::path& path,
    const std::filesystem::path& source_path
  ) -> bool;

  // registers everything an exported manifest lists, the way a shipped game learns its assets
  auto load_manifest(this AssetManager& self, const std::filesystem::path& path) -> bool;

  // the asset imported from `source_path` (physical or virtual), or a null UUID. Does not load or acquire it
  auto find_asset(this AssetManager& self, const std::filesystem::path& source_path) -> UUID;
  auto acquire_ref(this AssetManager& self, ReadGuard<Asset> asset) -> void;
  auto release_ref(this AssetManager& self, ReadGuard<Asset> asset) -> void;

  // What an asset should be loaded with when nothing is handed to `load_asset` at the call site.
  // The engine cannot discover it, a material's definition lives in the editor's sidecar, so
  // whoever registers the asset pushes it here first. Entries survive a load, so unload/reload works.
  auto set_pending_load_info(this AssetManager& self, const UUID& uuid, LoadInfo info) -> void;
  auto get_pending_load_info(this AssetManager& self, const UUID& uuid) -> LoadInfo;
  auto clear_pending_load_info(this AssetManager& self, const UUID& uuid) -> void;

  auto load_asset(this AssetManager& self, const UUID& uuid, LoadInfo explicit_load = {}, bool should_acquire = true)
    -> bool;

  auto load_asset_async(this AssetManager& self, const UUID& uuid, LoadInfo explicit_load = {}) -> bool;
  // publishes what workers loaded since the last commit, in the order they finished, so children land
  // before the parents that reference them, then unloads whatever dropped to zero references and is
  // still there. Runs on the main thread once per frame, before any module updates, so asset state
  // never changes under a frame in progress and a payload read this frame outlives the frame
  auto commit(this AssetManager& self) -> void;
  // unloads what dropped to zero references now instead of at the next commit. Payloads it unloads are
  // only retired, so views read this frame stay valid. Main thread only
  auto flush_unloads(this AssetManager& self) -> void;
  auto is_loading(this AssetManager& self, const UUID& uuid) -> bool;

  auto unload_asset(this AssetManager& self, const UUID& uuid) -> void;

  auto is_loaded(this AssetManager& self, const UUID& uuid) -> bool;

  auto get_asset(this AssetManager& self, const UUID& uuid) -> ReadGuard<Asset>;

  auto get_model(this AssetManager& self, const UUID& uuid) -> AssetView<Model>;
  auto get_model(this AssetManager& self, ModelID model_id) -> AssetView<Model>;
  // the device address of a mesh's ray tracing structure, 0 while it has none. Static structures are built here and
  // nowhere else: a ready mesh without one gets it first when `budget` admits it, blocking until it is done. A null
  // `budget` builds nothing
  static auto resolve_mesh_blas(
    RenderContext& render_context, const Model& model, usize mesh_index, StaticBLASBudget* budget
  ) -> u64;

  auto get_texture(this AssetManager& self, const UUID& uuid) -> AssetView<Texture>;
  auto get_texture(this AssetManager& self, TextureID texture_id) -> AssetView<Texture>;

  auto get_null_material(this AssetManager& self) -> ReadGuard<Asset>;
  auto get_material(this AssetManager& self, const UUID& uuid) -> AssetView<Material>;
  auto get_material(this AssetManager& self, MaterialID material_id) -> AssetView<Material>;
  // replaces a loaded material's parameters and marks it dirty. Every texture it newly names is loaded and gets as many
  // references as the material has holders, the ones it stops naming give those back, the way releasing the material
  // would. False when `uuid` isn't a loaded material or a new texture won't load
  // replaces the payload, so readers of this frame keep the version they read
  auto set_material(this AssetManager& self, const UUID& uuid, const Material& material) -> bool;
  // takes a reference on every child the payload names now and gives back the ones it held before, in
  // that order, so a child that stays never drops to zero. Whoever writes a payload's child uuids in
  // place calls it afterwards. No asset lock may be held
  auto hold_children(this AssetManager& self, const UUID& uuid) -> void;
  auto set_material_dirty(this AssetManager& self, MaterialID material_id) -> void;
  auto set_material_dirty(this AssetManager& self, const UUID& uuid) -> void;
  auto set_all_materials_dirty(this AssetManager& self) -> void;
  auto get_dirty_material_ids(this AssetManager& self) -> std::vector<MaterialID>;
  auto get_materials_revision(this AssetManager& self) -> u32;
  template <typename Func>
  auto for_each_material_index(this AssetManager& self, Func&& func) -> void {
    self.material_storage.for_each_index([&func](u32 index) { func(static_cast<usize>(index)); });
  }

  auto get_scene(this AssetManager& self, const UUID& uuid) -> AssetView<Scene>;
  auto get_scene(this AssetManager& self, SceneID scene_id) -> AssetView<Scene>;

  auto get_audio(this AssetManager& self, const UUID& uuid) -> AssetView<AudioSource>;
  auto get_audio(this AssetManager& self, AudioID audio_id) -> AssetView<AudioSource>;

  auto get_terrain_edits(this AssetManager& self, const UUID& uuid) -> AssetView<TerrainEdits>;
  auto get_terrain_edits(this AssetManager& self, TerrainEditsID terrain_edits_id) -> AssetView<TerrainEdits>;
  auto set_terrain_edits(this AssetManager& self, const UUID& uuid, TerrainEdits&& edits) -> void;

  // these have no file of their own, so publishing is how the importer's payload enters the
  // registry, and it takes no reference because holders acquire
  auto publish_skeleton(this AssetManager& self, const UUID& uuid, Skeleton&& skeleton) -> bool;
  auto publish_animation(this AssetManager& self, const UUID& uuid, AnimationClip&& clip) -> bool;
  // for payloads built in code instead of read from the asset's file
  auto publish_texture(this AssetManager& self, const UUID& uuid, Texture&& texture) -> bool;
  auto publish_model(this AssetManager& self, const UUID& uuid, Model&& model) -> bool;
  // a model with no file of its own, built from `model_data` (see build_runtime_model) and loaded before this returns.
  // The caller holds one reference and gives it back with unload_asset, the asset is erased once nothing holds it.
  // Needs the renderer, a null UUID without one
  auto create_model(this AssetManager& self, ModelData&& model_data) -> UUID;

  auto get_skeleton(this AssetManager& self, const UUID& uuid) -> AssetView<Skeleton>;
  auto get_skeleton(this AssetManager& self, SkeletonID skeleton_id) -> AssetView<Skeleton>;

  auto get_animation(this AssetManager& self, const UUID& uuid) -> AssetView<AnimationClip>;
  auto get_animation(this AssetManager& self, AnimationID animation_id) -> AssetView<AnimationClip>;
  auto get_particle_system(this AssetManager& self, const UUID& uuid) -> AssetView<ParticleSystem>;
  auto get_particle_system(this AssetManager& self, ParticleSystemID particle_system_id) -> AssetView<ParticleSystem>;
  // recompiles into a new version that replaces the payload
  auto set_particle_system_dirty(this AssetManager& self, const UUID& uuid) -> void;
  auto edit_particle_system(
    this AssetManager& self, const UUID& uuid, const std::function<void(ParticleSystem&)>& mutate, bool recompile = true
  ) -> void;

  auto get_cinematic(this AssetManager& self, const UUID& uuid) -> AssetView<Cinematic>;
  auto get_cinematic(this AssetManager& self, CinematicID cinematic_id) -> AssetView<Cinematic>;
  // `mutate` edits a copy, which then replaces the payload, so readers of this frame keep the old one
  auto edit_cinematic(this AssetManager& self, const UUID& uuid, const std::function<void(Cinematic&)>& mutate) -> void;

private:
  auto load_asset_impl(
    this AssetManager& self, const UUID& uuid, LoadInfo explicit_load, bool should_acquire, bool async
  ) -> bool;

  // the storage owner for a model, whose GPU resources go with it when the commit destroys it
  static auto model_payload(Model&& model) -> std::shared_ptr<const Model>;
  enum class PublishResult : u8 { Published, Staged, Rejected };
  // the one place a payload enters the registry: published straight away on the main thread, staged for
  // the next commit on any other. `type` None accepts any record type. Rejected means the record is gone,
  // has another type, or is already loaded or staged, and the caller drops its payload
  auto publish_payload(this AssetManager& self, const UUID& uuid, AssetType type, u64 payload_id) -> PublishResult;
  // false when the record no longer stages `payload_id`
  auto publish_staged(this AssetManager& self, const UUID& uuid, u64 payload_id) -> bool;
  auto retire(this AssetManager& self, std::shared_ptr<const void> payload) -> void;
  // unloads at once if the asset is loaded and nothing references it. The commit's path, and
  // `delete_asset`'s, which cannot wait for one
  auto unload_now(this AssetManager& self, const UUID& uuid) -> void;
  auto child_uuids(this AssetManager& self, const UUID& uuid) -> ankerl::svector<UUID, 8>;
  auto drop_children(this AssetManager& self, const UUID& uuid) -> void;
  // both under the registry's write lock. Insert hands back null when the uuid is registered already
  auto insert_record(this AssetManager& self, const UUID& uuid) -> Asset*;
  auto retire_record(this AssetManager& self, AssetRegistry::iterator it) -> void;
  auto unload_asset_impl(this AssetManager& self, AssetType type, u64 id) -> bool;

  // the model whose pack a skeleton or a clip was published from, which is the only thing that can
  // materialize one
  auto source_model_uuid(this AssetManager& self, const std::filesystem::path& path) -> UUID;

  // `model_path` is the pack the data came from, which is what the skeleton and the clips it
  // publishes get registered against, since neither has a file of its own
  auto load_model(this AssetManager& self, ModelData&& model_data, const std::filesystem::path& model_path, bool async)
    -> ModelID;
  // Unpacks the compiled model out of the `.oxpack` at `path`.
  auto load_model(this AssetManager& self, const std::filesystem::path& path, bool async) -> ModelID;
  auto unload_model(this AssetManager& self, ModelID model_id) -> bool;
  // Blocks until every mesh job of the model has finished.
  auto wait_until_model_loaded(this AssetManager& self, ModelID model_id) -> void;
  auto notify_model_loaded(this AssetManager& self) -> void;

  auto load_texture(this AssetManager& self, const std::filesystem::path& path, TextureLoadInfo info = {}) -> TextureID;
  auto load_texture(this AssetManager& self, const TextureData& data, const TextureLoadInfo& info) -> TextureID;
  auto unload_texture(this AssetManager& self, TextureID texture_id) -> bool;

  auto load_material(this AssetManager& self, const std::filesystem::path& path, const Material& info = {})
    -> MaterialID;
  auto unload_material(this AssetManager& self, MaterialID material_id) -> bool;

  auto load_scene(this AssetManager& self, const std::filesystem::path& path) -> SceneID;
  auto unload_scene(this AssetManager& self, SceneID scene_id) -> bool;

  auto load_audio(this AssetManager& self, const std::filesystem::path& path) -> AudioID;
  auto unload_audio(this AssetManager& self, AudioID audio_id) -> bool;

  auto load_terrain_edits(this AssetManager& self, const std::filesystem::path& path) -> TerrainEditsID;
  auto unload_terrain_edits(this AssetManager& self, TerrainEditsID terrain_edits_id) -> bool;

  auto load_skeleton(this AssetManager& self, Skeleton&& skeleton) -> SkeletonID;
  auto unload_skeleton(this AssetManager& self, SkeletonID skeleton_id) -> bool;

  auto load_animation(this AssetManager& self, AnimationClip&& clip) -> AnimationID;
  auto unload_animation(this AssetManager& self, AnimationID animation_id) -> bool;

  auto load_particle_system(this AssetManager& self, const std::filesystem::path& path) -> ParticleSystemID;
  auto unload_particle_system(this AssetManager& self, ParticleSystemID particle_system_id) -> bool;

  auto load_cinematic(this AssetManager& self, const std::filesystem::path& path) -> CinematicID;
  auto unload_cinematic(this AssetManager& self, CinematicID cinematic_id) -> bool;

  AssetRegistry asset_registry = {};
  // generic-string keys: native separators differ per platform and would hash apart
  ankerl::unordered_dense::map<std::string, UUID> source_index = {};

  // records erased from the registry, kept alive because a handle may still point at one. Registering
  // the uuid again brings its record back, so those handles see the asset again
  ankerl::unordered_dense::map<UUID, std::unique_ptr<Asset>> retired_records = {};

  // guards `asset_registry`, `retired_records` and `source_index`
  std::shared_mutex registry_mutex = {};

  // what each loaded asset references: a material its textures, a model its materials, skeleton and
  // clips, a clip its skeleton, a particle system its material and mesh. Taken when the asset is
  // published and given back when it unloads, one reference each however many hold the parent
  ankerl::unordered_dense::map<UUID, ankerl::svector<AssetRef, 4>> child_refs = {};
  // every section changes the map, there are no readers to share it with
  std::mutex child_refs_mutex = {};

  struct StagedPayload {
    UUID uuid = {};
    AssetType type = AssetType::None;
    u64 payload_id = ~0_u64;
  };
  std::vector<StagedPayload> staged_payloads = {};
  // only pushed to and swapped out, never read in place
  std::mutex staged_payloads_mutex = {};

  // assets whose count reached zero since the last commit, which unloads the ones still at zero
  std::vector<UUID> pending_unloads = {};
  // only pushed to and swapped out, never read in place
  std::mutex pending_unloads_mutex = {};

  std::vector<MaterialID> dirty_materials = {};
  // guards `dirty_materials`, every section changes it
  std::mutex dirty_materials_mutex = {};
  // bumped whenever a material is created, changed or destroyed
  u32 materials_revision = 0;

  std::shared_mutex loading_mutex = {};
  ankerl::unordered_dense::set<UUID> loading_assets = {};

  std::shared_mutex pending_load_info_mutex = {};
  ankerl::unordered_dense::map<UUID, LoadInfo> pending_load_info = {};

  std::mutex model_load_mutex = {};
  std::condition_variable model_load_cv = {};

  // each one is only touched under its matching `*_mutex` above, so they skip a lock of their own
  // immutable once published and read without a lock, see `retire`
  AssetStorage<AudioSource, AudioID> audio_storage = {};
  AssetStorage<Skeleton, SkeletonID> skeleton_storage = {};
  AssetStorage<AnimationClip, AnimationID> animation_storage = {};
  AssetStorage<Model, ModelID> model_storage = {};
  AssetStorage<Texture, TextureID> texture_storage = {};
  // a material's slot index is its index in the renderer's GPU material buffer
  AssetStorage<Material, MaterialID> material_storage = {};
  AssetStorage<Cinematic, CinematicID> cinematic_storage = {};
  AssetStorage<TerrainEdits, TerrainEditsID> terrain_edits_storage = {};
  AssetStorage<ParticleSystem, ParticleSystemID> particle_system_storage = {};
  // a scene never moves, its observers hold its address, so its slot adopts the allocation it was built in
  AssetStorage<Scene, SceneID> scene_storage = {};

  // payloads replaced or destroyed since the last commit, which destroys them: by then no reader of the
  // frame they were retired in can still hold a pointer to one. Always destroyed on the main thread
  std::vector<std::shared_ptr<const void>> retired_payloads = {};
  // only pushed to and swapped out, never read in place
  std::mutex retired_payloads_mutex = {};

  UUID null_material = {};
};
} // namespace ox
