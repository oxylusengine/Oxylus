#include "Asset/AssetManager.hpp"

#include <vuk/Types.hpp>
#include <vuk/vsl/Core.hpp>
#include <zpp_bits.h>

#include "Memory/Hasher.hpp"
#include "Memory/Stack.hpp"
#include "OS/File.hpp"
#include "Scripting/LuaSystem.hpp"
#include "Utils/Log.hpp"

namespace ox {
auto begin_asset_meta(JsonWriter& writer, const UUID& uuid, AssetType type) -> void {
  ZoneScoped;

  writer.begin_obj();
  writer["uuid"] = uuid.str();
  writer["type"] = std::to_underlying(type);
}

auto write_texture_asset_meta(JsonWriter& writer, Texture*) -> bool {
  ZoneScoped;

  return true;
}

auto write_material_asset_meta(JsonWriter& writer, const UUID& uuid, const Material& material) -> bool {
  ZoneScoped;

  writer.begin_obj();

  writer["uuid"] = uuid.str();
  writer["sampling_mode"] = static_cast<u32>(material.sampling_mode);
  writer["albedo_color"] = material.albedo_color;
  writer["emissive_color"] = material.emissive_color;
  writer["roughness_factor"] = material.roughness_factor;
  writer["metallic_factor"] = material.metallic_factor;
  writer["alpha_mode"] = std::to_underlying(material.alpha_mode);
  writer["alpha_cutoff"] = material.alpha_cutoff;
  writer["albedo_texture"] = material.albedo_texture.str().c_str();
  writer["normal_texture"] = material.normal_texture.str().c_str();
  writer["emissive_texture"] = material.emissive_texture.str().c_str();
  writer["metallic_roughness_texture"] = material.metallic_roughness_texture.str().c_str();
  writer["occlusion_texture"] = material.occlusion_texture.str().c_str();

  writer.end_obj();

  return true;
}

auto write_scene_asset_meta(JsonWriter& writer, const Scene* scene) -> bool {
  ZoneScoped;

  writer["name"] = scene->scene_name;

  return true;
}

auto write_script_asset_meta(JsonWriter&, LuaSystem*) -> bool {
  ZoneScoped;

  return true;
}

auto end_asset_meta(JsonWriter& writer, const std::filesystem::path& path) -> bool {
  ZoneScoped;

  writer.end_obj();

  auto meta_path = path;
  if (path.has_extension() && path.extension() != ".oxasset") {
    meta_path += ".oxasset";
  }

  auto file = File(meta_path, FileAccess::Write);
  file.write(writer.stream.view());
  file.close();
  return true;
}

auto AssetManager::init(this AssetManager& self) -> std::expected<void, std::string> {
  ZoneScoped;

  self.null_material = self.create_asset(AssetType::Material);
  self.load_material(self.null_material, {});

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

auto AssetManager::read_meta_file(this AssetManager& self, const std::filesystem::path& path)
  -> std::unique_ptr<AssetMetaFile> {
  auto content = File::to_string(path);
  if (content.empty()) {
    OX_LOG_ERROR("Failed to read/open file {}!", path);
    return nullptr;
  }

  auto meta_file = std::make_unique<AssetMetaFile>();
  meta_file->contents = simdjson::padded_string(content);
  meta_file->doc = meta_file->parser.iterate(meta_file->contents);

  if (meta_file->doc.error()) {
    OX_LOG_ERROR("Failed to parse meta file! {}", simdjson::error_message(meta_file->doc.error()));
    return nullptr;
  }

  return meta_file;
}

auto AssetManager::read_meta_file_from_asset(this AssetManager& self, const std::filesystem::path& path)
  -> std::unique_ptr<AssetMetaFile> {
  ZoneScoped;

  if (!std::filesystem::exists(path)) {
    return nullptr;
  }

  memory::ScopedStack stack;

  auto meta_path = stack.format("{}.oxasset", path);
  if (!std::filesystem::exists(meta_path)) {
    return nullptr;
  }

  return self.read_meta_file(meta_path);
}

auto AssetManager::to_asset_file_type(const std::filesystem::path& path) -> AssetFileType {
  ZoneScoped;
  memory::ScopedStack stack;

  if (!path.has_extension()) {
    return AssetFileType::None;
  }

  auto extension = stack.to_upper(path.extension().string());
  switch (fnv64_str(extension)) {
    case fnv64_c(".GLB")    : return AssetFileType::GLB;
    case fnv64_c(".GLTF")   : return AssetFileType::GLTF;
    case fnv64_c(".PNG")    : return AssetFileType::PNG;
    case fnv64_c(".JPG")    :
    case fnv64_c(".JPEG")   : return AssetFileType::JPEG;
    case fnv64_c(".DDS")    : return AssetFileType::DDS;
    case fnv64_c(".JSON")   : return AssetFileType::JSON;
    case fnv64_c(".OXASSET"): return AssetFileType::Meta;
    case fnv64_c(".KTX2")   : return AssetFileType::KTX2;
    case fnv64_c(".LUA")    : return AssetFileType::LUA;
    default                 : return AssetFileType::None;
  }
}

auto AssetManager::to_asset_type_sv(AssetType type) -> std::string_view {
  ZoneScoped;

  switch (type) {
    case AssetType::None    : return "None";
    case AssetType::Shader  : return "Shader";
    case AssetType::Model   : return "Model";
    case AssetType::Texture : return "Texture";
    case AssetType::Material: return "Material";
    case AssetType::Font    : return "Font";
    case AssetType::Scene   : return "Scene";
    case AssetType::Audio   : return "Audio";
    case AssetType::Script  : return "Script";
    default                 : return {};
  }
}

// Caller must hold registry_mutex (shared or exclusive)
auto AssetManager::get_asset_ptr(this AssetManager& self, const UUID& uuid) -> Asset* {
  const auto it = self.asset_registry.find(uuid);
  if (it == self.asset_registry.end()) {
    return nullptr;
  }
  return &it->second;
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

  auto asset_type = AssetType::None;
  switch (to_asset_file_type(path)) {
    case AssetFileType::Meta: {
      return self.register_asset(path);
    }
    case AssetFileType::GLB:
    case AssetFileType::GLTF: {
      asset_type = AssetType::Model;
      break;
    }
    case AssetFileType::PNG:
    case AssetFileType::JPEG:
    case AssetFileType::DDS:
    case AssetFileType::KTX2: {
      asset_type = AssetType::Texture;
      break;
    }
    case AssetFileType::LUA: {
      asset_type = AssetType::Script;
      break;
    }
    default: {
      return UUID(nullptr);
    }
  }

  auto meta_path = stack.format("{}.oxasset", path);
  if (std::filesystem::exists(meta_path)) {
    return self.register_asset(meta_path);
  }

  auto uuid = self.create_asset(asset_type, path);
  if (!uuid) {
    return UUID(nullptr);
  }

  JsonWriter writer{};
  begin_asset_meta(writer, uuid, asset_type);

  switch (asset_type) {
    case AssetType::Model: {
      write_gltf_meta(self, path, writer);
    } break;
    case AssetType::Texture: {
      Texture texture = {};
      write_texture_asset_meta(writer, &texture);
    } break;
    case AssetType::Script: {
      write_script_asset_meta(writer, nullptr);
    } break;
    default:;
  }

  if (!end_asset_meta(writer, path)) {
    return UUID(nullptr);
  }

  return uuid;
}

auto AssetManager::delete_asset(this AssetManager& self, const UUID& uuid) -> void {
  ZoneScoped;

  bool is_loaded = false;
  u64 ref_count = 0;
  {
    auto read_lock = std::shared_lock(self.registry_mutex);
    auto* asset = self.get_asset_ptr(uuid);
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
      auto write_lock = std::unique_lock(self.registry_mutex);
      if (auto* asset = self.get_asset_ptr(uuid)) {
        asset->ref_count = ox::min(asset->ref_count, 1_u64);
      }
    }
    self.unload_asset(uuid);
  }

  {
    auto write_lock = std::unique_lock(self.registry_mutex);
    self.asset_registry.erase(uuid);
  }

  OX_LOG_INFO("Deleted asset {}.", uuid.str());
}

auto AssetManager::register_asset(this AssetManager& self, const std::filesystem::path& path) -> UUID {
  ZoneScoped;
  memory::ScopedStack stack;

  auto meta_json = self.read_meta_file(path);
  if (!meta_json) {
    return UUID(nullptr);
  }

  auto uuid_json = meta_json->doc["uuid"].get_string();
  if (uuid_json.error()) {
    OX_LOG_ERROR("Failed to read asset meta file. `uuid` is missing.");
    return UUID(nullptr);
  }

  auto type_json = meta_json->doc["type"].get_number();
  if (type_json.error()) {
    OX_LOG_ERROR("Failed to read asset meta file. `type` is missing.");
    return UUID(nullptr);
  }

  auto asset_path = path;
  asset_path.replace_extension("");
  auto uuid = UUID::from_string(uuid_json.value_unsafe()).value();
  auto type = static_cast<AssetType>(type_json.value_unsafe().get_uint64());

  if (!self.register_asset(uuid, type, asset_path)) {
    return UUID(nullptr);
  }

  return uuid;
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

auto AssetManager::acquire_ref(this AssetManager& self, const UUID& uuid) -> void {
  ZoneScoped;

  auto read_lock = std::shared_lock(self.registry_mutex);
  auto* asset = self.get_asset_ptr(uuid);
  if (asset && asset->is_loaded()) {
    asset->acquire_ref();
  }
}

auto AssetManager::unload(this AssetManager& self, const UUID& uuid) -> void {
  ZoneScoped;

  self.unload_asset(uuid);
}

auto AssetManager::export_asset(this AssetManager& self, const UUID& uuid, const std::filesystem::path& path) -> bool {
  auto asset = self.get_asset(uuid);
  if (!asset)
    return false;

  JsonWriter writer{};
  begin_asset_meta(writer, uuid, asset->type);

  switch (asset->type) {
    case AssetType::Texture:
    case AssetType::Model  : {
      OX_LOG_ERROR("Cannot export unsupported asset type {}.", to_asset_type_sv(asset->type));
      return false;
    }
    case AssetType::Scene: {
      if (!self.export_scene(asset->uuid, writer, path))
        return false;
    } break;
    case AssetType::Material: {
      if (!self.export_material(asset->uuid, writer, path))
        return false;
    } break;
    case AssetType::Script: {
      if (!self.export_script(asset->uuid, writer, path))
        return false;
    } break;
    default: return false;
  }

  return end_asset_meta(writer, path);
}

auto AssetManager::export_scene(
  this AssetManager& self, const UUID& uuid, JsonWriter& writer, const std::filesystem::path& path
) -> bool {
  ZoneScoped;

  auto scene = self.get_scene(uuid);
  write_scene_asset_meta(writer, scene.value);

  return scene->save_to_file(path);
}

auto AssetManager::export_material(
  this AssetManager& self, const UUID& uuid, JsonWriter& writer, const std::filesystem::path& path
) -> bool {
  ZoneScoped;

  auto material = self.get_material(uuid);

  writer.key("material");
  return write_material_asset_meta(writer, uuid, *material.value);
}

auto AssetManager::export_script(
  this AssetManager& self, const UUID& uuid, JsonWriter& writer, const std::filesystem::path& path
) -> bool {
  ZoneScoped;

  return write_script_asset_meta(writer, nullptr);
}

auto AssetManager::load_asset(this AssetManager& self, const UUID& uuid) -> bool {
  auto read_lock = std::shared_lock(self.registry_mutex);
  auto* asset = self.get_asset_ptr(uuid);
  if (!asset)
    return false;
  auto type = asset->type;
  read_lock.unlock();

  switch (type) {
    case AssetType::Model   : return self.load_model(uuid);
    case AssetType::Texture : return self.load_texture(uuid);
    case AssetType::Scene   : return self.load_scene(uuid);
    case AssetType::Audio   : return self.load_audio(uuid);
    case AssetType::Script  : return self.load_script(uuid);
    case AssetType::Material: return self.load_material(uuid, {});
    default                 :;
  }

  return false;
}

auto AssetManager::unload_asset(this AssetManager& self, const UUID& uuid) -> bool {
  auto read_lock = std::shared_lock(self.registry_mutex);
  auto* asset = self.get_asset_ptr(uuid);
  if (!asset)
    return false;
  auto type = asset->type;
  read_lock.unlock();

  switch (type) {
    case AssetType::Model   : return self.unload_model(uuid);
    case AssetType::Texture : return self.unload_texture(uuid);
    case AssetType::Scene   : return self.unload_scene(uuid);
    case AssetType::Audio   : return self.unload_audio(uuid);
    case AssetType::Script  : return self.unload_script(uuid);
    case AssetType::Material: return self.unload_material(uuid);
    default                 :;
  }

  return false;
}

auto AssetManager::load_texture(this AssetManager& self, const UUID& uuid, TextureLoadInfo info) -> bool {
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

  auto data_source = TextureDataSource{};
  auto source_bytes = std::get_if<std::span<const u8>>(&info.source);
  auto source_path = std::get_if<std::filesystem::path>(&info.source);
  if (source_bytes || (source_path && !source_path->empty())) {
    data_source = info.source;
  } else {
    data_source = asset_path;
  }

  auto texture = Texture::create({
    .source = data_source,
    .level_count = info.level_count,
    .is_srgb = info.is_srgb,
    .target_width = info.target_width,
    .target_height = info.target_height,
    .sampler_info = info.sampler_info,
  });
  if (!texture)
    return false;

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

  auto lock = std::unique_lock(self.materials_mutex);
  if (std::ranges::find(self.dirty_materials, material_id) != self.dirty_materials.end()) {
    return;
  }

  self.dirty_materials.emplace_back(material_id);
}

auto AssetManager::set_material_dirty(this AssetManager& self, const UUID& uuid) -> void {
  ZoneScoped;

  auto reg_lock = std::shared_lock(self.registry_mutex);
  auto* asset = self.get_asset_ptr(uuid);
  if (!asset)
    return;
  auto id = asset->material_id;
  reg_lock.unlock();

  self.set_material_dirty(id);
}

auto AssetManager::set_all_materials_dirty(this AssetManager& self) -> void {
  ZoneScoped;

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

} // namespace ox
