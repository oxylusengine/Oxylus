#pragma once

#include <simdjson.h>

#include "Asset/AssetFile.hpp"
#include "Asset/AudioSource.hpp"
#include "Asset/Material.hpp"
#include "Asset/Mesh.hpp"
#include "Asset/Texture.hpp"
#include "Core/ESystem.hpp"
#include "Core/UUID.hpp"
#include "Memory/SlotMap.hpp"
#include "Scene/Scene.hpp"
#include "Utils/JsonWriter.hpp"

namespace ox {
struct Asset {
  UUID uuid = {};
  std::string path = {};
  AssetType type = AssetType::None;
  union {
    MeshID mesh_id = MeshID::Invalid;
    TextureID texture_id;
    MaterialID material_id;
    SceneID scene_id;
    AudioID audio_id;
    ScriptID script_id;
  };

  // Reference count of loads
  u64 ref_count = 0;

  auto is_loaded() const -> bool { return mesh_id != MeshID::Invalid; }

  auto acquire_ref() -> void { ++std::atomic_ref(ref_count); }

  auto release_ref() -> bool { return --std::atomic_ref(ref_count) == 0; }
};

using AssetRegistry = ankerl::unordered_dense::map<UUID, Asset>;
class AssetManager : public ESystem {
public:
  struct AssetMetaFile {
    simdjson::padded_string contents;
    simdjson::ondemand::parser parser;
    simdjson::simdjson_result<simdjson::ondemand::document> doc;
  };

  auto init() -> std::expected<void, std::string> override;
  auto deinit() -> std::expected<void, std::string> override;

  auto registry() const -> const AssetRegistry&;

  auto read_meta_file(const std::string& path) -> std::unique_ptr<AssetMetaFile>;

  auto load_deferred_assets() -> void;

  auto create_asset(AssetType type, const std::string& path = {}) -> UUID;

  auto to_asset_file_type(const std::string& path) -> AssetFileType;
  auto to_asset_type_sv(AssetType type) -> std::string_view;

  auto import_asset(const std::string& path) -> UUID;

  auto delete_asset(const UUID& uuid) -> void;

  //  ── Registered Assets ─────────────────────────────────────────────────
  // Assets that already exist in project root and have meta file with
  // valid UUID's.
  //
  // Add already existing asset into the registry.
  // File must end with `.oxasset` extension.
  auto register_asset(const std::string& path) -> UUID;
  auto register_asset(const UUID& uuid, AssetType type, const std::string& path) -> bool;

  auto export_asset(const UUID& uuid, const std::string& path) -> bool;
  auto export_texture(const UUID& uuid, JsonWriter& writer, const std::string& path) -> bool;
  auto export_mesh(const UUID& uuid, JsonWriter& writer, const std::string& path) -> bool;
  auto export_scene(const UUID& uuid, JsonWriter& writer, const std::string& path) -> bool;
  auto export_material(const UUID& uuid, JsonWriter& writer, const std::string& path) -> bool;
  auto export_script(const UUID& uuid, JsonWriter& writer, const std::string& path) -> bool;

  auto load_asset(const UUID& uuid) -> bool;
  auto unload_asset(const UUID& uuid) -> bool;

  auto load_mesh(const UUID& uuid) -> bool;
  auto unload_mesh(const UUID& uuid) -> bool;

  auto load_texture(const UUID& uuid, const TextureLoadInfo& info = {}) -> bool;
  auto unload_texture(const UUID& uuid) -> bool;
  auto is_texture_loaded(const UUID& uuid) -> bool;

  auto load_material(const UUID& uuid,
                     const Material& material_info,
                     option<ankerl::unordered_dense::map<UUID, TextureLoadInfo>> texture_info_map = nullopt) -> bool;
  auto unload_material(const UUID& uuid) -> bool;

  auto load_scene(const UUID& uuid) -> bool;
  auto unload_scene(const UUID& uuid) -> bool;

  auto load_audio(const UUID& uuid) -> bool;
  auto unload_audio(const UUID& uuid) -> bool;

  auto load_script(const UUID& uuid) -> bool;
  auto unload_script(const UUID& uuid) -> bool;

  auto get_asset(const UUID& uuid) -> Asset*;

  auto get_mesh(const UUID& uuid) -> Mesh*;
  auto get_mesh(MeshID mesh_id) -> Mesh*;

  auto get_texture(const UUID& uuid) -> Texture*;
  auto get_texture(TextureID texture_id) -> Texture*;

  auto get_material(const UUID& uuid) -> Material*;
  auto get_material(MaterialID material_id) -> Material*;
  auto get_materials() -> std::span<Material>;
  auto get_materials_dirty() -> const std::vector<MaterialID>&;
  auto set_material_dirty(MaterialID material_id) -> void;
  auto get_materials_buffer(this AssetManager& self,
                            VkContext& vk_context,
                            vuk::PersistentDescriptorSet& descriptor_set,
                            u32 textures_binding) -> vuk::Value<vuk::Buffer>;

  auto get_scene(const UUID& uuid) -> Scene*;
  auto get_scene(SceneID scene_id) -> Scene*;

  auto get_audio(const UUID& uuid) -> AudioSource*;
  auto get_audio(AudioID audio_id) -> AudioSource*;

  auto get_script(const UUID& uuid) -> LuaSystem*;
  auto get_script(ScriptID script_id) -> LuaSystem*;

private:
  AssetRegistry asset_registry = {};

  std::shared_mutex registry_mutex = {};
  std::shared_mutex textures_mutex = {};
  std::shared_mutex materials_mutex = {};

  vuk::Unique<vuk::Buffer> materials_buffer = vuk::Unique<vuk::Buffer>();
  std::vector<MaterialID> dirty_materials = {};

  SlotMap<Mesh, MeshID> mesh_map = {};
  SlotMap<Texture, TextureID> texture_map = {};
  SlotMap<Material, MaterialID> material_map = {};
  SlotMap<std::unique_ptr<Scene>, SceneID> scene_map = {};
  SlotMap<AudioSource, AudioID> audio_map = {};
  SlotMap<std::unique_ptr<LuaSystem>, ScriptID> script_map = {};

  std::vector<std::function<void()>> deferred_load_queue = {};
};
} // namespace ox
