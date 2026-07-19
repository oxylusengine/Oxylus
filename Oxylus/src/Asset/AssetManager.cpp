#include "Asset/AssetManager.hpp"

#include <vuk/Types.hpp>
#include <vuk/vsl/Core.hpp>
#include <zpp_bits.h>

#include "Memory/Stack.hpp"
#include "OS/File.hpp"
#include "Scripting/LuaSystem.hpp"
#include "Utils/Log.hpp"

namespace ox {
auto AssetManager::init(this AssetManager& self) -> std::expected<void, std::string> {
  ZoneScoped;

  self.null_material = self.create_asset(AssetType::Material);
  // self.load_material(self.get_null_material());

  return {};
}

auto AssetManager::deinit(this AssetManager& self) -> std::expected<void, std::string> {
  ZoneScoped;

  for (auto& [uuid, asset] : self.asset_registry) {
    if (asset.is_loaded() && asset.ref_count != 0) {
      OX_LOG_WARN("An asset ({}, {}) with refcount of {} is still alive!", uuid.str(), asset.path, asset.ref_count);
    }
  }

  self.asset_registry.clear();
  self.dirty_materials.clear();
  self.model_map.reset();
  self.texture_map.reset();
  self.material_map.reset();
  self.scene_map.reset();
  self.audio_map.reset();
  self.script_map.reset();

  return {};
}

auto AssetManager::registry(this const AssetManager& self) -> const AssetRegistry& {
  ZoneScoped;

  return self.asset_registry;
}

auto AssetManager::create_asset(this AssetManager& self, const AssetType type, const std::filesystem::path& path)
  -> UUID {
  const auto uuid = UUID::generate_random();
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

auto AssetManager::import_asset(this AssetManager& self, const std::filesystem::path& path) -> UUID {
  ZoneScoped;
  memory::ScopedStack stack;

  if (!std::filesystem::exists(path)) {
    OX_LOG_ERROR("Trying to import an asset '{}' that doesn't exist.", path);
    return UUID(nullptr);
  }

  auto file = File(path, FileAccess::Read);
  auto header = AssetFileHeader{};
  auto read_bytes = file.read(&header, sizeof(AssetFileHeader));
  if (read_bytes < sizeof(AssetFileHeader)) {
    OX_LOG_ERROR("Failed to read file {}. Not big enough to be an asset.", path);
    return UUID(nullptr);
  }

  if (header.magic != AssetFileHeader::SIGNATURE) {
    OX_LOG_ERROR("Failed to read file {}. Not a valid asset file.", path);
    return UUID(nullptr);
  }

  auto uuid = self.create_asset(header, path);
  if (!uuid) {
    return UUID(nullptr);
  }

  return uuid;
}

auto AssetManager::delete_asset(this AssetManager& self, const UUID& uuid) -> void {
  ZoneScoped;

  bool is_loaded = false;
  u64 ref_count = 0;
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

  OX_LOG_INFO("Registered new asset: {}", uuid.str());

  return true;
}

auto AssetManager::acquire_ref(this AssetManager& self, const UUID& uuid) -> void {
  ZoneScoped;

  auto asset = self.get_asset(uuid);
  if (asset && asset->is_loaded()) {
    asset->acquire_ref();
  }
}

auto AssetManager::unload(this AssetManager& self, const UUID& uuid) -> void {
  ZoneScoped;

  self.unload_asset(uuid);
}

auto AssetManager::load_asset(this AssetManager& self, const UUID& uuid) -> bool {
  ZoneScoped;

  auto asset = self.get_asset(uuid);
  if (!asset)
    return false;

  auto asset_type = asset->type;
  auto asset_path = asset->path;

  asset.reset();

  auto asset_id = [&]() -> u64 {
    switch (asset_type) {
      case AssetType::Model   : return static_cast<u64>(self.load_model(asset_path));
      case AssetType::Texture : return static_cast<u64>(self.load_texture(asset_path));
      case AssetType::Scene   : return static_cast<u64>(self.load_scene(asset_path));
      case AssetType::Audio   : return static_cast<u64>(self.load_audio(asset_path));
      case AssetType::Script  : return static_cast<u64>(self.load_script(asset_path));
      case AssetType::Material: return static_cast<u64>(self.load_material(asset_path));
      default                 :;
    }
  }();

  if (asset_id == ~0_u64) {
    return false;
  }

  asset = self.get_asset(uuid);
  if (!asset) {
    return false;
  }

  if (asset->is_loaded()) {
    // TODO: unload `asset_id`.
    return true;
  }

  asset->model_id = static_cast<ModelID>(asset_id);

  return true;
}

auto AssetManager::unload_asset(this AssetManager& self, const UUID& uuid) -> bool {
  ZoneScoped;

  auto asset = self.get_asset(uuid);
  if (!asset)
    return false;

  switch (asset->type) {
    case AssetType::Model   : return self.unload_model(std::move(asset));
    case AssetType::Texture : return self.unload_texture(std::move(asset));
    case AssetType::Scene   : return self.unload_scene(std::move(asset));
    case AssetType::Audio   : return self.unload_audio(std::move(asset));
    case AssetType::Script  : return self.unload_script(std::move(asset));
    case AssetType::Material: return self.unload_material(std::move(asset));
    default                 :;
  }

  return false;
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

  auto read_lock = std::shared_lock(self.materials_mutex);
  if (std::ranges::find(self.dirty_materials, material_id) != self.dirty_materials.end()) {
    return;
  }

  read_lock.unlock();

  auto write_lock = std::unique_lock(self.materials_mutex);
  self.dirty_materials.emplace_back(material_id);
}

auto AssetManager::set_material_dirty(this AssetManager& self, const UUID& uuid) -> void {
  ZoneScoped;

  auto asset = self.get_asset(uuid);
  if (!asset)
    return;
  auto id = asset->material_id;

  self.set_material_dirty(id);
}

auto AssetManager::set_all_materials_dirty(this AssetManager& self) -> void {
  ZoneScoped;

  auto read_lock = std::shared_lock(self.registry_mutex);
  for (auto& [uuid, asset] : self.asset_registry) {
    if (asset.type == AssetType::Material) {
      self.set_material_dirty(asset.material_id);
    }
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

auto AssetManager::get_script(this AssetManager& self, const UUID& uuid) -> ReadGuard<LuaSystem> {
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

auto AssetManager::get_script(this AssetManager& self, ScriptID script_id) -> ReadGuard<LuaSystem> {
  ZoneScoped;

  if (script_id == ScriptID::Invalid)
    return {};
  self.scripts_mutex.lock_shared();
  auto* script = self.script_map.slot(script_id);
  if (!script) {
    self.scripts_mutex.unlock_shared();
    return {};
  }
  return ReadGuard<LuaSystem>(self.scripts_mutex, script->get(), adopt_lock);
}

auto AssetManager::load_texture(this AssetManager& self, const UUID& uuid) -> bool {
  ZoneScoped;

  auto asset_path = std::filesystem::path{};
  {
    auto asset = self.get_asset(uuid);
    if (!asset)
      return false;

    asset->acquire_ref();

    if (asset->is_loaded())
      return true;

    asset_path = asset->path;
  }

  auto texture = Texture::create({
    .extent = {1, 1, 1},
    .usage = vuk::ImageUsageFlagBits::eSampled,
  });
  if (!texture)
    return false;
  // TODO load data

  auto asset = self.get_asset(uuid);
  if (!asset)
    return false;
  if (asset->is_loaded())
    return true;

  auto write_lock = std::unique_lock(self.textures_mutex);
  asset->texture_id = self.texture_map.create_slot(std::move(texture));

  OX_LOG_INFO("Loaded texture {} {}.", asset->uuid.str(), SlotMap_decode_id(asset->texture_id).index);

  return true;
}

auto AssetManager::unload_texture(this AssetManager& self, const UUID& uuid) -> bool {
  ZoneScoped;

  auto asset = self.get_asset(uuid);
  if (!asset || !(asset->is_loaded() && asset->release_ref()))
    return false;

  self.texture_map.destroy_slot(asset->texture_id);
  asset->texture_id = TextureID::Invalid;

  OX_LOG_INFO("Unloaded texture {}", uuid.str());

  return true;
}

auto AssetManager::is_texture_loaded(this AssetManager& self, const UUID& uuid) -> bool {
  ZoneScoped;

  auto asset = self.get_asset(uuid);

  return asset && asset->is_loaded();
}

auto AssetManager::load_material(this AssetManager& self, const UUID& uuid, const Material& info) -> bool {
  ZoneScoped;

  auto asset = self.get_asset(uuid);
  if (!asset)
    return false;

  asset->acquire_ref();

  if (asset->is_loaded()) {
    auto* material = self.material_map.slot(asset->material_id);
    self.acquire_ref(material->albedo_texture);
    self.acquire_ref(material->normal_texture);
    self.acquire_ref(material->emissive_texture);
    self.acquire_ref(material->metallic_roughness_texture);
    self.acquire_ref(material->occlusion_texture);
    return true;
  }

  asset->material_id = self.material_map.create_slot({
    .albedo_color = info.albedo_color,
    .emissive_color = info.emissive_color,
    .roughness_factor = info.roughness_factor,
    .metallic_factor = info.metallic_factor,
    .alpha_mode = info.alpha_mode,
    .alpha_cutoff = info.alpha_cutoff,
    .albedo_texture = info.albedo_texture,
    .normal_texture = info.normal_texture,
    .emissive_texture = info.emissive_texture,
    .metallic_roughness_texture = info.metallic_roughness_texture,
    .occlusion_texture = info.occlusion_texture,
  });

  self.set_material_dirty(asset->material_id);

  return true;
}

auto AssetManager::unload_material(this AssetManager& self, const UUID& uuid) -> bool {
  ZoneScoped;

  auto asset = self.get_asset(uuid);
  if (!asset || !(asset->is_loaded() && asset->release_ref()))
    return false;

  const auto* material = self.material_map.slot(asset->material_id);

  if (material->albedo_texture)
    self.unload_texture(material->albedo_texture);
  if (material->normal_texture)
    self.unload_texture(material->normal_texture);
  if (material->emissive_texture)
    self.unload_texture(material->emissive_texture);
  if (material->metallic_roughness_texture)
    self.unload_texture(material->metallic_roughness_texture);
  if (material->occlusion_texture)
    self.unload_texture(material->occlusion_texture);

  {
    auto write_lock = std::unique_lock(self.materials_mutex);
    self.material_map.destroy_slot(asset->material_id);
    asset->material_id = MaterialID::Invalid;
  }

  OX_LOG_INFO("Unloaded material {}", uuid.str());

  return true;
}

auto AssetManager::load_scene(this AssetManager& self, const UUID& uuid) -> bool {
  ZoneScoped;

  auto asset_path = std::filesystem::path{};
  {
    auto asset = self.get_asset(uuid);
    if (!asset)
      return false;

    asset->acquire_ref();

    if (asset->is_loaded())
      return true;

    asset_path = asset->path;
  }

  auto scene = std::make_unique<Scene>();
  scene->init("unnamed_scene");

  if (!scene->load_from_file(asset_path))
    return false;

  auto asset = self.get_asset(uuid);
  if (!asset)
    return false;
  if (asset->is_loaded())
    return true;

  auto write_lock = std::unique_lock(self.scenes_mutex);
  asset->scene_id = self.scene_map.create_slot(std::move(scene));

  return true;
}

auto AssetManager::unload_scene(this AssetManager& self, const UUID& uuid) -> bool {
  ZoneScoped;

  auto asset = self.get_asset(uuid);
  if (!asset || !(asset->is_loaded() && asset->release_ref()))
    return false;

  auto write_lock = std::unique_lock(self.scenes_mutex);
  self.scene_map.destroy_slot(asset->scene_id);
  asset->scene_id = SceneID::Invalid;

  OX_LOG_INFO("Unloaded scene {}", uuid.str());

  return true;
}

auto AssetManager::load_audio(this AssetManager& self, const UUID& uuid) -> bool {
  ZoneScoped;

  auto asset_path = std::filesystem::path{};
  {
    auto asset = self.get_asset(uuid);
    if (!asset)
      return false;

    asset->acquire_ref();

    if (asset->is_loaded())
      return true;

    asset_path = asset->path;
  }

  auto audio = AudioSource{};
  audio.load(asset_path);

  auto asset = self.get_asset(uuid);
  if (!asset)
    return false;
  if (asset->is_loaded())
    return true;

  auto write_lock = std::unique_lock(self.audio_mutex);
  asset->audio_id = self.audio_map.create_slot(std::move(audio));

  OX_LOG_INFO("Loaded audio {}.", uuid.str());

  return true;
}

auto AssetManager::unload_audio(this AssetManager& self, const UUID& uuid) -> bool {
  ZoneScoped;

  auto asset = self.get_asset(uuid);
  if (!asset || !(asset->is_loaded() && asset->release_ref()))
    return false;

  auto audio_read_lock = std::shared_lock(self.audio_mutex);
  auto* audio = self.audio_map.slot(asset->audio_id);
  if (audio)
    audio->unload();

  audio_read_lock.unlock();

  auto write_lock = std::unique_lock(self.audio_mutex);
  self.audio_map.destroy_slot(asset->audio_id);
  asset->audio_id = AudioID::Invalid;

  OX_LOG_INFO("Unloaded audio {}.", uuid.str());

  return true;
}

auto AssetManager::load_script(this AssetManager& self, const UUID& uuid) -> bool {
  ZoneScoped;

  auto asset_path = std::filesystem::path{};
  {
    auto asset = self.get_asset(uuid);
    if (!asset)
      return false;

    asset->acquire_ref();

    if (asset->is_loaded())
      return true;

    asset_path = asset->path;
  }

  auto lua_system = std::make_unique<LuaSystem>();
  lua_system->load(asset_path);

  auto asset = self.get_asset(uuid);
  if (!asset)
    return false;
  if (asset->is_loaded())
    return true;

  auto write_lock = std::unique_lock(self.scripts_mutex);
  asset->script_id = self.script_map.create_slot(std::move(lua_system));

  OX_LOG_INFO("Loaded script {} {}.", asset->uuid.str(), SlotMap_decode_id(asset->script_id).index);

  return true;
}

auto AssetManager::unload_script(this AssetManager& self, const UUID& uuid) -> bool {
  ZoneScoped;

  auto asset = self.get_asset(uuid);
  if (!asset || !(asset->is_loaded() && asset->release_ref()))
    return false;

  auto write_lock = std::unique_lock(self.scripts_mutex);
  self.script_map.destroy_slot(asset->script_id);
  asset->script_id = ScriptID::Invalid;

  OX_LOG_INFO("Unloaded script {}.", uuid.str());

  return true;
}

} // namespace ox
