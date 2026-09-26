#include <algorithm>
#include <flecs.h>
#include <glm/common.hpp>

#include "Asset/AssetManager.hpp"
#include "Cinematic/Cinematic.hpp"
#include "Core/App.hpp"
#include "Memory/Stack.hpp"
#include "Scene/Components.hpp"
#include "Scene/Scene.hpp"
#include "Utils/Log.hpp"

namespace ox {
auto wrap_clip_time(f32 time, f32 duration, bool loop) -> f32;

static auto snapshot_camera(flecs::entity camera) -> BoundCameraPose {
  auto pose = BoundCameraPose{};
  if (const auto* transform = camera.try_get<TransformComponent>()) {
    pose.position = transform->position;
    pose.rotation = transform->rotation;
    pose.has_transform = true;
  }
  if (const auto* camera_component = camera.try_get<CameraComponent>()) {
    pose.fov = camera_component->fov;
    pose.has_fov = true;
  }

  return pose;
}

// writes back what every track found before it first touched its target
static auto restore_bindings(const CinematicInstance& instance) -> void {
  ZoneScoped;

  for (const auto& bound : instance.bound_properties) {
    if (!bound.valid || !bound.target.is_alive()) {
      continue;
    }

    auto* base = bound.target.try_get_mut(bound.component);
    if (base == nullptr) {
      continue;
    }

    cinematic::write_value(static_cast<u8*>(base) + bound.offset, bound.kind, bound.restore_value, bound.storage);
    bound.target.modified(bound.component);
  }

  const auto camera_count = glm::min(instance.bound_cameras.size(), instance.camera_restore_poses.size());
  for (usize i = 0; i < camera_count; i++) {
    auto camera = instance.bound_cameras[i];
    const auto& pose = instance.camera_restore_poses[i];
    if (!camera || !camera.is_alive()) {
      continue;
    }

    if (auto* transform = camera.try_get_mut<TransformComponent>(); transform != nullptr && pose.has_transform) {
      transform->position = pose.position;
      transform->rotation = pose.rotation;
      camera.modified<TransformComponent>();
    }
    if (auto* camera_component = camera.try_get_mut<CameraComponent>(); camera_component != nullptr && pose.has_fov) {
      camera_component->fov = pose.fov;
      camera.modified<CameraComponent>();
    }
  }
}

struct ResolvedMember {
  u32 offset = 0;
  CinematicStorage storage = {};
};

// the layout the key kind implies has to be the one the member really has, or a write spills into
// its neighbours: an Int track memcpys into a u8 field, a Float4 track into an f32
static auto resolve_storage(flecs::world& world, flecs::entity type, const CinematicValueKind kind)
  -> option<CinematicStorage> {
  if (!type) {
    return nullopt;
  }

  if (const auto* enum_type = type.try_get<flecs::Enum>()) {
    if (kind != CinematicValueKind::Enum || enum_type->underlying_type == 0) {
      return nullopt;
    }

    type = world.entity(enum_type->underlying_type);
  }

  const auto* component = type.try_get<flecs::Component>();
  if (component == nullptr) {
    return nullopt;
  }

  const auto size = static_cast<u32>(component->size);
  const auto* primitive = type.try_get<flecs::Primitive>();

  switch (kind) {
    case CinematicValueKind::Float: {
      if (primitive == nullptr || (primitive->kind != EcsF32 && primitive->kind != EcsF64)) {
        return nullopt;
      }

      return CinematicStorage{.size = size, .is_signed = true};
    }
    case CinematicValueKind::Int :
    case CinematicValueKind::Enum: {
      if (primitive == nullptr) {
        return nullopt;
      }

      switch (primitive->kind) {
        case EcsI8  :
        case EcsI16 :
        case EcsI32 :
        case EcsI64 :
        case EcsChar:
        case EcsIPtr: return CinematicStorage{.size = size, .is_signed = true};
        case EcsU8  :
        case EcsU16 :
        case EcsU32 :
        case EcsU64 :
        case EcsByte:
        case EcsUPtr: return CinematicStorage{.size = size, .is_signed = false};
        default     : return nullopt;
      }
    }
    case CinematicValueKind::Bool: {
      if (primitive == nullptr || primitive->kind != EcsBool) {
        return nullopt;
      }

      return CinematicStorage{.size = size};
    }
    case CinematicValueKind::Float2:
    case CinematicValueKind::Float3:
    case CinematicValueKind::Float4:
    case CinematicValueKind::Quat  : {
      if (primitive != nullptr || size != value_kind_size(kind)) {
        return nullopt;
      }

      return CinematicStorage{.size = size};
    }
    case CinematicValueKind::Count: break;
  }

  return nullopt;
}

// walks the flecs meta description of `component` down a dotted member path and reports where that
// member sits relative to the component base, so the per-frame write needs no reflection at all
static auto resolve_member(
  flecs::world& world,
  const flecs::entity_t component,
  void* base,
  std::string_view member_path,
  const CinematicValueKind kind
) -> option<ResolvedMember> {
  ZoneScoped;
  memory::ScopedStack stack;

  if (member_path.empty()) {
    return nullopt;
  }

  auto cursor = world.cursor(component, base);
  auto remaining = member_path;
  while (!remaining.empty()) {
    const auto dot = remaining.find('.');
    const auto segment = remaining.substr(0, dot);
    if (segment.empty()) {
      return nullopt;
    }

    if (cursor.push() != 0) {
      return nullopt;
    }

    if (cursor.member(stack.null_terminate_cstr(segment)) != 0) {
      return nullopt;
    }

    remaining = dot == std::string_view::npos ? std::string_view{} : remaining.substr(dot + 1);
  }

  auto* member_ptr = cursor.get_ptr();
  if (member_ptr == nullptr) {
    return nullopt;
  }

  const auto storage = resolve_storage(world, cursor.get_type(), kind);
  if (!storage.has_value()) {
    return nullopt;
  }

  return ResolvedMember{
    .offset = static_cast<u32>(static_cast<u8*>(member_ptr) - static_cast<u8*>(base)),
    .storage = storage.value(),
  };
}

auto Scene::attach_cinematic(this Scene& self, flecs::entity entity) -> bool {
  ZoneScoped;

  const auto* player = entity.try_get<CinematicPlayerComponent>();
  if (player == nullptr) {
    return false;
  }

  const auto instance_it = self.entity_to_cinematic_instance_map.find(entity);
  if (instance_it == self.entity_to_cinematic_instance_map.end()) {
    const auto instance_id = self.cinematic_instances.create_slot(
      CinematicInstance{.cinematic_uuid = player->cinematic_uuid}
    );
    self.entity_to_cinematic_instance_map.emplace(entity, instance_id);

    return true;
  }

  auto* instance = self.cinematic_instances.slot(instance_it->second);
  if (instance == nullptr) {
    return false;
  }

  if (instance->cinematic_uuid != player->cinematic_uuid) {
    instance->cinematic_uuid = player->cinematic_uuid;
    instance->current_time = 0.0f;
    // the old bindings stay until the rebind, which carries their restore values over
    instance->bound = false;
    instance->seek_pending = true;
  }

  return true;
}

auto Scene::detach_cinematic(this Scene& self, flecs::entity entity) -> bool {
  ZoneScoped;

  const auto instance_it = self.entity_to_cinematic_instance_map.find(entity);
  if (instance_it == self.entity_to_cinematic_instance_map.end()) {
    return false;
  }

  self.cinematic_instances.destroy_slot(instance_it->second);
  self.entity_to_cinematic_instance_map.erase(instance_it);

  return true;
}

auto Scene::cinematic_instance(this Scene& self, flecs::entity entity) -> CinematicInstance* {
  ZoneScoped;

  const auto instance_it = self.entity_to_cinematic_instance_map.find(entity);
  if (instance_it == self.entity_to_cinematic_instance_map.end()) {
    return nullptr;
  }

  return self.cinematic_instances.slot(instance_it->second);
}

auto Scene::rebind_cinematic(this Scene& self, flecs::entity entity) -> void {
  ZoneScoped;

  auto* instance = self.cinematic_instance(entity);
  if (instance == nullptr) {
    return;
  }

  // a rebind usually happens with the tracks already applied, the editor adding a track mid-scrub
  // being the common case, so a member that was bound before keeps the value it had back then
  // instead of snapshotting the animated one
  const auto previous_properties = std::move(instance->bound_properties);
  const auto previous_cameras = std::move(instance->bound_cameras);
  const auto previous_poses = std::move(instance->camera_restore_poses);

  instance->bound_properties.clear();
  instance->bound_cameras.clear();
  instance->camera_restore_poses.clear();
  instance->arc_luts.clear();
  instance->bound = false;

  auto cinematic = App::mod<AssetManager>().get_cinematic(instance->cinematic_uuid);
  if (!cinematic) {
    return;
  }

  instance->bound_cameras.assign(cinematic->camera_tracks.size(), flecs::entity{});
  instance->camera_restore_poses.assign(cinematic->camera_tracks.size(), BoundCameraPose{});
  instance->arc_luts.assign(cinematic->camera_tracks.size() * Cinematic::ARC_LUT_SIZE, 0.0f);
  for (usize i = 0; i < cinematic->camera_tracks.size(); i++) {
    const auto& track = cinematic->camera_tracks[i];
    const auto camera = self.world.lookup(track.entity_path.c_str());
    instance->bound_cameras[i] = camera;
    if (!camera) {
      OX_LOG_WARN("Cinematic camera track '{}' targets a missing entity '{}'.", track.name, track.entity_path);
      continue;
    }

    const auto previous = std::ranges::find(previous_cameras, camera);
    const auto previous_index = static_cast<usize>(previous - previous_cameras.begin());
    instance->camera_restore_poses[i] = previous_index < previous_poses.size() ? previous_poses[previous_index]
                                                                               : snapshot_camera(camera);

    if (track.constant_speed) {
      cinematic::build_arc_length_lut(
        track,
        std::span(instance->arc_luts).subspan(i * Cinematic::ARC_LUT_SIZE, Cinematic::ARC_LUT_SIZE)
      );
    }
  }

  instance->bound_properties.assign(cinematic->property_tracks.size(), BoundProperty{});
  for (usize i = 0; i < cinematic->property_tracks.size(); i++) {
    const auto& track = cinematic->property_tracks[i];
    auto& bound = instance->bound_properties[i];
    bound.kind = track.kind;

    const auto target = self.world.lookup(track.entity_path.c_str());
    if (!target) {
      OX_LOG_WARN("Cinematic property track '{}' targets a missing entity '{}'.", track.name, track.entity_path);
      continue;
    }

    const auto component = self.world.lookup(track.component_path.c_str());
    if (!component) {
      OX_LOG_WARN("Cinematic property track '{}' names an unknown component '{}'.", track.name, track.component_path);
      continue;
    }

    auto* base = target.try_get_mut(component);
    if (base == nullptr) {
      OX_LOG_WARN(
        "Cinematic property track '{}': entity '{}' has no '{}'.",
        track.name,
        track.entity_path,
        track.component_path
      );
      continue;
    }

    const auto member = resolve_member(self.world, component, base, track.member_path, track.kind);
    if (!member.has_value()) {
      OX_LOG_WARN(
        "Cinematic property track '{}': '{}' has no {} member '{}'.",
        track.name,
        track.component_path,
        value_kind_name(track.kind),
        track.member_path
      );
      continue;
    }

    bound.target = target;
    bound.component = component;
    bound.offset = member->offset;
    bound.storage = member->storage;
    // read before anything is written, so stopping can put the authored value back
    const auto previous = std::ranges::find_if(previous_properties, [&](const BoundProperty& candidate) {
      return candidate.valid && candidate.target == target && candidate.component == component &&
             candidate.offset == member->offset && candidate.kind == track.kind;
    });
    bound
      .restore_value = previous != previous_properties.end()
                         ? previous->restore_value
                         : cinematic::read_value(static_cast<u8*>(base) + member->offset, track.kind, member->storage);
    bound.valid = true;
  }

  instance->bound = true;
  instance->seek_pending = true;
}

auto Scene::update_cinematics(this Scene& self, const f32 delta_time) -> void {
  ZoneScoped;

  if (self.cinematic_instances.size() == 0) {
    return;
  }

  auto& asset_man = App::mod<AssetManager>();

  for (const auto& [entity, instance_id] : self.entity_to_cinematic_instance_map) {
    auto* player = self.world.entity(entity.id()).try_get_mut<CinematicPlayerComponent>();
    if (player == nullptr) {
      continue;
    }

    {
      auto* instance = self.cinematic_instances.slot(instance_id);
      if (instance == nullptr) {
        continue;
      }

      // a direct component write bypasses the observer, so the swap is caught here too
      if (instance->cinematic_uuid != player->cinematic_uuid) {
        instance->cinematic_uuid = player->cinematic_uuid;
        instance->current_time = 0.0f;
        instance->bound = false;
      }

      if (!instance->bound) {
        self.rebind_cinematic(entity);
      }
    }

    auto* instance = self.cinematic_instances.slot(instance_id);
    if (instance == nullptr || !instance->bound) {
      continue;
    }

    auto cinematic = asset_man.get_cinematic(instance->cinematic_uuid);
    if (!cinematic) {
      continue;
    }

    const auto looping = player->loop || cinematic->loop;
    const auto advancing = self.running && player->playing;
    const auto previous_time = instance->current_time;

    if (advancing) {
      instance->current_time = wrap_clip_time(
        instance->current_time + delta_time * player->speed,
        cinematic->duration,
        looping
      );

      // a one-shot that ran off the end holds its last frame rather than restoring, so a shot can
      // end on the pose it was cut to
      if (!looping && instance->current_time >= cinematic->duration) {
        player->playing = false;
      }
    }

    const auto changed = instance->current_time != previous_time || instance->seek_pending ||
                         instance->was_playing != advancing;
    if (!changed) {
      continue;
    }

    instance->seek_pending = false;
    instance->was_playing = advancing;

    const auto camera_count = glm::min(cinematic->camera_tracks.size(), instance->bound_cameras.size());
    for (usize i = 0; i < camera_count; i++) {
      const auto& track = cinematic->camera_tracks[i];
      auto camera = instance->bound_cameras[i];
      // an empty track has nothing to say about the pose, so it leaves the camera alone rather than
      // snapping it to the origin
      if (!track.enabled || track.waypoints.empty() || !camera || !camera.is_alive()) {
        continue;
      }

      const auto lut_offset = i * Cinematic::ARC_LUT_SIZE;
      const auto lut = instance->arc_luts.size() >= lut_offset + Cinematic::ARC_LUT_SIZE
                         ? std::span<const f32>(instance->arc_luts).subspan(lut_offset, Cinematic::ARC_LUT_SIZE)
                         : std::span<const f32>{};
      const auto sample = cinematic::sample_camera(track, lut, instance->current_time);

      if (auto* transform = camera.try_get_mut<TransformComponent>()) {
        transform->position = sample.position;
        transform->rotation = sample.rotation;
        // the TransformComponent OnSet observer is what pushes this to the GPU mirror
        camera.modified<TransformComponent>();
      }

      if (track.drive_fov) {
        if (auto* camera_component = camera.try_get_mut<CameraComponent>()) {
          camera_component->fov = sample.fov;
          camera.modified<CameraComponent>();
        }
      }
    }

    const auto property_count = glm::min(cinematic->property_tracks.size(), instance->bound_properties.size());
    for (usize i = 0; i < property_count; i++) {
      const auto& track = cinematic->property_tracks[i];
      const auto& bound = instance->bound_properties[i];
      // likewise: a track with no keys must not overwrite the member, otherwise adding one zeroes it
      if (!track.enabled || track.keys.empty() || !bound.valid || !bound.target.is_alive()) {
        continue;
      }

      auto* base = bound.target.try_get_mut(bound.component);
      if (base == nullptr) {
        continue;
      }

      const auto value = cinematic::sample_property(track.keys, track.kind, instance->current_time);
      cinematic::write_value(static_cast<u8*>(base) + bound.offset, track.kind, value, bound.storage);
      bound.target.modified(bound.component);
    }
  }
}

auto Scene::play_cinematic(this Scene& self, flecs::entity entity) -> void {
  ZoneScoped;

  auto* player = entity.try_get_mut<CinematicPlayerComponent>();
  if (player == nullptr) {
    return;
  }

  player->playing = true;

  if (auto* instance = self.cinematic_instance(entity)) {
    // a one-shot that ran off the end is parked on its last frame, and resuming from there would
    // stop it again on the very next update
    if (auto cinematic = App::mod<AssetManager>().get_cinematic(instance->cinematic_uuid)) {
      if (!player->loop && !cinematic->loop && instance->current_time >= cinematic->duration) {
        instance->current_time = 0.0f;
      }
    }

    instance->seek_pending = true;
  }
}

auto Scene::stop_cinematic(this Scene& self, flecs::entity entity) -> void {
  ZoneScoped;

  auto* player = entity.try_get_mut<CinematicPlayerComponent>();
  if (player == nullptr) {
    return;
  }

  player->playing = false;

  auto* instance = self.cinematic_instance(entity);
  if (instance == nullptr) {
    return;
  }

  instance->current_time = 0.0f;
  instance->was_playing = false;
  instance->seek_pending = false;

  if (player->restore_on_stop) {
    restore_bindings(*instance);
  }
}

auto Scene::restore_cinematics(this Scene& self) -> void {
  ZoneScoped;

  self.cinematic_instances.for_each_active([](usize, const CinematicInstance& instance) {
    if (instance.bound) {
      restore_bindings(instance);
    }
  });
}

auto Scene::seek_cinematic(this Scene& self, flecs::entity entity, const f32 time) -> void {
  ZoneScoped;

  auto* instance = self.cinematic_instance(entity);
  if (instance == nullptr) {
    return;
  }

  instance->current_time = glm::clamp(time, 0.0f, self.cinematic_duration(entity));
  instance->seek_pending = true;
}

auto Scene::cinematic_time(this Scene& self, flecs::entity entity) -> f32 {
  ZoneScoped;

  const auto instance_it = self.entity_to_cinematic_instance_map.find(entity);
  if (instance_it == self.entity_to_cinematic_instance_map.end()) {
    return 0.0f;
  }

  auto* instance = self.cinematic_instances.slot(instance_it->second);

  return instance != nullptr ? instance->current_time : 0.0f;
}

auto Scene::cinematic_duration(this Scene& self, flecs::entity entity) -> f32 {
  ZoneScoped;

  const auto instance_it = self.entity_to_cinematic_instance_map.find(entity);
  if (instance_it == self.entity_to_cinematic_instance_map.end()) {
    return 0.0f;
  }

  auto* instance = self.cinematic_instances.slot(instance_it->second);
  if (instance == nullptr) {
    return 0.0f;
  }

  auto cinematic = App::mod<AssetManager>().get_cinematic(instance->cinematic_uuid);

  return cinematic ? cinematic->duration : 0.0f;
}
} // namespace ox
