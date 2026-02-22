#include "Asset/AssetManager.hpp"

#include <vuk/Types.hpp>
#include <vuk/vsl/Core.hpp>

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

auto write_scene_asset_meta(JsonWriter& writer, Scene* scene) -> bool {
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
  if (path.has_extension() && path.extension() != "oxasset") {
    meta_path += ".oxasset";
  }

  auto file = File(meta_path, FileAccess::Write);
  file.write(writer.stream.view());
  file.close();
  return true;
}

auto AssetManager::init() -> std::expected<void, std::string> {
  ZoneScoped;

  null_material = create_asset(AssetType::Material);
  load_material(null_material, {});

  return {};
}

auto AssetManager::deinit() -> std::expected<void, std::string> {
  ZoneScoped;

  for (auto& [uuid, asset] : asset_registry) {
    // leak check
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

  asset_registry.clear();
  dirty_materials.clear();
  model_map.reset();
  texture_map.reset();
  material_map.reset();
  scene_map.reset();
  audio_map.reset();
  script_map.reset();

  return {};
}

auto AssetManager::registry() const -> const AssetRegistry& { return asset_registry; }

auto AssetManager::read_meta_file(const std::filesystem::path& path) -> std::unique_ptr<AssetMetaFile> {
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

auto AssetManager::create_asset(const AssetType type, const std::filesystem::path& path) -> UUID {
  const auto uuid = UUID::generate_random();
  auto [asset_it, inserted] = asset_registry.try_emplace(uuid);
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

auto AssetManager::import_asset(const std::filesystem::path& path) -> UUID {
  ZoneScoped;
  memory::ScopedStack stack;

  if (!std::filesystem::exists(path)) {
    OX_LOG_ERROR("Trying to import an asset '{}' that doesn't exist.", path);
    return UUID(nullptr);
  }

  auto asset_type = AssetType::None;
  switch (this->to_asset_file_type(path)) {
    case AssetFileType::Meta: {
      return this->register_asset(path);
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
    case ox::AssetFileType::LUA: {
      asset_type = AssetType::Script;
      break;
    }
    default: {
      return UUID(nullptr);
    }
  }

  // Check for meta file before creating new asset
  auto meta_path = stack.format("{}.oxasset", path);
  if (std::filesystem::exists(meta_path)) {
    return this->register_asset(meta_path);
  }

  auto uuid = this->create_asset(asset_type, path);
  if (!uuid) {
    return UUID(nullptr);
  }

  JsonWriter writer{};
  begin_asset_meta(writer, uuid, asset_type);

  switch (asset_type) {
    case AssetType::Model: {
      write_gltf_meta(*this, path, writer);
    } break;
    case AssetType::Texture: {
      Texture texture = {};

      write_texture_asset_meta(writer, &texture);
    } break;
    case ox::AssetType::Script: {
      write_script_asset_meta(writer, nullptr);
      break;
    }
    default:;
  }

  if (!end_asset_meta(writer, path)) {
    return UUID(nullptr);
  }

  return uuid;
}

auto AssetManager::delete_asset(const UUID& uuid) -> void {
  ZoneScoped;

  auto* asset = this->get_asset(uuid);
  if (asset->ref_count > 0) {
    OX_LOG_WARN("Deleting alive asset {} with {} references!", asset->uuid.str(), asset->ref_count);
  }

  if (asset->is_loaded()) {
    asset->ref_count = ox::min(asset->ref_count, 1_u64);
    this->unload_asset(uuid);

    {
      auto write_lock = std::unique_lock(registry_mutex);
      asset_registry.erase(uuid);
    }
  }

  OX_LOG_TRACE("Deleted asset {}.", uuid.str());
}

auto AssetManager::register_asset(const std::filesystem::path& path) -> UUID {
  ZoneScoped;

  memory::ScopedStack stack;

  auto meta_json = read_meta_file(path);
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

  if (!this->register_asset(uuid, type, asset_path)) {
    return UUID(nullptr);
  }

  return uuid;
}

auto AssetManager::register_asset(const UUID& uuid, AssetType type, const std::filesystem::path& path) -> bool {
  ZoneScoped;

  auto write_lock = std::unique_lock(registry_mutex);

  auto [asset_it, inserted] = asset_registry.try_emplace(uuid);
  if (!inserted) {
    if (asset_it != asset_registry.end()) {
      // Tried a reinsert, asset already exists
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

auto AssetManager::acquire_ref(const UUID& uuid) -> void {
  ZoneScoped;

  auto* asset = get_asset(uuid);
  if (asset && asset->is_loaded()) {
    asset->acquire_ref();
  }
}

auto AssetManager::release_ref(const UUID& uuid) -> void {
  ZoneScoped;

  unload_asset(uuid);
}

auto AssetManager::export_asset(const UUID& uuid, const std::filesystem::path& path) -> bool {
  auto* asset = this->get_asset(uuid);

  JsonWriter writer{};
  begin_asset_meta(writer, uuid, asset->type);

  switch (asset->type) {
    case AssetType::Texture:
    case AssetType::Model  : {
      // Exporting these seems pointless, so just dont support it
      OX_LOG_ERROR("Cannot export unsupported asset type {}.", to_asset_type_sv(asset->type));
      return false;
    } break;
    case AssetType::Scene: {
      if (!this->export_scene(asset->uuid, writer, path)) {
        return false;
      }
    } break;
    case ox::AssetType::Material: {
      if (!this->export_material(asset->uuid, writer, path)) {
        return false;
      }
    } break;
    case ox::AssetType::Script: {
      if (!this->export_script(asset->uuid, writer, path)) {
        return false;
      }
    } break;
    default: return false;
  }

  return end_asset_meta(writer, path);
}

auto AssetManager::export_scene(const UUID& uuid, JsonWriter& writer, const std::filesystem::path& path) -> bool {
  ZoneScoped;

  auto* scene = this->get_scene(uuid);
  OX_CHECK_NULL(scene);
  write_scene_asset_meta(writer, scene);

  return scene->save_to_file(path);
}

auto AssetManager::export_material(const UUID& uuid, JsonWriter& writer, const std::filesystem::path& path) -> bool {
  ZoneScoped;

  auto* material = this->get_material(uuid);
  OX_CHECK_NULL(material);

  writer.key("material");
  auto result = write_material_asset_meta(writer, uuid, *material);

  return result;
}

auto AssetManager::export_script(const UUID& uuid, JsonWriter& writer, const std::filesystem::path& path) -> bool {
  ZoneScoped;

  return write_texture_asset_meta(writer, nullptr);
}

auto AssetManager::load_asset(const UUID& uuid) -> bool {
  const auto* asset = this->get_asset(uuid);
  switch (asset->type) {
    case AssetType::Model: {
      return this->load_model(uuid);
    }
    case AssetType::Texture: {
      return this->load_texture(uuid);
    }
    case AssetType::Scene: {
      return this->load_scene(uuid);
    }
    case AssetType::Audio: {
      return this->load_audio(uuid);
    }
    case AssetType::Script: {
      return this->load_script(uuid);
    }
    case AssetType::Material: {
      return this->load_material(uuid, {});
    }
    default:;
  }

  return false;
}

auto AssetManager::unload_asset(const UUID& uuid) -> bool {
  const auto* asset = this->get_asset(uuid);
  OX_CHECK_NULL(asset);
  switch (asset->type) {
    case AssetType::Model: {
      return this->unload_model(uuid);
    } break;
    case AssetType::Texture: {
      return this->unload_texture(uuid);
    } break;
    case AssetType::Scene: {
      return this->unload_scene(uuid);
    } break;
    case AssetType::Audio: {
      return this->unload_audio(uuid);
      break;
    }
    case AssetType::Script: {
      return this->unload_script(uuid);
      break;
    }
    case AssetType::Material: {
      return this->unload_material(uuid);
      break;
    }
    default:;
  }

  return false;
}

auto AssetManager::load_texture(const UUID& uuid, TextureLoadInfo info) -> bool {
  ZoneScoped;

  auto read_lock = std::shared_lock(textures_mutex);
  auto* asset = this->get_asset(uuid);
  OX_CHECK_NULL(asset);
  asset->acquire_ref();

  if (asset->is_loaded()) {
    return true;
  }

  read_lock.unlock();

  {
    Texture texture{};
    texture.create(asset->path, std::move(info));

    auto write_lock = std::unique_lock(textures_mutex);
    asset->texture_id = texture_map.create_slot(std::move(texture));

    OX_LOG_INFO("Loaded texture {} {}.", asset->uuid.str(), SlotMap_decode_id(asset->texture_id).index);
  }

  return true;
}

auto AssetManager::unload_texture(const UUID& uuid) -> bool {
  ZoneScoped;

  auto* asset = this->get_asset(uuid);
  if (!asset || !(asset->is_loaded() && asset->release_ref())) {
    return false;
  }

  texture_map.destroy_slot(asset->texture_id);
  asset->texture_id = TextureID::Invalid;

  OX_LOG_TRACE("Unloaded texture {}", uuid.str());

  return true;
}

auto AssetManager::is_texture_loaded(const UUID& uuid) -> bool {
  ZoneScoped;

  std::shared_lock _(textures_mutex);
  auto* asset = this->get_asset(uuid);
  if (!asset) {
    return false;
  }

  return asset->is_loaded();
}

auto AssetManager::load_material(const UUID& uuid, const Material& info) -> bool {
  ZoneScoped;

  auto* asset = this->get_asset(uuid);
  OX_CHECK_NULL(asset);
  asset->acquire_ref();

  if (asset->is_loaded()) {
    auto* material = get_material(asset->material_id);
    acquire_ref(material->albedo_texture);
    acquire_ref(material->normal_texture);
    acquire_ref(material->emissive_texture);
    acquire_ref(material->metallic_roughness_texture);
    acquire_ref(material->occlusion_texture);

    return true;
  }

  asset->material_id = material_map.create_slot({
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

  this->set_material_dirty(asset->material_id);

  return true;
}

auto AssetManager::unload_material(const UUID& uuid) -> bool {
  ZoneScoped;

  auto* asset = this->get_asset(uuid);
  OX_CHECK_NULL(asset);
  if (!(asset->is_loaded() && asset->release_ref())) {
    return false;
  }

  const auto* material = this->get_material(asset->material_id);
  if (material->albedo_texture) {
    this->unload_texture(material->albedo_texture);
  }

  if (material->normal_texture) {
    this->unload_texture(material->normal_texture);
  }

  if (material->emissive_texture) {
    this->unload_texture(material->emissive_texture);
  }

  if (material->metallic_roughness_texture) {
    this->unload_texture(material->metallic_roughness_texture);
  }

  if (material->occlusion_texture) {
    this->unload_texture(material->occlusion_texture);
  }

  material_map.destroy_slot(asset->material_id);
  asset->material_id = MaterialID::Invalid;

  OX_LOG_INFO("Unloaded material {}", uuid.str());

  return true;
}

auto AssetManager::load_scene(const UUID& uuid) -> bool {
  ZoneScoped;

  auto* asset = this->get_asset(uuid);
  asset->scene_id = this->scene_map.create_slot(std::make_unique<Scene>());
  auto* scene = this->scene_map.slot(asset->scene_id)->get();

  scene->init("unnamed_scene");

  if (!scene->load_from_file(asset->path)) {
    return false;
  }

  asset->acquire_ref();
  return true;
}

auto AssetManager::unload_scene(const UUID& uuid) -> bool {
  ZoneScoped;

  auto* asset = this->get_asset(uuid);
  OX_CHECK_NULL(asset);
  if (!(asset->is_loaded() && asset->release_ref())) {
    return false;
  }

  scene_map.destroy_slot(asset->scene_id);
  asset->scene_id = SceneID::Invalid;

  OX_LOG_INFO("Unloaded scene {}", uuid.str());

  return true;
}

auto AssetManager::load_audio(const UUID& uuid) -> bool {
  ZoneScoped;

  auto* asset = this->get_asset(uuid);
  OX_CHECK_NULL(asset);
  asset->acquire_ref();

  if (asset->is_loaded()) {
    return true;
  }

  AudioSource audio{};
  audio.load(asset->path);
  asset->audio_id = audio_map.create_slot(std::move(audio));

  OX_LOG_INFO("Loaded audio {}", uuid.str());

  return true;
}

auto AssetManager::unload_audio(const UUID& uuid) -> bool {
  ZoneScoped;

  auto* asset = this->get_asset(uuid);
  if (!asset || !(asset->is_loaded() && asset->release_ref())) {
    return false;
  }

  auto* audio = this->get_audio(asset->audio_id);
  OX_CHECK_NULL(audio);
  audio->unload();

  audio_map.destroy_slot(asset->audio_id);
  asset->audio_id = AudioID::Invalid;

  OX_LOG_INFO("Unloaded audio {}.", uuid.str());

  return true;
}

auto AssetManager::load_script(const UUID& uuid) -> bool {
  ZoneScoped;

  auto* asset = this->get_asset(uuid);
  OX_CHECK_NULL(asset);
  asset->acquire_ref();

  if (asset->is_loaded())
    return true;

  asset->script_id = script_map.create_slot(std::make_unique<LuaSystem>());
  auto* system = script_map.slot(asset->script_id);
  system->get()->load(asset->path);

  OX_LOG_INFO("Loaded script {} {}.", asset->uuid.str(), SlotMap_decode_id(asset->script_id).index);

  return true;
}

auto AssetManager::unload_script(const UUID& uuid) -> bool {
  ZoneScoped;

  auto* asset = this->get_asset(uuid);
  if (!asset || !(asset->is_loaded() && asset->release_ref())) {
    return false;
  }

  script_map.destroy_slot(asset->script_id);
  asset->script_id = ScriptID::Invalid;

  OX_LOG_INFO("Unloaded script {}.", uuid.str());

  return true;
}

auto AssetManager::get_asset(const UUID& uuid) -> Asset* {
  ZoneScoped;

  auto read_lock = std::shared_lock(registry_mutex);
  const auto it = asset_registry.find(uuid);
  if (it == asset_registry.end()) {
    return nullptr;
  }

  return &it->second;
}

auto AssetManager::get_model(const UUID& uuid) -> Model* {
  ZoneScoped;

  const auto* asset = this->get_asset(uuid);
  if (asset == nullptr) {
    return nullptr;
  }

  OX_CHECK_EQ(asset->type, AssetType::Model);
  if (asset->type != AssetType::Model || asset->model_id == ModelID::Invalid) {
    return nullptr;
  }

  return model_map.slot(asset->model_id);
}

auto AssetManager::get_model(const ModelID model_id) -> Model* {
  ZoneScoped;

  if (model_id == ModelID::Invalid) {
    return nullptr;
  }

  return model_map.slot(model_id);
}

auto AssetManager::get_texture(const UUID& uuid) -> Texture* {
  ZoneScoped;

  const auto* asset = this->get_asset(uuid);
  if (asset == nullptr) {
    return nullptr;
  }

  OX_CHECK_EQ(asset->type, AssetType::Texture);
  if (asset->type != AssetType::Texture || asset->texture_id == TextureID::Invalid) {
    return nullptr;
  }

  return texture_map.slot(asset->texture_id);
}

auto AssetManager::get_texture(const TextureID texture_id) -> Texture* {
  ZoneScoped;

  if (texture_id == TextureID::Invalid) {
    return nullptr;
  }

  return texture_map.slot(texture_id);
}

auto AssetManager::get_null_material() -> const Asset * {
  return get_asset(null_material);
}

auto AssetManager::get_material(const UUID& uuid) -> Material* {
  ZoneScoped;

  const auto* asset = this->get_asset(uuid);
  if (asset == nullptr) {
    return nullptr;
  }

  OX_CHECK_EQ(asset->type, AssetType::Material);
  if (asset->type != AssetType::Material || asset->material_id == MaterialID::Invalid) {
    return nullptr;
  }

  return material_map.slot(asset->material_id);
}

auto AssetManager::get_material(const MaterialID material_id) -> Material* {
  ZoneScoped;

  if (material_id == MaterialID::Invalid) {
    return nullptr;
  }

  return material_map.slot(material_id);
}

auto AssetManager::set_material_dirty(MaterialID material_id) -> void {
  ZoneScoped;

  std::shared_lock shared_lock(materials_mutex);
  if (std::ranges::find(dirty_materials, material_id) != dirty_materials.end()) {
    return;
  }

  shared_lock.unlock();
  materials_mutex.lock();
  dirty_materials.emplace_back(material_id);
  materials_mutex.unlock();
}

auto AssetManager::set_material_dirty(const UUID& uuid) -> void {
  ZoneScoped;

  auto material = get_asset(uuid);
  set_material_dirty(material->material_id);
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

  auto read_lock = std::shared_lock(self.materials_mutex);
  auto dirty_materials = std::vector(self.dirty_materials);

  read_lock.unlock();
  auto write_lock = std::unique_lock(self.materials_mutex);
  self.dirty_materials.clear();

  return dirty_materials;
}

auto AssetManager::get_scene(const UUID& uuid) -> Scene* {
  ZoneScoped;

  const auto* asset = this->get_asset(uuid);
  if (asset == nullptr) {
    return nullptr;
  }

  OX_CHECK_EQ(asset->type, AssetType::Scene);
  if (asset->type != AssetType::Scene || asset->scene_id == SceneID::Invalid) {
    return nullptr;
  }

  return scene_map.slot(asset->scene_id)->get();
}

auto AssetManager::get_scene(const SceneID scene_id) -> Scene* {
  ZoneScoped;

  if (scene_id == SceneID::Invalid) {
    return nullptr;
  }

  return scene_map.slot(scene_id)->get();
}

auto AssetManager::get_audio(const UUID& uuid) -> AudioSource* {
  const auto* asset = this->get_asset(uuid);
  if (asset == nullptr) {
    return nullptr;
  }

  OX_CHECK_EQ(asset->type, AssetType::Audio);
  if (asset->type != AssetType::Audio || asset->audio_id == AudioID::Invalid) {
    return nullptr;
  }

  return audio_map.slot(asset->audio_id);
}

auto AssetManager::get_audio(const AudioID audio_id) -> AudioSource* {
  ZoneScoped;

  if (audio_id == AudioID::Invalid) {
    return nullptr;
  }

  return audio_map.slot(audio_id);
}

auto AssetManager::get_script(const UUID& uuid) -> LuaSystem* {
  const auto* asset = this->get_asset(uuid);
  if (asset == nullptr) {
    return nullptr;
  }

  OX_CHECK_EQ(asset->type, AssetType::Script);
  if (asset->type != AssetType::Script || asset->script_id == ScriptID::Invalid) {
    return nullptr;
  }

  return script_map.slot(asset->script_id)->get();
}

auto AssetManager::get_script(ScriptID script_id) -> LuaSystem* {
  ZoneScoped;

  if (script_id == ScriptID::Invalid) {
    return nullptr;
  }

  return script_map.slot(script_id)->get();
}
} // namespace ox
