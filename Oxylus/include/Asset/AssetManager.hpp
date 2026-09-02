#pragma once

#include <condition_variable>
#include <functional>
#include <variant>

#include "Asset/AssetFile.hpp"
#include "Asset/AudioSource.hpp"
#include "Asset/Material.hpp"
#include "Asset/Model.hpp"
#include "Asset/ParticleSystem.hpp"
#include "Asset/TerrainEdits.hpp"
#include "Asset/Texture.hpp"
#include "Core/UUID.hpp"
#include "Memory/ReadGuard.hpp"
#include "Memory/SlotMap.hpp"
#include "Scene/Scene.hpp"
#include "Scripting/LuaScript.hpp"

namespace ox {
struct Asset {
  UUID uuid = {};
  std::filesystem::path path = {};
  AssetType type = AssetType::None;
  union {
    ModelID model_id = ModelID::Invalid;
    TextureID texture_id;
    MaterialID material_id;
    SceneID scene_id;
    AudioID audio_id;
    ScriptID script_id;
    TerrainEditsID terrain_edits_id;
    ParticleSystemID particle_system_id;
  };

  // Reference count of loads
  u64 ref_count = 0;

  auto is_loaded() const -> bool { return model_id != ModelID::Invalid; }

  auto acquire_ref() -> void { ++std::atomic_ref(ref_count); }

  auto release_ref() -> bool { return --std::atomic_ref(ref_count) == 0; }
};

using AssetRegistry = ankerl::unordered_dense::map<UUID, Asset>;

class AssetManager {
public:
  constexpr static auto MODULE_NAME = "AssetManager";

  // `monostate` is 'nothing supplied', which is what makes the pending map's fallback possible.
  using LoadInfo = std::variant<std::monostate, TextureLoadInfo, Material, ModelData, TextureData>;

  static auto to_asset_type_sv(AssetType type) -> std::string_view;

  auto init(this AssetManager& self) -> std::expected<void, std::string>;
  auto deinit(this AssetManager& self) -> std::expected<void, std::string>;

  auto get_registry_snapshot(this AssetManager& self) -> std::vector<Asset>;

  auto create_asset(this AssetManager& self, AssetType type, const std::filesystem::path& path = {}) -> UUID;
  auto delete_asset(this AssetManager& self, const UUID& uuid) -> void;
  auto register_asset(this AssetManager& self, const UUID& uuid, AssetType type, const std::filesystem::path& path)
    -> bool;
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
  auto is_loading(this AssetManager& self, const UUID& uuid) -> bool;

  auto unload_asset(this AssetManager& self, const UUID& uuid) -> void;

  auto is_loaded(this AssetManager& self, const UUID& uuid) -> bool;

  auto get_asset(this AssetManager& self, const UUID& uuid) -> ReadGuard<Asset>;

  auto get_model(this AssetManager& self, const UUID& uuid) -> ReadGuard<Model>;
  auto get_model(this AssetManager& self, ModelID model_id) -> ReadGuard<Model>;

  auto get_texture(this AssetManager& self, const UUID& uuid) -> ReadGuard<Texture>;
  auto get_texture(this AssetManager& self, TextureID texture_id) -> ReadGuard<Texture>;

  auto get_null_material(this AssetManager& self) -> ReadGuard<Asset>;
  auto get_material(this AssetManager& self, const UUID& uuid) -> ReadGuard<Material>;
  auto get_material(this AssetManager& self, MaterialID material_id) -> ReadGuard<Material>;
  auto set_material_dirty(this AssetManager& self, MaterialID material_id) -> void;
  auto set_material_dirty(this AssetManager& self, const UUID& uuid) -> void;
  auto set_all_materials_dirty(this AssetManager& self) -> void;
  auto get_dirty_material_ids(this AssetManager& self) -> std::vector<MaterialID>;

  auto get_scene(this AssetManager& self, const UUID& uuid) -> ReadGuard<Scene>;
  auto get_scene(this AssetManager& self, SceneID scene_id) -> ReadGuard<Scene>;

