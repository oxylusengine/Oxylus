#include "Asset/AssetManager.hpp"

#include <ankerl/svector.h>
#include <array>
#include <atomic>
#include <vuk/Types.hpp>
#include <vuk/vsl/Core.hpp>
#include <zpp_bits.h>

#include "Core/App.hpp"
#include "Memory/Hasher.hpp"
#include "Memory/Stack.hpp"
#include "OS/File.hpp"
#include "Scripting/LuaScript.hpp"
#include "Utils/Log.hpp"

namespace ox {
auto AssetManager::init(this AssetManager& self) -> std::expected<void, std::string> {
  ZoneScoped;

  self.null_material = self.create_asset(AssetType::Material);
  self.load_asset(self.null_material, {});

  return {};
}

auto AssetManager::deinit(this AssetManager& self) -> std::expected<void, std::string> {
  ZoneScoped;

  for (auto& [uuid, asset] : self.asset_registry) {
    if (asset.is_loaded() && asset.ref_count != 0) {
      OX_LOG_WARN(
        "A {} asset ({}, {}) with refcount of {} is still alive!",
        to_asset_type_sv(asset.type),
        uuid.str(),
        asset.path,
        asset.ref_count
      );
    }
  }

  self.asset_registry.clear();
  self.pending_load_info.clear();
  self.dirty_materials.clear();
  self.model_map.reset();
  self.texture_map.reset();
  self.material_map.reset();
  self.scene_map.reset();
  self.audio_map.reset();
  self.script_map.reset();
  self.terrain_edits_map.reset();
  self.particle_system_map.reset();

  return {};
}

auto AssetManager::get_registry_snapshot(this AssetManager& self) -> std::vector<Asset> {
  ZoneScoped;

  auto read_lock = std::shared_lock(self.registry_mutex);
  auto snapshot = std::vector<Asset>{};
  snapshot.reserve(self.asset_registry.size());
  for (const auto& [uuid, asset] : self.asset_registry) {
    snapshot.emplace_back(asset);
  }

  return snapshot;
}

auto AssetManager::to_asset_type_sv(AssetType type) -> std::string_view {
  ZoneScoped;

  switch (type) {
    case AssetType::None          : return "None";
    case AssetType::Shader        : return "Shader";
    case AssetType::Model         : return "Model";
    case AssetType::Texture       : return "Texture";
    case AssetType::Material      : return "Material";
    case AssetType::Font          : return "Font";
    case AssetType::Scene         : return "Scene";
    case AssetType::Audio         : return "Audio";
    case AssetType::Script        : return "Script";
    case AssetType::Terrain       : return "Terrain";
    case AssetType::ParticleSystem: return "ParticleSystem";
    default                       : return {};
  }
}

auto AssetManager::create_asset(this AssetManager& self, const AssetType type, const std::filesystem::path& path)
  -> UUID {
  const auto uuid = UUID::generate_random();
  auto write_lock = std::unique_lock(self.registry_mutex);
  auto [asset_it, inserted] = self.asset_registry.try_emplace(uuid);
  if (!inserted) {
    OX_LOG_ERROR("Can't create asset {}!", uuid.str());
    return UUID(nullptr);
  }

  auto& asset = asset_it->second;
  asset.uuid = uuid;
  asset.type = type;
  asset.path = path;

  return asset.uuid;
}

auto AssetManager::delete_asset(this AssetManager& self, const UUID& uuid) -> void {
  ZoneScoped;

  auto is_loaded = false;
  auto ref_count = 0_u64;
  {
    auto asset = self.get_asset(uuid);
    if (!asset)
      return;

    ref_count = asset->ref_count;
    is_loaded = asset->is_loaded();

    if (ref_count > 0) {
      OX_LOG_WARN("Deleting alive asset {} with {} references!", asset->uuid.str(), ref_count);
    }
  }

  if (is_loaded) {
    {
      auto asset = self.get_asset(uuid);
      asset->ref_count = ox::min(asset->ref_count, 1_u64);
    }
    self.unload_asset(uuid);
  }

  {
    auto write_lock = std::unique_lock(self.registry_mutex);
    self.asset_registry.erase(uuid);
  }

  self.clear_pending_load_info(uuid);

  OX_LOG_INFO("Deleted asset {}.", uuid.str());
}

auto AssetManager::register_asset(
  this AssetManager& self, const UUID& uuid, AssetType type, const std::filesystem::path& path
) -> bool {
  ZoneScoped;

  auto write_lock = std::unique_lock(self.registry_mutex);

  auto [asset_it, inserted] = self.asset_registry.try_emplace(uuid);
  if (!inserted) {
    if (asset_it != self.asset_registry.end()) {
      return true;
    }
    return false;
  }

  auto& asset = asset_it->second;
  asset.uuid = uuid;
  asset.path = path;
  asset.type = type;

  OX_LOG_INFO("Registered new asset: {}:{}", to_asset_type_sv(asset.type), uuid.str());

  return true;
}

auto AssetManager::set_pending_load_info(this AssetManager& self, const UUID& uuid, LoadInfo info) -> void {
  ZoneScoped;

  auto write_lock = std::unique_lock(self.pending_load_info_mutex);
  self.pending_load_info.insert_or_assign(uuid, std::move(info));
}

auto AssetManager::get_pending_load_info(this AssetManager& self, const UUID& uuid) -> LoadInfo {
  ZoneScoped;

  // a copy, not a guard: the value goes straight into a load that must not run holding this lock
  auto read_lock = std::shared_lock(self.pending_load_info_mutex);
  const auto it = self.pending_load_info.find(uuid);
  if (it == self.pending_load_info.end()) {
    return {};
  }

  return it->second;
}

auto AssetManager::clear_pending_load_info(this AssetManager& self, const UUID& uuid) -> void {
  ZoneScoped;

  auto write_lock = std::unique_lock(self.pending_load_info_mutex);
  self.pending_load_info.erase(uuid);
}

auto AssetManager::acquire_ref(this AssetManager& self, ReadGuard<Asset> asset) -> void {
  ZoneScoped;

  if (!asset || !asset->is_loaded()) {
    return;
  }

  // acquire self first
  asset->acquire_ref();

  auto children = ankerl::svector<UUID, 8>{};
  switch (asset->type) {
    case AssetType::None          :
    case AssetType::Shader        :
    case AssetType::Font          :
    case AssetType::Scene         :
    case AssetType::Audio         :
    case AssetType::Texture       :
    case AssetType::Terrain       :
    case AssetType::Script        : break;
    case AssetType::ParticleSystem: {
      auto particle_system = self.get_particle_system(asset->particle_system_id);
      if (particle_system) {
        children = {particle_system->render.material, particle_system->render.mesh};
      }
    } break;
    case AssetType::Model: {
      auto model = self.get_model(asset->model_id);
      if (model) {
        children.assign(model->materials.begin(), model->materials.end());
      }
    } break;
    case AssetType::Material: {
      auto material = self.get_material(asset->material_id);
      if (material) {
        children = {
          material->albedo_texture,
          material->normal_texture,
          material->emissive_texture,
          material->metallic_roughness_texture,
          material->occlusion_texture,
        };
      }
    } break;
  }

  asset.reset();

  for (auto& child : children) {
    self.acquire_ref(self.get_asset(child));
  }
}

auto AssetManager::release_ref(this AssetManager& self, ReadGuard<Asset> asset) -> void {
  ZoneScoped;

  if (!asset || !asset->is_loaded()) {
    return;
  }

  const auto uuid = asset->uuid;
  const auto type = asset->type;

  // release children first
  auto children = ankerl::svector<UUID, 8>{};
  switch (type) {
    case AssetType::None          :
    case AssetType::Shader        :
    case AssetType::Font          :
    case AssetType::Scene         :
    case AssetType::Audio         :
    case AssetType::Texture       :
    case AssetType::Terrain       :
    case AssetType::Script        : break;
    case AssetType::ParticleSystem: {
      auto particle_system = self.get_particle_system(asset->particle_system_id);
      if (particle_system) {
        children = {particle_system->render.material, particle_system->render.mesh};
      }
    } break;
    case AssetType::Model: {
      auto model = self.get_model(asset->model_id);
      if (model) {
        children.assign(model->materials.begin(), model->materials.end());
      }
    } break;
    case AssetType::Material: {
      auto material = self.get_material(asset->material_id);
      if (material) {
        children = {
          material->albedo_texture,
          material->normal_texture,
          material->emissive_texture,
          material->metallic_roughness_texture,
          material->occlusion_texture,
        };
      }
    } break;
  }

  // then release self
  auto should_unload = false;
  if (asset->ref_count > 0) {
    should_unload = asset->release_ref();
  }

  asset.reset();

  for (auto& child : children) {
    self.release_ref(self.get_asset(child));
  }

  if (should_unload) {
    auto removed_type = AssetType::None;
    auto removed_id = std::to_underlying(ModelID::Invalid);
    {
      auto write_lock = std::unique_lock(self.registry_mutex);
      auto it = self.asset_registry.find(uuid);
      if (it == self.asset_registry.end()) {
        return;
      }

      removed_type = it->second.type;
      removed_id = std::to_underlying(it->second.model_id);
      self.asset_registry.erase(it);
    }

    self.unload_asset_impl(removed_type, removed_id);
  }
}

auto AssetManager::unload_asset_impl(this AssetManager& self, const AssetType type, const u64 id) -> bool {
  ZoneScoped;

  if (id == std::to_underlying(ModelID::Invalid)) {
    return false;
  }

  switch (type) {
    case AssetType::Model         : return self.unload_model(static_cast<ModelID>(id));
    case AssetType::Texture       : return self.unload_texture(static_cast<TextureID>(id));
    case AssetType::Material      : return self.unload_material(static_cast<MaterialID>(id));
    case AssetType::Scene         : return self.unload_scene(static_cast<SceneID>(id));
    case AssetType::Audio         : return self.unload_audio(static_cast<AudioID>(id));
    case AssetType::Script        : return self.unload_script(static_cast<ScriptID>(id));
    case AssetType::Terrain       : return self.unload_terrain_edits(static_cast<TerrainEditsID>(id));
    case AssetType::ParticleSystem: return self.unload_particle_system(static_cast<ParticleSystemID>(id));
    case AssetType::None          :
    case AssetType::Shader        :
    case AssetType::Font          : return false;
  }

  return false;
}

auto AssetManager::load_asset(this AssetManager& self, const UUID& uuid, LoadInfo explicit_load, bool should_acquire)
  -> bool {
  return self.load_asset_impl(uuid, std::move(explicit_load), should_acquire, false);
}

auto AssetManager::load_asset_async(this AssetManager& self, const UUID& uuid, LoadInfo explicit_load) -> bool {
  ZoneScoped;

  if (!self.get_asset(uuid)) {
    OX_LOG_ERROR("Trying to asynchronously load an asset that isn't registered.");
    return false;
  }

  {
    auto lock = std::unique_lock(self.loading_mutex);
    if (!self.loading_assets.emplace(uuid).second) {
      return true;
    }
  }

  auto& job_man = App::get_job_manager();
  job_man.push_job_name("AssetManager_LoadAssetAsync");
  job_man.submit(Job::create([&self, uuid, load_info = std::move(explicit_load)]() {
    OX_DEFER(&) {
      auto lock = std::unique_lock(self.loading_mutex);
      self.loading_assets.erase(uuid);
    };

    std::ignore = self.load_asset_impl(uuid, load_info, false, true);
  }));
  job_man.pop_job_name();

  return true;
}

auto AssetManager::is_loading(this AssetManager& self, const UUID& uuid) -> bool {
  auto lock = std::shared_lock(self.loading_mutex);
  return self.loading_assets.contains(uuid);
}

auto AssetManager::load_asset_impl(
  this AssetManager& self, const UUID& uuid, LoadInfo explicit_load, bool should_acquire, bool async
) -> bool {
  ZoneScoped;

  auto asset = self.get_asset(uuid);
  if (!asset) {
    return false;
  }

  if (std::holds_alternative<std::monostate>(explicit_load)) {
    explicit_load = self.get_pending_load_info(uuid);
  }

  if (asset->is_loaded()) {
    const auto loaded_model_id = asset->type == AssetType::Model ? asset->model_id : ModelID::Invalid;
    const auto* texture_info = asset->type == AssetType::Texture ? std::get_if<TextureLoadInfo>(&explicit_load)
                                                                 : nullptr;
    const auto loaded_texture_id = texture_info ? asset->texture_id : TextureID::Invalid;
    const auto loaded_path = texture_info ? asset->path : std::filesystem::path{};

    if (should_acquire) {
      self.acquire_ref(std::move(asset));
    }
    asset.reset();

    if (texture_info) {
      auto texture = self.get_texture(loaded_texture_id);
      if (texture && texture->is_srgb() != texture_info->is_srgb) {
        // A compiled texture's colour space is settled at import, so no slot ever wins the race --
        // the pack does, and the fix is to re-import rather than to split the asset.
        const auto is_compiled = loaded_path.extension() == ".oxpack";
        OX_LOG_WARN(
          "Texture '{}' is {} as {} but is being used as {}. {}",
          loaded_path.string(),
          is_compiled ? "compiled" : "already loaded",
          texture->is_srgb() ? "sRGB" : "linear",
          texture_info->is_srgb ? "sRGB" : "linear",
          is_compiled ? "Re-import the source with the other color space."
                      : "Whichever slot loaded it first won; use a separate asset per color space."
        );
      }
    }

    if (!async && loaded_model_id != ModelID::Invalid) {
      self.wait_until_model_loaded(loaded_model_id);
    }

    return true;
  }

  auto asset_type = asset->type;
  auto asset_path = asset->path;

  asset.reset();

  auto asset_id = [&]() -> u64 {
    switch (asset_type) {
      case AssetType::Model: {
        if (auto* model_data = std::get_if<ModelData>(&explicit_load)) {
          return static_cast<u64>(self.load_model(std::move(*model_data), async));
        }

        return static_cast<u64>(self.load_model(asset_path, async));
      }
      case AssetType::Texture: {
        if (auto* texture_data = std::get_if<TextureData>(&explicit_load)) {
          return static_cast<u64>(self.load_texture(*texture_data, TextureLoadInfo{}));
        }

        const auto* info = std::get_if<TextureLoadInfo>(&explicit_load);
        return static_cast<u64>(self.load_texture(asset_path, info ? *info : TextureLoadInfo{}));
      }
      case AssetType::Scene         : return static_cast<u64>(self.load_scene(asset_path));
      case AssetType::Audio         : return static_cast<u64>(self.load_audio(asset_path));
      case AssetType::Script        : return static_cast<u64>(self.load_script(asset_path));
      case AssetType::Terrain       : return static_cast<u64>(self.load_terrain_edits(asset_path));
      case AssetType::ParticleSystem: return static_cast<u64>(self.load_particle_system(asset_path));
      case AssetType::Material      : {
        const auto* info = std::get_if<Material>(&explicit_load);
        return static_cast<u64>(self.load_material(asset_path, info ? *info : Material{}));
      }
      default:;
    }

    return ~0_u64;
  }();

  if (asset_id == ~0_u64) {
    return false;
  }

  auto published = false;
  {
    auto write_lock = std::unique_lock(self.registry_mutex);
    const auto it = self.asset_registry.find(uuid);
    if (it != self.asset_registry.end() && !it->second.is_loaded()) {
      it->second.model_id = static_cast<ModelID>(asset_id);
      published = true;
    }
  }

  // The registry entry was unregistered, or another thread loaded it first, while this payload was
  // being built. Nothing references it, so drop it again.
  if (!published) {
    self.unload_asset_impl(asset_type, asset_id);

    return true;
  }

  if (should_acquire) {
    self.acquire_ref(self.get_asset(uuid));
  }

  return true;
}

auto AssetManager::unload_asset(this AssetManager& self, const UUID& uuid) -> void {
  ZoneScoped;

  self.release_ref(self.get_asset(uuid));
}

auto AssetManager::load_texture(this AssetManager& self, const std::filesystem::path& path, TextureLoadInfo info)
  -> TextureID {
  ZoneScoped;

  auto data_source = TextureDataSource{};
  auto source_bytes = std::get_if<std::span<const u8>>(&info.source);
  auto source_path = std::get_if<std::filesystem::path>(&info.source);
  if (source_bytes || (source_path && !source_path->empty())) {
    data_source = info.source;
  } else {
    data_source = path;
  }

  // A cooked texture lives in a pack of its own; only PNG/JPEG still reach the engine encoded.
  if (
    const auto* file_path = std::get_if<std::filesystem::path>(&data_source);
    file_path && file_path->extension() == ".oxpack"
  ) {
    auto pack = AssetFile::unpack(*file_path);
    if (!pack) {
      return TextureID::Invalid;
    }

    for (auto& entry : pack->entries) {
      if (auto* texture_data = std::get_if<TextureData>(&entry.data)) {
        return self.load_texture(*texture_data, info);
      }
    }

    OX_LOG_ERROR("Asset pack '{}' contains no texture.", *file_path);
    return TextureID::Invalid;
  }

  auto texture = Texture::create({
    .source = data_source,
    .level_count = info.level_count,
    .is_srgb = info.is_srgb,
    .target_width = info.target_width,
    .target_height = info.target_height,
    .sampler_info = info.sampler_info,
    .batch = info.batch,
  });
  if (!texture) {
    return TextureID::Invalid;
  }

  auto write_lock = std::unique_lock(self.textures_mutex);
  return self.texture_map.create_slot(std::move(texture));
}

auto AssetManager::load_texture(this AssetManager& self, const TextureData& data, const TextureLoadInfo& info)
  -> TextureID {
  ZoneScoped;

  auto texture = Texture::create(data, info);
  if (!texture) {
    return TextureID::Invalid;
  }

  auto write_lock = std::unique_lock(self.textures_mutex);
  return self.texture_map.create_slot(std::move(texture));
}

auto AssetManager::unload_texture(this AssetManager& self, const TextureID texture_id) -> bool {
  ZoneScoped;

  auto write_lock = std::unique_lock(self.textures_mutex);
  auto* texture = self.texture_map.slot(texture_id);
  if (!texture) {
    return false;
  }

  texture->destroy();
  self.texture_map.destroy_slot(texture_id);

  return true;
}

auto AssetManager::load_material(this AssetManager& self, const std::filesystem::path& path, const Material& info)
  -> MaterialID {
  ZoneScoped;

  const auto load_texture_slot = [&self](const UUID& texture_uuid, bool is_srgb) -> void {
    if (texture_uuid) {
      self.load_asset(texture_uuid, TextureLoadInfo{.is_srgb = is_srgb}, false);
    }
  };

  load_texture_slot(info.albedo_texture, true);
  load_texture_slot(info.normal_texture, false);
  load_texture_slot(info.emissive_texture, true);
  load_texture_slot(info.metallic_roughness_texture, false);
  load_texture_slot(info.occlusion_texture, false);

  auto write_lock = std::unique_lock(self.materials_mutex);
  auto material_id = self.material_map.create_slot(Material(info));

  write_lock.unlock();

  self.set_material_dirty(material_id);

  return material_id;
}

auto AssetManager::unload_material(this AssetManager& self, const MaterialID material_id) -> bool {
  ZoneScoped;

  auto write_lock = std::unique_lock(self.materials_mutex);
  self.material_map.destroy_slot(material_id);
  std::erase(self.dirty_materials, material_id);

  return true;
}

auto AssetManager::load_scene(this AssetManager& self, const std::filesystem::path& path) -> SceneID {
  ZoneScoped;

  auto scene = std::make_unique<Scene>();
  scene->init("unnamed_scene");

  if (!scene->load_from_file(path)) {
    return SceneID::Invalid;
  }

  auto write_lock = std::unique_lock(self.scenes_mutex);
  return self.scene_map.create_slot(std::move(scene));
}

auto AssetManager::unload_scene(this AssetManager& self, const SceneID scene_id) -> bool {
  ZoneScoped;

  auto write_lock = std::unique_lock(self.scenes_mutex);
  self.scene_map.destroy_slot(scene_id);

  return true;
}

auto AssetManager::load_audio(this AssetManager& self, const std::filesystem::path& path) -> AudioID {
  ZoneScoped;

  auto audio = AudioSource{};
  audio.load(path);

  auto write_lock = std::unique_lock(self.audio_mutex);
  return self.audio_map.create_slot(std::move(audio));
}

auto AssetManager::unload_audio(this AssetManager& self, const AudioID audio_id) -> bool {
  ZoneScoped;

  auto write_lock = std::unique_lock(self.audio_mutex);
  auto* audio = self.audio_map.slot(audio_id);
  if (audio) {
    audio->unload();
  }

  self.audio_map.destroy_slot(audio_id);

  return true;
}

auto AssetManager::load_script(this AssetManager& self, const std::filesystem::path& path) -> ScriptID {
  ZoneScoped;

  auto script = std::make_unique<LuaScript>();
  script->path = path;

  auto write_lock = std::unique_lock(self.scripts_mutex);
  return self.script_map.create_slot(std::move(script));
}

auto AssetManager::unload_script(this AssetManager& self, const ScriptID script_id) -> bool {
  ZoneScoped;

  auto write_lock = std::unique_lock(self.scripts_mutex);
  self.script_map.destroy_slot(script_id);

  return true;
}

auto AssetManager::load_particle_system(this AssetManager& self, const std::filesystem::path& path)
  -> ParticleSystemID {
  ZoneScoped;

  auto system = ParticleSystem::read(path);
  auto payload = system ? std::move(*system) : ParticleSystem::make_default();

  if (payload.render.material) {
    self.load_asset(payload.render.material, {}, false);
  }
  if (payload.render.mesh) {
    self.load_asset(payload.render.mesh, {}, false);
  }

  auto write_lock = std::unique_lock(self.particle_systems_mutex);
  return self.particle_system_map.create_slot(std::move(payload));
}

auto AssetManager::unload_particle_system(this AssetManager& self, const ParticleSystemID particle_system_id) -> bool {
  ZoneScoped;

  auto write_lock = std::unique_lock(self.particle_systems_mutex);
  auto* system = self.particle_system_map.slot(particle_system_id);
  if (!system) {
    return false;
  }

  system->destroy();
  self.particle_system_map.destroy_slot(particle_system_id);

  return true;
}

auto AssetManager::load_terrain_edits(this AssetManager& self, const std::filesystem::path& path) -> TerrainEditsID {
  ZoneScoped;

  auto edits = TerrainEdits::read(path).value_or(TerrainEdits{});

  auto write_lock = std::unique_lock(self.terrain_edits_mutex);
  return self.terrain_edits_map.create_slot(std::move(edits));
}

auto AssetManager::unload_terrain_edits(this AssetManager& self, const TerrainEditsID terrain_edits_id) -> bool {
  ZoneScoped;

  auto write_lock = std::unique_lock(self.terrain_edits_mutex);
  self.terrain_edits_map.destroy_slot(terrain_edits_id);

  return true;
}

auto AssetManager::is_loaded(this AssetManager& self, const UUID& uuid) -> bool {
  ZoneScoped;

  auto asset = self.get_asset(uuid);

  return asset && asset->is_loaded();
}

auto AssetManager::get_asset(this AssetManager& self, const UUID& uuid) -> ReadGuard<Asset> {
  ZoneScoped;

  self.registry_mutex.lock_shared();
  const auto it = self.asset_registry.find(uuid);
  if (it == self.asset_registry.end()) {
    self.registry_mutex.unlock_shared();
    return {};
  }
  return ReadGuard<Asset>(self.registry_mutex, &it->second, adopt_lock);
}

auto AssetManager::get_model(this AssetManager& self, const UUID& uuid) -> ReadGuard<Model> {
  ZoneScoped;

  ModelID model_id;
  {
    auto guard = self.get_asset(uuid);
    if (!guard || guard->type != AssetType::Model || guard->model_id == ModelID::Invalid)
      return {};
    model_id = guard->model_id;
  }
  return self.get_model(model_id);
}

auto AssetManager::get_model(this AssetManager& self, const ModelID model_id) -> ReadGuard<Model> {
  ZoneScoped;

  if (model_id == ModelID::Invalid)
    return {};
  self.models_mutex.lock_shared();
  auto* model = self.model_map.slot(model_id);
  if (!model) {
    self.models_mutex.unlock_shared();
    return {};
  }
  return ReadGuard<Model>(self.models_mutex, model, adopt_lock);
}

auto AssetManager::wait_until_model_loaded(this AssetManager& self, const ModelID model_id) -> void {
  ZoneScoped;

  auto lock = std::unique_lock(self.model_load_mutex);
  self.model_load_cv.wait(lock, [&self, model_id] {
    auto model = self.get_model(model_id);
    return !model || model->is_fully_loaded();
  });
}

auto AssetManager::notify_model_loaded(this AssetManager& self) -> void {
  ZoneScoped;

  // `pending_meshes` is mutated outside `model_load_mutex`, so this lock is what closes the window
  // between a waiter evaluating the predicate and sleeping on it.
  auto lock = std::unique_lock(self.model_load_mutex);
  lock.unlock();
  self.model_load_cv.notify_all();
}

auto AssetManager::get_texture(this AssetManager& self, const UUID& uuid) -> ReadGuard<Texture> {
  ZoneScoped;

  TextureID texture_id;
  {
    auto guard = self.get_asset(uuid);
    if (!guard || guard->type != AssetType::Texture || guard->texture_id == TextureID::Invalid)
      return {};
    texture_id = guard->texture_id;
  }
  return self.get_texture(texture_id);
}

auto AssetManager::get_texture(this AssetManager& self, const TextureID texture_id) -> ReadGuard<Texture> {
  ZoneScoped;

  if (texture_id == TextureID::Invalid)
    return {};
  self.textures_mutex.lock_shared();
  auto* texture = self.texture_map.slot(texture_id);
  if (!texture) {
    self.textures_mutex.unlock_shared();
    return {};
  }
  return ReadGuard<Texture>(self.textures_mutex, texture, adopt_lock);
}

auto AssetManager::get_null_material(this AssetManager& self) -> ReadGuard<Asset> {
  return self.get_asset(self.null_material);
}

auto AssetManager::get_material(this AssetManager& self, const UUID& uuid) -> ReadGuard<Material> {
  ZoneScoped;

  MaterialID material_id;
  {
    auto guard = self.get_asset(uuid);
    if (!guard || guard->type != AssetType::Material || guard->material_id == MaterialID::Invalid)
      return {};
    material_id = guard->material_id;
  }
  return self.get_material(material_id);
}

auto AssetManager::get_material(this AssetManager& self, const MaterialID material_id) -> ReadGuard<Material> {
  ZoneScoped;

  if (material_id == MaterialID::Invalid)
    return {};
  self.materials_mutex.lock_shared();
  auto* material = self.material_map.slot(material_id);
  if (!material) {
    self.materials_mutex.unlock_shared();
    return {};
  }
  return ReadGuard<Material>(self.materials_mutex, material, adopt_lock);
}

auto AssetManager::set_material_dirty(this AssetManager& self, MaterialID material_id) -> void {
  ZoneScoped;

  auto lock = std::unique_lock(self.materials_mutex);
  if (std::ranges::find(self.dirty_materials, material_id) != self.dirty_materials.end()) {
    return;
  }

  self.dirty_materials.emplace_back(material_id);
}

auto AssetManager::set_material_dirty(this AssetManager& self, const UUID& uuid) -> void {
  ZoneScoped;

  auto asset = self.get_asset(uuid);
  if (!asset || !asset->is_loaded() || asset->type != AssetType::Material) {
    return;
  }

  self.set_material_dirty(asset->material_id);
}

auto AssetManager::set_all_materials_dirty(this AssetManager& self) -> void {
  ZoneScoped;

  auto material_ids = std::vector<MaterialID>();
  {
    auto read_lock = std::shared_lock(self.registry_mutex);
    for (const auto& [uuid, asset] : self.asset_registry) {
      if (asset.type == AssetType::Material && asset.is_loaded()) {
        material_ids.emplace_back(asset.material_id);
      }
    }
  }

  for (const auto material_id : material_ids) {
    self.set_material_dirty(material_id);
  }
}

auto AssetManager::get_dirty_material_ids(this AssetManager& self) -> std::vector<MaterialID> {
  ZoneScoped;

  auto write_lock = std::unique_lock(self.materials_mutex);
  auto dirty_copy = std::vector(self.dirty_materials);
  self.dirty_materials.clear();

  return dirty_copy;
}

auto AssetManager::get_scene(this AssetManager& self, const UUID& uuid) -> ReadGuard<Scene> {
  ZoneScoped;

  SceneID scene_id;
  {
    auto guard = self.get_asset(uuid);
    if (!guard || guard->type != AssetType::Scene || guard->scene_id == SceneID::Invalid)
      return {};
    scene_id = guard->scene_id;
  }
  return self.get_scene(scene_id);
}

auto AssetManager::get_scene(this AssetManager& self, const SceneID scene_id) -> ReadGuard<Scene> {
  ZoneScoped;

  if (scene_id == SceneID::Invalid)
    return {};
  self.scenes_mutex.lock_shared();
  auto* scene = self.scene_map.slot(scene_id);
  if (!scene) {
    self.scenes_mutex.unlock_shared();
    return {};
  }
  return ReadGuard<Scene>(self.scenes_mutex, scene->get(), adopt_lock);
}

auto AssetManager::get_audio(this AssetManager& self, const UUID& uuid) -> ReadGuard<AudioSource> {
  ZoneScoped;

  AudioID audio_id;
  {
    auto guard = self.get_asset(uuid);
    if (!guard || guard->type != AssetType::Audio || guard->audio_id == AudioID::Invalid)
      return {};
    audio_id = guard->audio_id;
  }
  return self.get_audio(audio_id);
}

auto AssetManager::get_audio(this AssetManager& self, const AudioID audio_id) -> ReadGuard<AudioSource> {
  ZoneScoped;

  if (audio_id == AudioID::Invalid)
    return {};
  self.audio_mutex.lock_shared();
  auto* audio = self.audio_map.slot(audio_id);
  if (!audio) {
    self.audio_mutex.unlock_shared();
    return {};
  }
  return ReadGuard<AudioSource>(self.audio_mutex, audio, adopt_lock);
}

auto AssetManager::get_script(this AssetManager& self, const UUID& uuid) -> ReadGuard<LuaScript> {
  ZoneScoped;

  ScriptID script_id;
  {
    auto guard = self.get_asset(uuid);
    if (!guard || guard->type != AssetType::Script || guard->script_id == ScriptID::Invalid)
      return {};
    script_id = guard->script_id;
  }
  return self.get_script(script_id);
}

auto AssetManager::get_script(this AssetManager& self, ScriptID script_id) -> ReadGuard<LuaScript> {
  ZoneScoped;

  if (script_id == ScriptID::Invalid)
    return {};
  self.scripts_mutex.lock_shared();
  auto* script = self.script_map.slot(script_id);
  if (!script) {
    self.scripts_mutex.unlock_shared();
    return {};
  }
  return ReadGuard<LuaScript>(self.scripts_mutex, script->get(), adopt_lock);
}

auto AssetManager::get_terrain_edits(this AssetManager& self, const UUID& uuid) -> ReadGuard<TerrainEdits> {
  ZoneScoped;

  TerrainEditsID terrain_edits_id;
  {
    auto guard = self.get_asset(uuid);
    if (!guard || guard->type != AssetType::Terrain || guard->terrain_edits_id == TerrainEditsID::Invalid)
      return {};
    terrain_edits_id = guard->terrain_edits_id;
  }
  return self.get_terrain_edits(terrain_edits_id);
}

auto AssetManager::get_terrain_edits(this AssetManager& self, const TerrainEditsID terrain_edits_id)
  -> ReadGuard<TerrainEdits> {
  ZoneScoped;

  if (terrain_edits_id == TerrainEditsID::Invalid)
    return {};
  self.terrain_edits_mutex.lock_shared();
  auto* edits = self.terrain_edits_map.slot(terrain_edits_id);
  if (!edits) {
    self.terrain_edits_mutex.unlock_shared();
    return {};
  }
  return ReadGuard<TerrainEdits>(self.terrain_edits_mutex, edits, adopt_lock);
}

auto AssetManager::set_terrain_edits(this AssetManager& self, const UUID& uuid, TerrainEdits&& edits) -> void {
  ZoneScoped;

  TerrainEditsID terrain_edits_id;
  {
    auto guard = self.get_asset(uuid);
    if (!guard || guard->type != AssetType::Terrain || guard->terrain_edits_id == TerrainEditsID::Invalid)
      return;
    terrain_edits_id = guard->terrain_edits_id;
  }

  auto write_lock = std::unique_lock(self.terrain_edits_mutex);
  if (auto* slot = self.terrain_edits_map.slot(terrain_edits_id)) {
    *slot = std::move(edits);
  }
}

auto AssetManager::get_particle_system(this AssetManager& self, const UUID& uuid) -> ReadGuard<ParticleSystem> {
  ZoneScoped;

  ParticleSystemID particle_system_id;
  {
    auto guard = self.get_asset(uuid);
    if (!guard || guard->type != AssetType::ParticleSystem || guard->particle_system_id == ParticleSystemID::Invalid)
      return {};
    particle_system_id = guard->particle_system_id;
  }
  return self.get_particle_system(particle_system_id);
}

auto AssetManager::get_particle_system(this AssetManager& self, const ParticleSystemID particle_system_id)
  -> ReadGuard<ParticleSystem> {
  ZoneScoped;

  if (particle_system_id == ParticleSystemID::Invalid)
    return {};
  self.particle_systems_mutex.lock_shared();
  auto* system = self.particle_system_map.slot(particle_system_id);
  if (!system) {
    self.particle_systems_mutex.unlock_shared();
    return {};
  }
  return ReadGuard<ParticleSystem>(self.particle_systems_mutex, system, adopt_lock);
}

auto AssetManager::edit_particle_system(
  this AssetManager& self, const UUID& uuid, const std::function<void(ParticleSystem&)>& mutate, const bool recompile
) -> void {
  ZoneScoped;

  ParticleSystemID particle_system_id;
  auto holder_count = 0_u64;
  {
    auto guard = self.get_asset(uuid);
    if (!guard || guard->type != AssetType::ParticleSystem || guard->particle_system_id == ParticleSystemID::Invalid)
      return;
    particle_system_id = guard->particle_system_id;
    holder_count = std::atomic_ref(guard->ref_count).load();
  }

  auto previous_children = std::array<UUID, 2>{};
  auto current_children = std::array<UUID, 2>{};
  {
    auto write_lock = std::unique_lock(self.particle_systems_mutex);
    auto* slot = self.particle_system_map.slot(particle_system_id);
    if (!slot) {
      return;
    }

    previous_children = {slot->render.material, slot->render.mesh};
    mutate(*slot);
    if (recompile) {
      slot->recompile();
    }
    current_children = {slot->render.material, slot->render.mesh};
  }

  // acquire_ref walks into the sub-assets once per holder of this system, so swapping one has to
  // hand that many refs over, not a single one
  for (usize i = 0; i < previous_children.size(); i++) {
    if (previous_children[i] == current_children[i]) {
      continue;
    }

    if (current_children[i]) {
      self.load_asset(current_children[i], {}, false);
    }

    for (auto ref = 0_u64; ref < holder_count; ref++) {
      self.acquire_ref(self.get_asset(current_children[i]));
      self.release_ref(self.get_asset(previous_children[i]));
    }
  }
}

auto AssetManager::set_particle_system_dirty(this AssetManager& self, const UUID& uuid) -> void {
  ZoneScoped;

  ParticleSystemID particle_system_id;
  {
    auto guard = self.get_asset(uuid);
    if (!guard || guard->type != AssetType::ParticleSystem || guard->particle_system_id == ParticleSystemID::Invalid)
      return;
    particle_system_id = guard->particle_system_id;
  }

  auto write_lock = std::unique_lock(self.particle_systems_mutex);
  if (auto* slot = self.particle_system_map.slot(particle_system_id)) {
    slot->recompile();
  }
}
} // namespace ox
