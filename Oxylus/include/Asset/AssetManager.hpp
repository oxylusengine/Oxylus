#pragma once

#include <simdjson.h>

#include "Asset/AssetFile.hpp"
#include "Asset/AudioSource.hpp"
#include "Asset/Material.hpp"
#include "Asset/Model.hpp"
#include "Asset/Texture.hpp"
#include "Core/UUID.hpp"
#include "Memory/ReadGuard.hpp"
#include "Memory/SlotMap.hpp"
#include "Scene/Scene.hpp"
#include "Scripting/LuaSystem.hpp"

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

  auto init(this AssetManager& self) -> std::expected<void, std::string>;
  auto deinit(this AssetManager& self) -> std::expected<void, std::string>;

  auto registry(this const AssetManager& self) -> const AssetRegistry&;

  auto create_asset(this AssetManager& self, AssetType type, const std::filesystem::path& path = {}) -> UUID;
  auto import_asset(this AssetManager& self, const std::filesystem::path& path) -> UUID;
  auto delete_asset(this AssetManager& self, const UUID& uuid) -> void;
  auto register_asset(this AssetManager& self, const UUID& uuid, AssetType type, const std::filesystem::path& path)
    -> bool;
  auto acquire_ref(this AssetManager& self, const UUID& uuid) -> void;
  auto unload(this AssetManager& self, const UUID& uuid) -> void;

  // auto export_asset(this AssetManager& self, const UUID& uuid, const std::filesystem::path& path) -> bool;
  // auto export_scene(this AssetManager& self, const UUID& uuid, JsonWriter& writer, const std::filesystem::path& path)
  //   -> bool;
  // auto export_material(this AssetManager& self, const UUID& uuid, JsonWriter& writer, const std::filesystem::path&
  // path)
  //   -> bool;
  // auto export_script(this AssetManager& self, const UUID& uuid, JsonWriter& writer, const std::filesystem::path&
  // path)
  //   -> bool;

  auto load_asset(this AssetManager& self, const UUID& uuid) -> bool;
  auto unload_asset(this AssetManager& self, const UUID& uuid) -> bool;

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

  auto get_script(this AssetManager& self, const UUID& uuid) -> ReadGuard<LuaSystem>;
  auto get_script(this AssetManager& self, ScriptID script_id) -> ReadGuard<LuaSystem>;

  auto is_texture_loaded(this AssetManager& self, const UUID& uuid) -> bool;

private:
  auto load_model(this AssetManager& self, const std::filesystem::path &path) -> ModelID;
  auto unload_model(this AssetManager& self, ReadGuard<Asset> asset) -> bool;

  auto load_texture(this AssetManager& self, const std::filesystem::path &path) -> TextureID;
  auto unload_texture(this AssetManager& self, ReadGuard<Asset> asset) -> bool;

  auto load_material(this AssetManager& self, const std::filesystem::path &path) -> MaterialID;
  auto unload_material(this AssetManager& self, ReadGuard<Asset> asset) -> bool;

  auto load_scene(this AssetManager& self, const std::filesystem::path &path) -> SceneID;
  auto unload_scene(this AssetManager& self, ReadGuard<Asset> asset) -> bool;

  auto load_audio(this AssetManager& self, const std::filesystem::path &path) -> AudioID;
  auto unload_audio(this AssetManager& self, ReadGuard<Asset> asset) -> bool;

  auto load_script(this AssetManager& self, const std::filesystem::path &path) -> ScriptID;
  auto unload_script(this AssetManager& self, ReadGuard<Asset> asset) -> bool;

  AssetRegistry asset_registry = {};

  std::shared_mutex registry_mutex = {};
  std::shared_mutex models_mutex = {};
  std::shared_mutex textures_mutex = {};
  std::shared_mutex materials_mutex = {};
  std::shared_mutex scenes_mutex = {};
  std::shared_mutex audio_mutex = {};
  std::shared_mutex scripts_mutex = {};

  std::vector<MaterialID> dirty_materials = {};

  SlotMap<Model, ModelID> model_map = {};
  SlotMap<Texture, TextureID> texture_map = {};
  SlotMap<Material, MaterialID> material_map = {};
  SlotMap<std::unique_ptr<Scene>, SceneID> scene_map = {};
  SlotMap<AudioSource, AudioID> audio_map = {};
  SlotMap<std::unique_ptr<LuaSystem>, ScriptID> script_map = {};

  UUID null_material = {};
};
} // namespace ox