  auto get_audio(this AssetManager& self, const UUID& uuid) -> ReadGuard<AudioSource>;
  auto get_audio(this AssetManager& self, AudioID audio_id) -> ReadGuard<AudioSource>;

  auto get_script(this AssetManager& self, const UUID& uuid) -> ReadGuard<LuaScript>;
  auto get_script(this AssetManager& self, ScriptID script_id) -> ReadGuard<LuaScript>;

  auto get_terrain_edits(this AssetManager& self, const UUID& uuid) -> ReadGuard<TerrainEdits>;
  auto get_terrain_edits(this AssetManager& self, TerrainEditsID terrain_edits_id) -> ReadGuard<TerrainEdits>;
  auto set_terrain_edits(this AssetManager& self, const UUID& uuid, TerrainEdits&& edits) -> void;

  auto get_particle_system(this AssetManager& self, const UUID& uuid) -> ReadGuard<ParticleSystem>;
  auto get_particle_system(this AssetManager& self, ParticleSystemID particle_system_id) -> ReadGuard<ParticleSystem>;
  auto set_particle_system_dirty(this AssetManager& self, const UUID& uuid) -> void;
  auto edit_particle_system(
    this AssetManager& self, const UUID& uuid, const std::function<void(ParticleSystem&)>& mutate, bool recompile = true
  ) -> void;

private:
  auto load_asset_impl(
    this AssetManager& self, const UUID& uuid, LoadInfo explicit_load, bool should_acquire, bool async
  ) -> bool;

  auto unload_asset_impl(this AssetManager& self, AssetType type, u64 id) -> bool;

  auto load_model(this AssetManager& self, ModelData&& model_data, bool async) -> ModelID;
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

  auto load_script(this AssetManager& self, const std::filesystem::path& path) -> ScriptID;
  auto unload_script(this AssetManager& self, ScriptID script_id) -> bool;

  auto load_terrain_edits(this AssetManager& self, const std::filesystem::path& path) -> TerrainEditsID;
  auto unload_terrain_edits(this AssetManager& self, TerrainEditsID terrain_edits_id) -> bool;

  auto load_particle_system(this AssetManager& self, const std::filesystem::path& path) -> ParticleSystemID;
  auto unload_particle_system(this AssetManager& self, ParticleSystemID particle_system_id) -> bool;

  AssetRegistry asset_registry = {};

  std::shared_mutex registry_mutex = {};
  std::shared_mutex models_mutex = {};
  std::shared_mutex textures_mutex = {};
  std::shared_mutex materials_mutex = {};
  std::shared_mutex scenes_mutex = {};
  std::shared_mutex audio_mutex = {};
  std::shared_mutex scripts_mutex = {};
  std::shared_mutex terrain_edits_mutex = {};
  std::shared_mutex particle_systems_mutex = {};

  std::vector<MaterialID> dirty_materials = {};

  std::shared_mutex loading_mutex = {};
  ankerl::unordered_dense::set<UUID> loading_assets = {};

  std::shared_mutex pending_load_info_mutex = {};
  ankerl::unordered_dense::map<UUID, LoadInfo> pending_load_info = {};

  std::mutex model_load_mutex = {};
  std::condition_variable model_load_cv = {};

  SlotMap<Model, ModelID> model_map = {};
  SlotMap<Texture, TextureID> texture_map = {};
  SlotMap<Material, MaterialID> material_map = {};
  SlotMap<std::unique_ptr<Scene>, SceneID> scene_map = {};
  SlotMap<AudioSource, AudioID> audio_map = {};
  SlotMap<std::unique_ptr<LuaScript>, ScriptID> script_map = {};
  SlotMap<TerrainEdits, TerrainEditsID> terrain_edits_map = {};
  SlotMap<ParticleSystem, ParticleSystemID> particle_system_map = {};

  UUID null_material = {};
};
} // namespace ox
