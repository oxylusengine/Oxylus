#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <flecs.h>
#include <string>
#include <string_view>
#include <utility>

#include "Asset/AssetManager.hpp"
#include "Asset/Model.hpp"
#include "Audio/AudioEngine.hpp"
#include "Core/App.hpp"
#include "Core/VFS.hpp"
#include "DotNetHosts.hpp"
#include "Memory/Stack.hpp"
#include "Scene/Components.hpp"
#include "Scene/Scene.hpp"
#include "Scripting/DotNetScripting.hpp"

namespace ox {
static auto to_view(OxStringView view) -> std::string_view { return std::string_view(view.data, view.length); }

static auto to_uuid(const OxAssetId& id) -> UUID {
  auto bytes = std::array<u8, 16>{};
  std::memcpy(bytes.data(), &id, sizeof(id));
  if (auto uuid = UUID::from_bytes(bytes)) {
    return *uuid;
  }

  return UUID(nullptr);
}

static auto to_asset_id(const UUID& uuid) -> OxAssetId {
  auto id = OxAssetId{};
  std::memcpy(&id, uuid.bytes().data(), sizeof(id));
  return id;
}

static auto asset_manager() -> AssetManager* {
  return App::get() && App::has_mod<AssetManager>() ? &App::mod<AssetManager>() : nullptr;
}

static auto audio_engine() -> AudioEngine* {
  return App::get() && App::has_mod<AudioEngine>() ? &App::mod<AudioEngine>() : nullptr;
}

// how much of `text` fits in `out`, the full length is what the caller sizes a retry by
static auto write_text(std::string_view text, c8* out, u32 capacity) -> u32 {
  if (out) {
    std::memcpy(out, text.data(), std::min<usize>(text.size(), capacity));
  }

  return static_cast<u32>(text.size());
}

static auto write_path(const std::filesystem::path& path, c8* out, u32 capacity) -> u32 {
  return write_text(to_utf8(path), out, capacity);
}

static auto asset_find(OxStringView path, OxAssetId* id) -> bool {
  ZoneScoped;

  *id = {};
  auto* assets = asset_manager();
  if (!assets) {
    return false;
  }

  const auto uuid = assets->find_asset(std::filesystem::path(VFS::ASSETS_DIR) / std::filesystem::path(to_view(path)));
  if (!uuid) {
    return false;
  }

  *id = to_asset_id(uuid);
  return true;
}

static auto asset_acquire(const OxAssetId* id) -> bool {
  ZoneScoped;

  auto* assets = asset_manager();
  const auto uuid = to_uuid(*id);
  return assets && uuid && assets->load_asset(uuid);
}

static auto asset_release(const OxAssetId* id) -> void {
  ZoneScoped;

  auto* assets = asset_manager();
  const auto uuid = to_uuid(*id);
  if (assets && uuid) {
    assets->unload_asset(uuid);
  }
}

static auto asset_info(const OxAssetId* id, OxAssetInfo* info) -> bool {
  *info = {};
  auto* assets = asset_manager();
  const auto uuid = to_uuid(*id);
  if (!assets || !uuid) {
    return false;
  }

  const auto is_loaded = assets->is_loaded(uuid);
  auto asset = assets->get_asset(uuid);
  if (!asset) {
    return false;
  }

  *info = OxAssetInfo{
    .type = static_cast<u32>(asset->type),
    .is_loaded = is_loaded ? 1_u32 : 0_u32,
    .ref_count = asset->ref_count,
  };
  return true;
}

static constexpr auto MATERIAL_TEXTURES = std::array{
  &Material::albedo_texture,
  &Material::normal_texture,
  &Material::emissive_texture,
  &Material::metallic_roughness_texture,
  &Material::occlusion_texture,
};

static constexpr auto MATERIAL_TEXTURE_FIELDS = std::array{
  &OxMaterialData::albedo_texture,
  &OxMaterialData::normal_texture,
  &OxMaterialData::emissive_texture,
  &OxMaterialData::metallic_roughness_texture,
  &OxMaterialData::occlusion_texture,
};

static auto to_material_data(const Material& material) -> OxMaterialData {
  auto data = OxMaterialData{
    .albedo_color =
      {material.albedo_color.x, material.albedo_color.y, material.albedo_color.z, material.albedo_color.w},
    .uv_size = {material.uv_size.x, material.uv_size.y},
    .uv_offset = {material.uv_offset.x, material.uv_offset.y},
    .emissive_color = {material.emissive_color.x, material.emissive_color.y, material.emissive_color.z},
    .roughness_factor = material.roughness_factor,
    .metallic_factor = material.metallic_factor,
    .normal_scale = material.normal_scale,
    .occlusion_strength = material.occlusion_strength,
    .alpha_mode = static_cast<u32>(material.alpha_mode),
    .alpha_cutoff = material.alpha_cutoff,
    .sampling_mode = static_cast<u32>(material.sampling_mode),
    .flip_normal_y = material.flip_normal_y ? 1_u32 : 0_u32,
  };
  for (usize i = 0; i < MATERIAL_TEXTURES.size(); i++) {
    const auto& texture = material.*MATERIAL_TEXTURES[i];
    data.*MATERIAL_TEXTURE_FIELDS[i] = texture ? to_asset_id(texture) : OxAssetId{};
  }

  return data;
}

static auto to_material(const OxMaterialData& data) -> Material {
  auto material = Material{
    .albedo_color = {data.albedo_color.x, data.albedo_color.y, data.albedo_color.z, data.albedo_color.w},
    .uv_size = {data.uv_size.x, data.uv_size.y},
    .uv_offset = {data.uv_offset.x, data.uv_offset.y},
    .emissive_color = {data.emissive_color.x, data.emissive_color.y, data.emissive_color.z},
    .roughness_factor = data.roughness_factor,
    .metallic_factor = data.metallic_factor,
    .normal_scale = data.normal_scale,
    .occlusion_strength = data.occlusion_strength,
    .alpha_mode = static_cast<AlphaMode>(data.alpha_mode),
    .alpha_cutoff = data.alpha_cutoff,
    .sampling_mode = static_cast<SamplingMode>(data.sampling_mode),
    .flip_normal_y = data.flip_normal_y != 0,
  };
  for (usize i = 0; i < MATERIAL_TEXTURES.size(); i++) {
    material.*MATERIAL_TEXTURES[i] = to_uuid(data.*MATERIAL_TEXTURE_FIELDS[i]);
  }

  return material;
}

static auto asset_materials(const OxAssetId* model, OxAssetId* materials, i32 capacity, i32* count) -> OxStatus {
  ZoneScoped;

  *count = 0;
  auto* assets = asset_manager();
  const auto uuid = to_uuid(*model);
  if (!assets || !uuid) {
    return OxStatus::NotFound;
  }

  const auto found = assets->get_model(uuid);
  if (!found) {
    return OxStatus::NotFound;
  }

  *count = static_cast<i32>(found->materials.size());
  for (usize i = 0; i < std::min(found->materials.size(), static_cast<usize>(std::max(capacity, 0))); i++) {
    materials[i] = to_asset_id(found->materials[i]);
  }

  return OxStatus::Ok;
}

static auto asset_material_get(const OxAssetId* material, OxMaterialData* data) -> OxStatus {
  ZoneScoped;

  *data = {};
  auto* assets = asset_manager();
  const auto uuid = to_uuid(*material);
  if (!assets || !uuid) {
    return OxStatus::NotFound;
  }

  const auto found = assets->get_material(uuid);
  if (!found) {
    return OxStatus::NotFound;
  }

  *data = to_material_data(*found.value);
  return OxStatus::Ok;
}

static auto asset_material_set(const OxAssetId* material, const OxMaterialData* data) -> OxStatus {
  ZoneScoped;

  auto* assets = asset_manager();
  const auto uuid = to_uuid(*material);
  if (!assets || !uuid) {
    return OxStatus::NotFound;
  }

  if (
    data->alpha_mode > static_cast<u32>(AlphaMode::Blend) ||
    data->sampling_mode > static_cast<u32>(SamplingMode::LinearRepeatedAnisotropy)
  ) {
    return OxStatus::InvalidArgument;
  }

  const auto incoming = to_material(*data);
  for (const auto texture : MATERIAL_TEXTURES) {
    if (!(incoming.*texture)) {
      continue;
    }

    const auto asset = assets->get_asset(incoming.*texture);
    if (!asset || asset->type != AssetType::Texture) {
      return OxStatus::InvalidArgument;
    }
  }

  return assets->set_material(uuid, incoming) ? OxStatus::Ok : OxStatus::NotFound;
}

static auto audio_source_of(ecs_world_t* world, u64 entity) -> const AudioSourceComponent* {
  if (entity == 0 || !ecs_is_alive(world, entity)) {
    return nullptr;
  }

  return flecs::entity(world, entity).try_get<AudioSourceComponent>();
}

static auto audio_command(ecs_world_t* world, u64 entity, OxAudioCommand command) -> OxStatus {
  ZoneScoped;

  auto* assets = asset_manager();
  auto* audio = audio_engine();
  const auto* source = audio_source_of(world, entity);
  if (!assets || !audio || !source) {
    return OxStatus::NotFound;
  }

  // the guard keeps the sound alive for the call and no longer
  auto clip = assets->get_audio(source->audio_source);
  if (!clip || !clip->get_source()) {
    return OxStatus::NotFound;
  }

  auto* sound = clip->get_source();
  switch (command) {
    case OxAudioCommand::Play  : audio->play_source(sound); break;
    case OxAudioCommand::Stop  : audio->stop_source(sound); break;
    case OxAudioCommand::Pause : audio->pause_source(sound); break;
    case OxAudioCommand::Resume: audio->unpause_source(sound); break;
  }

  return OxStatus::Ok;
}

static auto audio_state(ecs_world_t* world, u64 entity, OxAudioState* state) -> OxStatus {
  *state = {};
  auto* assets = asset_manager();
  auto* audio = audio_engine();
  const auto* source = audio_source_of(world, entity);
  if (!assets || !audio || !source) {
    return OxStatus::NotFound;
  }

  auto clip = assets->get_audio(source->audio_source);
  if (!clip || !clip->get_source()) {
    return OxStatus::NotFound;
  }

  auto* sound = clip->get_source();
  *state = OxAudioState{
    .is_playing = audio->is_source_playing(sound) ? 1_u32 : 0_u32,
    .cursor = audio->get_source_cursor(sound),
    .length = audio->get_source_length(sound),
  };
  return OxStatus::Ok;
}

static auto audio_set_sound(ecs_world_t* world, u64 entity, const OxAssetId* sound) -> OxStatus {
  ZoneScoped;

  auto* assets = asset_manager();
  const auto* source = audio_source_of(world, entity);
  if (!assets || !source) {
    return OxStatus::NotFound;
  }

  const auto uuid = to_uuid(*sound);
  if (uuid == source->audio_source) {
    return OxStatus::Ok;
  }

  // the component owns a reference on whatever it names, its OnRemove gives back the last one
  if (uuid && !assets->load_asset(uuid)) {
    return OxStatus::NotFound;
  }

  auto updated = *source;
  if (updated.audio_source) {
    assets->unload_asset(updated.audio_source);
  }

  updated.audio_source = uuid;
  flecs::entity(world, entity).set<AudioSourceComponent>(updated);
  return OxStatus::Ok;
}

static auto audio_one_shot(const OxAssetId* sound, f32 volume, const OxVec3* position) -> OxStatus {
  ZoneScoped;

  auto* assets = asset_manager();
  auto* audio = audio_engine();
  if (!assets || !audio) {
    return OxStatus::NotFound;
  }

  auto clip = assets->get_audio(to_uuid(*sound));
  if (!clip || !clip->get_source()) {
    return OxStatus::NotFound;
  }

  const auto world_position = position ? glm::vec3(position->x, position->y, position->z) : glm::vec3();
  const auto is_playing = audio->play_one_shot(clip->get_source(), volume, position ? &world_position : nullptr);
  return is_playing ? OxStatus::Ok : OxStatus::InvalidArgument;
}

static auto scene_spawn(ecs_world_t* world, OxStringView name, u64 parent) -> u64 {
  ZoneScoped;

  auto* scene = scene_of(world);
  if (!scene) {
    return 0;
  }

  // safe naming, a script spawning "Bullet" a hundred times gets a hundred entities rather than the first one back.
  // named against the parent's scope too, or a second "Bullet" under the same parent aborts in child_of
  const auto parent_entity = parent != 0 && ecs_is_alive(world, parent) ? flecs::entity(world, parent) : flecs::entity();
  const auto requested = name.length == 0 ? std::string("entity") : std::string(to_view(name));
  auto entity = scene->create_entity(scene->safe_entity_name(requested, parent_entity), false);
  if (parent_entity) {
    entity.child_of(parent_entity);
  }

  return entity.id();
}

static auto scene_spawn_model(ecs_world_t* world, const OxAssetId* model, u64 parent) -> u64 {
  ZoneScoped;

  auto* scene = scene_of(world);
  auto* assets = asset_manager();
  const auto uuid = to_uuid(*model);
  if (!scene || !assets || !uuid) {
    return 0;
  }

  {
    auto asset = assets->get_asset(uuid);
    if (!asset || asset->type != AssetType::Model) {
      return 0;
    }
  }

  const auto parent_entity = parent != 0 && ecs_is_alive(world, parent) ? flecs::entity(world, parent)
                                                                        : flecs::entity();
  return scene->create_model_entity(uuid, parent_entity).id();
}

static auto scene_spawn_particles(ecs_world_t* world, const OxAssetId* particle_system) -> u64 {
  ZoneScoped;

  auto* scene = scene_of(world);
  auto* assets = asset_manager();
  const auto uuid = to_uuid(*particle_system);
  if (!scene || !assets || !uuid) {
    return 0;
  }

  {
    auto asset = assets->get_asset(uuid);
    if (!asset || asset->type != AssetType::ParticleSystem) {
      return 0;
    }
  }

  return scene->create_particle_system_entity(uuid).id();
}

struct SceneEntity {
  Scene* scene = nullptr;
  flecs::entity entity = {};
};

static auto scene_entity_of(ecs_world_t* world, u64 entity) -> SceneEntity {
  auto* scene = scene_of(world);
  if (!scene || entity == 0 || !ecs_is_alive(world, entity)) {
    return {};
  }

  return {.scene = scene, .entity = flecs::entity(world, entity)};
}

static auto to_vec4(const OxVec4& value) -> glm::vec4 { return glm::vec4(value.x, value.y, value.z, value.w); }

static auto scene_particles_command(ecs_world_t* world, u64 entity, OxParticleCommand command, u32 value) -> void {
  ZoneScoped;

  const auto target = scene_entity_of(world, entity);
  if (!target.scene) {
    return;
  }

  switch (command) {
    case OxParticleCommand::Play     : target.scene->play_particles(target.entity); break;
    case OxParticleCommand::Stop     : target.scene->stop_particles(target.entity); break;
    case OxParticleCommand::Restart  : target.scene->restart_particles(target.entity); break;
    case OxParticleCommand::EmitBurst: target.scene->emit_particle_burst(target.entity, value); break;
  }
}

static auto scene_particles_playing(ecs_world_t* world, u64 entity) -> bool {
  const auto target = scene_entity_of(world, entity);
  return target.scene && target.scene->is_particles_playing(target.entity);
}

static auto scene_particles_set_parameter(ecs_world_t* world, u64 entity, u32 index, const OxVec4* value) -> void {
  ZoneScoped;

  if (const auto target = scene_entity_of(world, entity); target.scene) {
    target.scene->set_particle_parameter(target.entity, index, to_vec4(*value));
  }
}

static auto scene_particles_set_parameter_named(ecs_world_t* world, u64 entity, OxStringView name, const OxVec4* value)
  -> bool {
  ZoneScoped;

  const auto target = scene_entity_of(world, entity);
  return target.scene && target.scene->set_particle_parameter(target.entity, to_view(name), to_vec4(*value));
}

static auto scene_cinematic_command(ecs_world_t* world, u64 entity, OxCinematicCommand command, f32 value) -> void {
  ZoneScoped;

  const auto target = scene_entity_of(world, entity);
  if (!target.scene) {
    return;
  }

  switch (command) {
    case OxCinematicCommand::Play: target.scene->play_cinematic(target.entity); break;
    case OxCinematicCommand::Stop: target.scene->stop_cinematic(target.entity); break;
    case OxCinematicCommand::Seek: target.scene->seek_cinematic(target.entity, value); break;
  }
}

static auto scene_cinematic_state(ecs_world_t* world, u64 entity, OxCinematicState* state) -> void {
  *state = {};
  if (const auto target = scene_entity_of(world, entity); target.scene) {
    *state = OxCinematicState{
      .time = target.scene->cinematic_time(target.entity),
      .duration = target.scene->cinematic_duration(target.entity),
    };
  }
}

static auto scene_input_focused(ecs_world_t* world) -> bool {
  const auto* scene = scene_of(world);
  return scene && scene->input_focused;
}

static auto scene_uuid(ecs_world_t* world, OxAssetId* id) -> bool {
  *id = {};
  const auto* scene = scene_of(world);
  if (!scene) {
    return false;
  }

  *id = to_asset_id(scene->get_uuid());
  return true;
}

static auto scene_save(ecs_world_t* world, OxStringView path) -> OxStatus {
  const auto* scene = scene_of(world);
  if (!scene) {
    return OxStatus::NotFound;
  }

  if (path.length == 0) {
    return OxStatus::InvalidArgument;
  }

  return scene->save_to_file(std::filesystem::path(to_view(path))) ? OxStatus::Ok : OxStatus::Exception;
}

static auto scene_set_ui_dpi_ratio(ecs_world_t* world, f32 ratio) -> void {
  if (const auto* scene = scene_of(world)) {
    scene->set_rml_dpi_ratio(ratio);
  }
}

static auto scene_clear_ui_dpi_ratio(ecs_world_t* world) -> void {
  if (const auto* scene = scene_of(world)) {
    scene->clear_rml_dpi_ratio_override();
  }
}

static auto scene_request_load(const OxAssetId* scene) -> OxStatus {
  ZoneScoped;

  auto* assets = asset_manager();
  const auto uuid = to_uuid(*scene);
  if (!assets || !uuid || !App::get()) {
    return OxStatus::NotFound;
  }

  {
    auto asset = assets->get_asset(uuid);
    if (!asset || asset->type != AssetType::Scene) {
      return OxStatus::NotFound;
    }
  }

  return App::get_event_system().emit(SceneLoadRequestEvent{.scene = uuid}) ? OxStatus::Ok : OxStatus::InvalidArgument;
}

static auto scene_set_terrain_track(ecs_world_t* world, const OxTerrainWaypoint* waypoints, i32 count, f32 smoothing)
  -> OxStatus {
  ZoneScoped;

  auto* scene = scene_of(world);
  if (!scene) {
    return OxStatus::NotFound;
  }

  const auto is_count_valid = count == 0 || (count >= 2 && std::cmp_less_equal(count, TERRAIN_TRACK_MAX_POINTS));
  if (!is_count_valid || (count > 0 && !waypoints) || !std::isfinite(smoothing) || smoothing < 0.0f) {
    return OxStatus::InvalidArgument;
  }

  for (i32 i = 0; i < count; i++) {
    const auto& waypoint = waypoints[i];
    const auto is_finite = std::isfinite(waypoint.position.x) && std::isfinite(waypoint.position.z) &&
                           std::isfinite(waypoint.width) && std::isfinite(waypoint.shoulder);
    if (!is_finite || waypoint.width <= 0.0f || waypoint.shoulder < 0.0f) {
      return OxStatus::InvalidArgument;
    }
  }

  memory::ScopedStack stack;
  auto track = stack.alloc<TerrainWaypoint>(static_cast<usize>(count));
  for (i32 i = 0; i < count; i++) {
    const auto& waypoint = waypoints[i];
    track[static_cast<usize>(i)] = TerrainWaypoint{
      .position = {waypoint.position.x, waypoint.position.z},
      .width = waypoint.width,
      .shoulder = waypoint.shoulder,
    };
  }

  scene->set_terrain_track(track, smoothing);
  return OxStatus::Ok;
}

static auto virtual_dir(OxVfsDir dir) -> const c8* {
  switch (dir) {
    case OxVfsDir::App   : return VFS::APP_DIR;
    case OxVfsDir::Assets: return VFS::ASSETS_DIR;
    case OxVfsDir::Cooked: return VFS::COOKED_DIR;
  }

  return VFS::ASSETS_DIR;
}

static auto vfs_resolve(OxVfsDir dir, OxStringView relative_path, c8* out, u32 capacity) -> u32 {
  if (!App::get()) {
    return 0;
  }

  return write_path(App::get_vfs().resolve_physical_dir(virtual_dir(dir), to_view(relative_path)), out, capacity);
}

static auto vfs_to_physical(OxStringView path, c8* out, u32 capacity) -> u32 {
  return App::get() ? write_path(App::get_vfs().to_physical(to_view(path)), out, capacity) : 0;
}

static auto vfs_to_virtual(OxStringView path, c8* out, u32 capacity) -> u32 {
  return App::get() ? write_path(App::get_vfs().to_virtual(to_view(path)), out, capacity) : 0;
}

auto fill_content_api(OxNativeApi& api) -> void {
  api.asset_find = asset_find;
  api.asset_acquire = asset_acquire;
  api.asset_release = asset_release;
  api.asset_info = asset_info;
  api.asset_materials = asset_materials;
  api.asset_material_get = asset_material_get;
  api.asset_material_set = asset_material_set;
  api.audio_command = audio_command;
  api.audio_state = audio_state;
  api.audio_set_sound = audio_set_sound;
  api.audio_one_shot = audio_one_shot;
  api.scene_spawn = scene_spawn;
  api.scene_spawn_model = scene_spawn_model;
  api.scene_spawn_particles = scene_spawn_particles;
  api.scene_particles_command = scene_particles_command;
  api.scene_particles_playing = scene_particles_playing;
  api.scene_particles_set_parameter = scene_particles_set_parameter;
  api.scene_particles_set_parameter_named = scene_particles_set_parameter_named;
  api.scene_cinematic_command = scene_cinematic_command;
  api.scene_cinematic_state = scene_cinematic_state;
  api.scene_input_focused = scene_input_focused;
  api.scene_uuid = scene_uuid;
  api.scene_save = scene_save;
  api.scene_set_ui_dpi_ratio = scene_set_ui_dpi_ratio;
  api.scene_clear_ui_dpi_ratio = scene_clear_ui_dpi_ratio;
  api.scene_request_load = scene_request_load;
  api.scene_set_terrain_track = scene_set_terrain_track;
  api.vfs_resolve = vfs_resolve;
  api.vfs_to_physical = vfs_to_physical;
  api.vfs_to_virtual = vfs_to_virtual;
}
} // namespace ox
