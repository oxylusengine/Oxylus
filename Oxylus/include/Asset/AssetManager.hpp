#pragma once

#include "Asset/AssetMetadata.hpp"
#include "Asset/AudioSource.hpp"
#include "Asset/Material.hpp"
#include "Asset/Model.hpp"
#include "Asset/Texture.hpp"
#include "Core/UUID.hpp"
#include "Memory/Borrowed.hpp"
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

  static auto to_file_format(const std::filesystem::path& path) -> FileFormat;
  static auto to_asset_type_sv(AssetType type) -> std::string_view;

  auto init(this AssetManager&) -> std::expected<void, std::string>;
  auto deinit(this AssetManager&) -> std::expected<void, std::string>;

  auto asset_root_path(this AssetManager&, AssetType type) -> std::filesystem::path;
  auto get_registry(this AssetManager&) -> const AssetRegistry&;

  //  ── Created Assets ──────────────────────────────────────────────────
  // Assets that will be created and asinged new UUID.
  // All created assets will be automatically registered into the registry.
  //
  auto create_asset(this AssetManager&, AssetType type, const std::filesystem::path& path = {}) -> UUID;

  //  ── Registered Assets ─────────────────────────────────────────────────
  // Assets that already exist in project root and have meta file with
  // valid UUID's.
  //
  // Add already existing asset into the registry.
  // File must end with `.oxasset` extension.
  auto register_asset(this AssetManager&, const std::filesystem::path& path) -> UUID;
  auto register_asset(this AssetManager&, const UUID& uuid, AssetType type, const std::filesystem::path& path) -> bool;

  //  ── Load Assets ─────────────────────────────────────────────────────
  // Load contents of __registered__ assets.
  //
  auto load_asset(this AssetManager&, const UUID& uuid) -> bool;
  auto unload_asset(this AssetManager&, const UUID& uuid) -> bool;

  auto delete_asset(this AssetManager&, const UUID& uuid) -> void;
  auto is_valid(this AssetManager&, const UUID& uuid) -> bool;
  auto is_loaded(this AssetManager&, const UUID& uuid) -> bool;

  auto get_asset(this AssetManager&, const UUID& uuid) -> Borrowed<Asset>;

  auto get_model(this AssetManager&, const UUID& uuid) -> Model*;
  auto get_model(this AssetManager&, ModelID mesh_id) -> Model*;

  auto get_texture(this AssetManager&, const UUID& uuid) -> Borrowed<Texture>;
  auto get_texture(this AssetManager&, TextureID texture_id) -> Borrowed<Texture>;

  auto get_material(this AssetManager&, const UUID& uuid) -> Borrowed<Material>;
  auto get_material(this AssetManager&, MaterialID material_id) -> Borrowed<Material>;
  auto set_material_dirty(this AssetManager&, MaterialID material_id) -> void;
  auto set_material_dirty(this AssetManager&, const UUID& uuid) -> void;
  auto get_dirty_material_ids(this AssetManager&) -> std::vector<MaterialID>;

  auto get_scene(this AssetManager&, const UUID& uuid) -> Scene*;
  auto get_scene(this AssetManager&, SceneID scene_id) -> Scene*;

  auto get_audio(this AssetManager&, const UUID& uuid) -> AudioSource*;
  auto get_audio(this AssetManager&, AudioID audio_id) -> AudioSource*;

  auto get_script(this AssetManager&, const UUID& uuid) -> LuaSystem*;
  auto get_script(this AssetManager&, ScriptID script_id) -> LuaSystem*;

private:
  AssetRegistry asset_registry = {};

  std::shared_mutex registry_mutex = {};
  std::shared_mutex textures_mutex = {};
  std::shared_mutex materials_mutex = {};

  std::vector<MaterialID> dirty_materials = {};

  SlotMap<Model, ModelID> model_map = {};
  SlotMap<Texture, TextureID> texture_map = {};
  SlotMap<Material, MaterialID> material_map = {};
  SlotMap<std::unique_ptr<Scene>, SceneID> scene_map = {};
  SlotMap<AudioSource, AudioID> audio_map = {};
  SlotMap<std::unique_ptr<LuaSystem>, ScriptID> script_map = {};
};
} // namespace ox
