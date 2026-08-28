#include "CinematicEditorPanel.hpp"

#include <algorithm>
#include <cmath>
#include <glm/gtx/quaternion.hpp>
#include <icons/IconsMaterialDesignIcons.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>
#include <misc/cpp/imgui_stdlib.h>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Editor.hpp"
#include "Memory/Stack.hpp"
#include "Render/DebugRenderer.hpp"
#include "Scene/Components.hpp"
#include "Scene/EntitySerializer.hpp"
#include "UI/UI.hpp"

namespace ox {
constexpr static auto TIMELINE_RULER_HEIGHT = 22.0f;
constexpr static auto TIMELINE_KEY_RADIUS = 5.0f;
constexpr static auto TIMELINE_MIN_SPAN = 0.05f;
constexpr static auto PATH_SAMPLE_COUNT = 96;

// the coarsest of 1/2/5 x 10^n that still leaves `min_pixels` between two ticks
static auto nice_time_step(const f32 seconds_per_pixel, const f32 min_pixels) -> f32 {
  const auto raw = glm::max(seconds_per_pixel * min_pixels, 1e-6f);
  const auto magnitude = std::pow(10.0f, std::floor(std::log10(raw)));
  const auto normalized = raw / magnitude;
  const auto multiplier = normalized <= 1.0f ? 1.0f : (normalized <= 2.0f ? 2.0f : (normalized <= 5.0f ? 5.0f : 10.0f));

  return magnitude * multiplier;
}

// walks a component's flecs meta description and reports every leaf a track can drive, with the
// dotted path the runtime resolves it by
struct CinematicMemberCollector : IEntitySerializer {
  struct Entry {
    std::string path = {};
    CinematicValueKind kind = CinematicValueKind::Float;
  };

  std::vector<Entry> entries = {};
  std::string prefix = {};

  explicit CinematicMemberCollector(flecs::world& world_) : IEntitySerializer(world_) {}

  auto qualify(std::string_view name) -> std::string {
    if (prefix.empty()) {
      return std::string(name);
    }
    if (name.empty()) {
      return prefix;
    }

    return fmt::format("{}.{}", prefix, name);
  }

  auto on_primitive(std::string_view name, Primitive primitive) -> void override {
    auto kind = CinematicValueKind::Int;
    std::visit(
      [&](auto* value) {
        using T = std::remove_pointer_t<decltype(value)>;
        if constexpr (std::is_same_v<T, bool>) {
          kind = CinematicValueKind::Bool;
        } else if constexpr (std::is_floating_point_v<T>) {
          kind = CinematicValueKind::Float;
        }
      },
      primitive
    );

    entries.emplace_back(qualify(name), kind);
  }

  auto on_enum(std::string_view name, ecs_meta_op_kind_t, flecs::entity_t, void*) -> void override {
    entries.emplace_back(qualify(name), CinematicValueKind::Enum);
  }

  auto on_struct(std::string_view name, flecs::meta::op_t* ops, i32 op_count, void* base) -> void override {
    const auto path = qualify(name);
    const auto type_id = ops[0].type;

    if (type_id == world.id<glm::quat>().raw_id()) {
      // xyzw are not independently meaningful, so a quat is one key rather than four
      entries.emplace_back(path, CinematicValueKind::Quat);
      return;
    }

    if (type_id == world.id<glm::vec2>().raw_id()) {
      entries.emplace_back(path, CinematicValueKind::Float2);
    } else if (type_id == world.id<glm::vec3>().raw_id()) {
      entries.emplace_back(path, CinematicValueKind::Float3);
    } else if (type_id == world.id<glm::vec4>().raw_id()) {
      entries.emplace_back(path, CinematicValueKind::Float4);
    }

    auto previous = prefix;
    prefix = path;
    serialize_ops(ops + 1, op_count - 1, base);
    prefix = previous;
  }
};

static auto collect_members(flecs::world& world, flecs::entity target, const flecs::entity_t component)
  -> std::vector<CinematicMemberCollector::Entry> {
  ZoneScoped;

  auto* base = target.try_get_mut(component);
  if (base == nullptr) {
    return {};
  }

  auto collector = CinematicMemberCollector(world);
  collector.serialize(world.entity(component), base);

  return std::move(collector.entries);
}

// perpendicular distance from `point` to the segment ab
static auto point_segment_distance(const glm::vec3& point, const glm::vec3& a, const glm::vec3& b) -> f32 {
  const auto ab = b - a;
  const auto length_sq = glm::dot(ab, ab);
  if (length_sq <= 0.0f) {
    return glm::distance(point, a);
  }

  const auto t = glm::clamp(glm::dot(point - a, ab) / length_sq, 0.0f, 1.0f);

  return glm::distance(point, a + ab * t);
}

static auto quat_angle_between(const glm::quat& a, const glm::quat& b) -> f32 {
  return 2.0f * glm::acos(glm::clamp(glm::abs(glm::dot(glm::normalize(a), glm::normalize(b))), 0.0f, 1.0f));
}

static auto decimate_range(
  std::span<const CameraWaypoint> waypoints,
  const usize first,
  const usize last,
  const f32 tolerance,
  const f32 angle_tolerance,
  std::vector<bool>& keep
) -> void {
  if (last <= first + 1) {
    return;
  }

  auto worst = first;
  auto worst_error = 0.0f;
  for (auto i = first + 1; i < last; i++) {
    const auto distance = point_segment_distance(
      waypoints[i].position,
      waypoints[first].position,
      waypoints[last].position
    );
    // rotation is scaled into the same units as position so one threshold decides both
    const auto span = glm::max(waypoints[last].time - waypoints[first].time, 1e-4f);
    const auto local = (waypoints[i].time - waypoints[first].time) / span;
    const auto expected = glm::slerp(waypoints[first].rotation, waypoints[last].rotation, local);
    const auto angle = quat_angle_between(waypoints[i].rotation, expected);

    const auto error = glm::max(distance / glm::max(tolerance, 1e-5f), angle / glm::max(angle_tolerance, 1e-5f));
    if (error > worst_error) {
      worst_error = error;
      worst = i;
    }
  }

  if (worst_error <= 1.0f) {
    return;
  }

  keep[worst] = true;
  decimate_range(waypoints, first, worst, tolerance, angle_tolerance, keep);
  decimate_range(waypoints, worst, last, tolerance, angle_tolerance, keep);
}

CinematicEditorPanel::CinematicEditorPanel() : EditorPanelState("Cinematic Editor", ICON_MDI_MOVIE_OPEN, false) {
  this->window_default_size = {1100, 620};
}

CinematicEditorPanel::~CinematicEditorPanel() {
  if (asset_uuid) {
    App::mod<AssetManager>().unload_asset(asset_uuid);
  }
}

auto CinematicEditorPanel::active_scene(this CinematicEditorPanel& self) -> Scene* {
  return App::mod<Editor>().get_selected_scene();
}

auto CinematicEditorPanel::editor_camera_viewport(this CinematicEditorPanel& self) -> ViewportPanel* {
  ZoneScoped;

  auto& main_viewport = App::mod<Editor>().main_viewport_panel;
  if (auto* focused = main_viewport.get_focused_viewport(); focused != nullptr && focused->editor_camera) {
    return focused;
  }

  for (auto* viewport : main_viewport.get_visible_viwports()) {
    if (viewport->editor_camera) {
      return viewport;
    }
  }

  return nullptr;
}

auto CinematicEditorPanel::resolve_player(this CinematicEditorPanel& self) -> flecs::entity {
  ZoneScoped;

  auto* scene = self.active_scene();
  if (scene == nullptr || !self.asset_uuid) {
    return {};
  }

  auto found = flecs::entity{};
  scene->world.query_builder<const CinematicPlayerComponent>().build().each(
    [&](flecs::entity e, const CinematicPlayerComponent& player) {
      if (!found && player.cinematic_uuid == self.asset_uuid) {
        found = e;
      }
    }
  );

  return found;
}

auto CinematicEditorPanel::create_player(this CinematicEditorPanel& self) -> void {
  ZoneScoped;

  auto* scene = self.active_scene();
  if (scene == nullptr || !self.asset_uuid) {
    return;
  }

  auto& asset_man = App::mod<AssetManager>();
  if (!asset_man.load_asset(self.asset_uuid)) {
    OX_LOG_ERROR("Couldn't load cinematic {}.", self.asset_uuid.str());
    return;
  }

  auto entity = scene->create_entity("Director", true);
  entity.add<TransformComponent>();
  // the component owns this reference from here on, and its OnRemove observer gives it back
  entity.set<CinematicPlayerComponent>({.cinematic_uuid = self.asset_uuid});
}

auto CinematicEditorPanel::seek(this CinematicEditorPanel& self, const f32 time) -> void {
  self.current_time = glm::clamp(time, 0.0f, glm::max(self.duration, 0.0f));

  auto* scene = self.active_scene();
  const auto player = self.resolve_player();
  if (scene != nullptr && player) {
    scene->seek_cinematic(player, self.current_time);
  }
}

auto CinematicEditorPanel::open_asset(this CinematicEditorPanel& self, const UUID& uuid) -> void {
  ZoneScoped;

  auto& asset_man = App::mod<AssetManager>();

  // the panel holds its own ref for as long as it shows the asset
  if (!asset_man.load_asset(uuid)) {
    OX_LOG_ERROR("Couldn't load cinematic {}.", uuid.str());
    return;
  }

  {
    auto cinematic = asset_man.get_cinematic(uuid);
    if (!cinematic) {
      OX_LOG_ERROR("Cinematic {} is not loaded.", uuid.str());
      asset_man.unload_asset(uuid);
      return;
    }

    self.cinematic_name = cinematic->name;
    self.duration = cinematic->duration;
    self.loop = cinematic->loop;
    self.camera_tracks = cinematic->camera_tracks;
    self.property_tracks = cinematic->property_tracks;
  }

  if (auto asset = asset_man.get_asset(uuid)) {
    self.asset_path = asset->path;
  }

  const auto previous_asset = self.asset_uuid;
  self.asset_uuid = uuid;
  if (previous_asset && previous_asset != uuid) {
    asset_man.unload_asset(previous_asset);
  }

  self.selection = {};
  self.active_camera_track = self.camera_tracks.empty() ? ~0_sz : 0;
  self.view_span = 0.0f;
  self.current_time = 0.0f;
  self.previewing = false;
  self.recording = false;
  self.visible = true;
}

auto CinematicEditorPanel::commit(this CinematicEditorPanel& self, const bool rebind_tracks) -> void {
  ZoneScoped;

  if (!self.asset_uuid) {
    return;
  }

  App::mod<AssetManager>().edit_cinematic(self.asset_uuid, [&self](Cinematic& cinematic) {
    cinematic.name = self.cinematic_name;
    cinematic.duration = self.duration;
    cinematic.loop = self.loop;
    cinematic.camera_tracks = self.camera_tracks;
    cinematic.property_tracks = self.property_tracks;
  });

  auto* scene = self.active_scene();
  if (const auto player = self.resolve_player(); scene != nullptr && player) {
    // the instance caches one resolved binding per track, so adding or removing a track invalidates
    // it wholesale. Re-snapshotting restore values on every key edit would also be wrong.
    if (rebind_tracks) {
      scene->rebind_cinematic(player);
    }

    scene->seek_cinematic(player, self.current_time);
  }
}

auto CinematicEditorPanel::selected_waypoint(this CinematicEditorPanel& self) -> CameraWaypoint* {
  if (self.selection.kind != CinematicTrackKind::Camera || !self.selection.has_key()) {
    return nullptr;
  }

  if (self.selection.track >= self.camera_tracks.size()) {
    return nullptr;
  }

  auto& track = self.camera_tracks[self.selection.track];
  if (self.selection.key >= track.waypoints.size()) {
    return nullptr;
  }

  return &track.waypoints[self.selection.key];
}

auto CinematicEditorPanel::grow_duration_to_fit(this CinematicEditorPanel& self) -> void {
  auto extent = 0.0f;
  for (const auto& track : self.camera_tracks) {
    if (!track.waypoints.empty()) {
      extent = glm::max(extent, track.waypoints.back().time);
    }
  }
  for (const auto& track : self.property_tracks) {
    if (!track.keys.empty()) {
      extent = glm::max(extent, track.keys.back().time);
    }
  }

  self.duration = glm::max(self.duration, extent);
}

auto CinematicEditorPanel::sort_active_keys(this CinematicEditorPanel& self) -> void {
  for (auto& track : self.camera_tracks) {
    std::ranges::stable_sort(track.waypoints, {}, &CameraWaypoint::time);
  }
  for (auto& track : self.property_tracks) {
    std::ranges::stable_sort(track.keys, {}, &CinematicKey::time);
  }
}

auto CinematicEditorPanel::frame_view(this CinematicEditorPanel& self) -> void {
  self.view_span = glm::max(self.duration, TIMELINE_MIN_SPAN);
  self.view_start = 0.0f;
}

auto CinematicEditorPanel::zoom_view(
  this CinematicEditorPanel& self, const f32 factor, const f32 anchor_time, const f32 anchor_fraction
) -> void {
  self.view_span = glm::clamp(self.view_span * factor, TIMELINE_MIN_SPAN, glm::max(self.duration, 1.0f) * 16.0f);
  self.view_start = anchor_time - anchor_fraction * self.view_span;
}

auto CinematicEditorPanel::snap_time(
  this CinematicEditorPanel& self,
  const f32 time,
  const f32 seconds_per_pixel,
  const CinematicSelection& exclude,
  const bool moving_playhead
) -> f32 {
  ZoneScoped;

  const auto& settings = self.timeline_settings;
  // alt is the escape hatch, so a key can still be parked on an arbitrary time without visiting the
  // settings menu first
  if (!settings.snap_enabled || ImGui::GetIO().KeyAlt || (moving_playhead && !settings.snap_playhead)) {
    return time;
  }

  const auto radius = settings.snap_pixels * seconds_per_pixel;
  auto nearest = time;
  auto nearest_distance = radius;
  const auto consider = [&](const f32 candidate) {
    const auto distance = glm::abs(candidate - time);
    if (distance < nearest_distance) {
      nearest_distance = distance;
      nearest = candidate;
    }
  };

  if (settings.snap_to_keys) {
    for (usize t = 0; t < self.camera_tracks.size(); t++) {
      const auto& waypoints = self.camera_tracks[t].waypoints;
      for (usize k = 0; k < waypoints.size(); k++) {
        if (exclude.kind == CinematicTrackKind::Camera && exclude.track == t && exclude.key == k) {
          continue;
        }
        consider(waypoints[k].time);
      }
    }

    for (usize t = 0; t < self.property_tracks.size(); t++) {
      const auto& keys = self.property_tracks[t].keys;
      for (usize k = 0; k < keys.size(); k++) {
        if (exclude.kind == CinematicTrackKind::Property && exclude.track == t && exclude.key == k) {
          continue;
        }
        consider(keys[k].time);
      }
    }

    consider(0.0f);
    consider(self.duration);
  }

  if (settings.snap_to_playhead && !moving_playhead) {
    consider(self.current_time);
  }

  // a key or a marker beats the grid whenever both are in range, so waypoints can be stacked on the
  // same instant across tracks even when that instant is not on a grid line
  if (nearest_distance < radius) {
    return nearest;
  }

  if (settings.snap_to_grid && settings.grid_step > 0.0f) {
    return glm::round(time / settings.grid_step) * settings.grid_step;
  }

  return time;
}

auto CinematicEditorPanel::key_property_track(this CinematicEditorPanel& self, const usize track_index, const f32 time)
  -> bool {
  ZoneScoped;

  if (track_index >= self.property_tracks.size()) {
    return false;
  }

  auto& track = self.property_tracks[track_index];
  auto value = cinematic::sample_property(track.keys, track.kind, time);

  // the resolved offset comes from the instance's binding rather than a second meta walk
  auto* scene = self.active_scene();
  if (const auto player = self.resolve_player(); scene != nullptr && player) {
    if (auto* instance = scene->cinematic_instance(player)) {
      if (track_index < instance->bound_properties.size()) {
        const auto& bound = instance->bound_properties[track_index];
        if (bound.valid && bound.target.is_alive()) {
          if (auto* base = bound.target.try_get_mut(bound.component)) {
            value = cinematic::read_value(static_cast<u8*>(base) + bound.offset, bound.kind);
          }
        }
      }
    }
  }

  const auto existing = std::ranges::find_if(track.keys, [&](const CinematicKey& key) {
    return glm::abs(key.time - time) < 1e-4f;
  });

  if (existing != track.keys.end()) {
    existing->value = value;
  } else {
    track.keys.emplace_back(CinematicKey{.time = time, .value = value});
    std::ranges::stable_sort(track.keys, {}, &CinematicKey::time);
  }

  self.grow_duration_to_fit();

  return true;
}

auto CinematicEditorPanel::add_camera_switch_track(this CinematicEditorPanel& self, const usize camera_track) -> void {
  ZoneScoped;

  auto* scene = self.active_scene();
  if (scene == nullptr || camera_track >= self.camera_tracks.size()) {
    return;
  }

  const auto& track = self.camera_tracks[camera_track];
  const auto target = scene->world.lookup(track.entity_path.c_str());
  if (!target || !target.has<CameraComponent>()) {
    OX_LOG_WARN("Camera track '{}' does not target an entity with a CameraComponent.", track.name);
    return;
  }

  const auto component_path = std::string(
    scene->world.entity(scene->world.id<CameraComponent>().raw_id()).path().c_str()
  );

  auto existing = std::ranges::find_if(self.property_tracks, [&](const CinematicPropertyTrack& candidate) {
    return candidate.entity_path == track.entity_path && candidate.component_path == component_path &&
           candidate.member_path == "active";
  });

  if (existing == self.property_tracks.end()) {
    auto switch_track = CinematicPropertyTrack{};
    switch_track.name = fmt::format("{} active", track.name.empty() ? "Camera" : track.name.c_str());
    switch_track.entity_path = track.entity_path;
    switch_track.component_path = component_path;
    switch_track.member_path = "active";
    switch_track.kind = CinematicValueKind::Bool;
    self.property_tracks.emplace_back(std::move(switch_track));
    existing = self.property_tracks.end() - 1;
  }

  self.selection = {CinematicTrackKind::Property, static_cast<usize>(existing - self.property_tracks.begin()), ~0_sz};
  self.commit(true);
}

auto CinematicEditorPanel::pilot_target(this CinematicEditorPanel& self) -> flecs::entity {
  ZoneScoped;

  auto* scene = self.active_scene();
  if (scene == nullptr) {
    return {};
  }

  auto fallback = flecs::entity{};
  for (const auto& track : self.camera_tracks) {
    if (!track.enabled) {
      continue;
    }

    const auto entity = scene->world.lookup(track.entity_path.c_str());
    if (!entity || !entity.is_alive()) {
      continue;
    }

    // following whichever camera the cinematic switched on is what makes a cut between two cameras
    // visible here, since the editor camera outranks both in the viewport
    if (const auto* camera = entity.try_get<CameraComponent>(); camera != nullptr && camera->active) {
      return entity;
    }

    if (!fallback) {
      fallback = entity;
    }
  }

  return fallback;
}

auto CinematicEditorPanel::snapshot_waypoint(this CinematicEditorPanel& self) -> bool {
  ZoneScoped;

  if (self.active_camera_track >= self.camera_tracks.size()) {
    if (self.camera_tracks.empty()) {
      OX_LOG_WARN("Add a camera track before snapshotting a waypoint.");
      return false;
    }

    self.active_camera_track = 0;
  }

  auto* viewport = self.editor_camera_viewport();
  if (viewport == nullptr || !viewport->editor_camera.has<TransformComponent>()) {
    OX_LOG_WARN("No viewport with an editor camera to snapshot from.");
    return false;
  }

  const auto& transform = viewport->editor_camera.get<TransformComponent>();
  auto waypoint = CameraWaypoint{
    .time = self.current_time,
    .position = transform.position,
    .rotation = transform.rotation,
  };
  if (viewport->editor_camera.has<CameraComponent>()) {
    waypoint.fov = viewport->editor_camera.get<CameraComponent>().fov;
  }

  auto& track = self.camera_tracks[self.active_camera_track];
  // a snapshot at an existing time replaces it, so re-framing a shot does not stack waypoints
  const auto existing = std::ranges::find_if(track.waypoints, [&](const CameraWaypoint& w) {
    return glm::abs(w.time - waypoint.time) < 1e-4f;
  });

  if (existing != track.waypoints.end()) {
    waypoint.easing = existing->easing;
    waypoint.cut = existing->cut;
    *existing = waypoint;
    self.selection =
      {CinematicTrackKind::Camera, self.active_camera_track, static_cast<usize>(existing - track.waypoints.begin())};
  } else {
    track.waypoints.emplace_back(waypoint);
    std::ranges::stable_sort(track.waypoints, {}, &CameraWaypoint::time);
    const auto placed = std::ranges::find_if(track.waypoints, [&](const CameraWaypoint& w) {
      return glm::abs(w.time - waypoint.time) < 1e-4f;
    });
    self.selection =
      {CinematicTrackKind::Camera, self.active_camera_track, static_cast<usize>(placed - track.waypoints.begin())};
  }

  self.grow_duration_to_fit();
  self.commit();

  return true;
}

auto CinematicEditorPanel::capture_tick(this CinematicEditorPanel& self, const f32 delta_time) -> void {
  ZoneScoped;

  if (!self.recording || self.active_camera_track >= self.camera_tracks.size()) {
    return;
  }

  self.capture_accumulator += delta_time;
  if (self.capture_accumulator < self.capture_interval) {
    return;
  }

  self.capture_accumulator = 0.0f;
  self.current_time += self.capture_interval;
  self.duration = glm::max(self.duration, self.current_time);
  std::ignore = self.snapshot_waypoint();
}

auto CinematicEditorPanel::decimate_track(this CinematicEditorPanel& self, const usize track_index) -> void {
  ZoneScoped;

  if (track_index >= self.camera_tracks.size()) {
    return;
  }

  auto& waypoints = self.camera_tracks[track_index].waypoints;
  if (waypoints.size() < 3) {
    return;
  }

  auto keep = std::vector<bool>(waypoints.size(), false);
  keep.front() = true;
  keep.back() = true;
  decimate_range(waypoints, 0, waypoints.size() - 1, self.decimate_tolerance, self.decimate_angle, keep);

  auto kept = std::vector<CameraWaypoint>{};
  kept.reserve(waypoints.size());
  for (usize i = 0; i < waypoints.size(); i++) {
    if (keep[i]) {
      kept.emplace_back(waypoints[i]);
    }
  }

  OX_LOG_INFO("Decimated camera track from {} to {} waypoints.", waypoints.size(), kept.size());
  waypoints = std::move(kept);
  self.selection = {CinematicTrackKind::Camera, track_index, ~0_sz};
  self.commit();
}

auto CinematicEditorPanel::on_update(this CinematicEditorPanel& self) -> void {
  ZoneScoped;

  if (!self.asset_uuid) {
    return;
  }

  const auto delta_time = static_cast<f32>(App::get_timestep().get_seconds());

  if (self.recording) {
    self.capture_tick(delta_time);
  } else if (self.previewing) {
    // the scene's own clock only runs in play mode, so edit-mode preview is driven from here
    auto next = self.current_time + delta_time;
    if (next >= self.duration) {
      next = self.loop ? std::fmod(next, glm::max(self.duration, 1e-4f)) : self.duration;
      if (!self.loop) {
        self.previewing = false;
      }
    }
    self.seek(next);
  }

  // written to every visible viewport rather than the focused one, so releasing the toggle while
  // this panel has focus still clears the pilot
  const auto pilot = self.piloting ? self.pilot_target() : flecs::entity{};
  for (auto* viewport : App::mod<Editor>().main_viewport_panel.get_visible_viwports()) {
    viewport->piloted_camera = pilot;
  }

  if (self.draw_path) {
    self.draw_path_overlay();
  }
}

auto CinematicEditorPanel::draw_path_overlay(this CinematicEditorPanel& self) -> void {
  ZoneScoped;

  auto& debug_renderer = App::mod<DebugRenderer>();

  for (usize i = 0; i < self.camera_tracks.size(); i++) {
    const auto& track = self.camera_tracks[i];
    if (!track.enabled || track.waypoints.size() < 2) {
      continue;
    }

    const auto selected = self.selection.kind == CinematicTrackKind::Camera && self.selection.track == i;
    const auto color = selected ? glm::vec4(1.0f, 0.8f, 0.2f, 1.0f) : glm::vec4(0.35f, 0.65f, 1.0f, 1.0f);

    const auto start = track.waypoints.front().time;
    const auto end = track.waypoints.back().time;
    auto previous_time = start;
    auto previous = cinematic::sample_camera_raw(track, start).position;
    for (auto step = 1; step <= PATH_SAMPLE_COUNT; step++) {
      const auto t = start + (end - start) * (static_cast<f32>(step) / static_cast<f32>(PATH_SAMPLE_COUNT));
      const auto current = cinematic::sample_camera_raw(track, t).position;
      // the camera never travels across a cut, so drawing the jump would invent a path that the
      // shot does not take
      const auto jumped = std::ranges::any_of(track.waypoints, [&](const CameraWaypoint& w) {
        return w.cut && w.time > previous_time && w.time <= t;
      });
      if (!jumped) {
        debug_renderer.draw_line(previous, current, 1.0f, color);
      }
      previous = current;
      previous_time = t;
    }

    for (usize w = 0; w < track.waypoints.size(); w++) {
      const auto is_selected = selected && self.selection.key == w;
      const auto point_color = is_selected              ? glm::vec4(1.0f, 0.4f, 0.2f, 1.0f)
                               : track.waypoints[w].cut ? glm::vec4(1.0f, 0.45f, 0.4f, 1.0f)
                                                        : color;
      debug_renderer.draw_point(track.waypoints[w].position, is_selected ? 0.12f : 0.07f, point_color);
    }
  }
}

auto CinematicEditorPanel::draw_toolbar(this CinematicEditorPanel& self) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  auto* scene = self.active_scene();
  const auto player = self.resolve_player();

  if (UI::button(self.previewing ? ICON_MDI_PAUSE : ICON_MDI_PLAY)) {
    self.previewing = !self.previewing;
    self.recording = false;
  }
  ImGui::SameLine();
  if (UI::button(ICON_MDI_STOP)) {
    self.previewing = false;
    self.recording = false;
    self.seek(0.0f);
  }

  ImGui::SameLine();
  if (UI::toggle_button(ICON_MDI_RECORD, self.recording)) {
    self.recording = !self.recording;
    self.previewing = false;
    self.capture_accumulator = 0.0f;
    if (!self.recording && self.active_camera_track < self.camera_tracks.size()) {
      self.decimate_track(self.active_camera_track);
    }
  }
  UI::tooltip_hover("Sample the editor camera into the active camera track");

  ImGui::SameLine();
  if (UI::button(ICON_MDI_CAMERA_PLUS)) {
    self.snapshot_waypoint();
  }
  UI::tooltip_hover("Add a waypoint at the cursor from the editor camera");

  ImGui::SameLine();
  if (UI::button(ICON_MDI_CONTENT_CUT) && self.snapshot_waypoint()) {
    // the snapshot leaves the new waypoint selected, so this is the one it just placed
    if (auto* waypoint = self.selected_waypoint(); waypoint != nullptr) {
      waypoint->cut = true;
      self.commit();
    }
  }
  UI::tooltip_hover("Add a cut at the cursor: hold the previous shot, then jump to the editor camera");

  ImGui::SameLine();
  if (UI::toggle_button(ICON_MDI_EYE, self.piloting)) {
    self.piloting = !self.piloting;
  }
  UI::tooltip_hover("Look through the first camera track's camera");

  ImGui::SameLine();
  ImGui::TextUnformatted(stack.format_char("{:.2f} / {:.2f}s", self.current_time, self.duration));

  ImGui::SameLine();
  if (UI::button(ICON_MDI_CONTENT_SAVE " Save") && !self.asset_path.empty()) {
    self.commit();
    if (!App::mod<AssetManager>().export_asset(self.asset_uuid, self.asset_path)) {
      OX_LOG_ERROR("Couldn't save cinematic to {}.", self.asset_path);
    }
  }

  if (!player) {
    ImGui::SameLine();
    if (UI::button(ICON_MDI_PLUS " Create Director")) {
      self.create_player();
    }
    UI::tooltip_hover("Nothing in this scene plays this cinematic yet");
  }

  UI::begin_properties();
  if (UI::input_text("Name", &self.cinematic_name)) {
    self.commit();
  }
  if (UI::property<f32>("Duration", &self.duration, 0.01f, 3600.0f)) {
    self.commit();
  }
  if (UI::property("Loop", &self.loop)) {
    self.commit();
  }
  UI::property("Draw path", &self.draw_path);
  UI::property<f32>("Capture interval", &self.capture_interval, 1.0f / 120.0f, 1.0f);
  UI::end_properties();

  if (scene == nullptr) {
    ImGui::TextUnformatted("No scene is open.");
  }
}

auto CinematicEditorPanel::draw_new_track_popup(this CinematicEditorPanel& self) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  auto* scene = self.active_scene();
  if (scene == nullptr) {
    ImGui::TextUnformatted("No scene is open.");
    return;
  }

  auto& world = scene->world;

  // entity picker, restricted to named entities because a track resolves its target by path
  auto entities = std::vector<flecs::entity>{};
  world.query_builder<const TransformComponent>().build().each([&](flecs::entity e, const TransformComponent&) {
    if (e.name().length() > 0) {
      entities.emplace_back(e);
    }
  });

  if (entities.empty()) {
    ImGui::TextUnformatted("No named entities in this scene.");
    return;
  }

  // the picker is held across frames, so an entity that went away has to fall back to a real one
  // before its index reaches the dropdown
  if (!self.picker_entity || !self.picker_entity.is_alive() || !std::ranges::contains(entities, self.picker_entity)) {
    self.picker_entity = entities.front();
    self.picker_component = 0;
    self.picker_member.clear();
  }

  auto entity_labels = std::vector<const c8*>{};
  auto selected_entity = 0;
  entity_labels.reserve(entities.size());
  for (usize i = 0; i < entities.size(); i++) {
    entity_labels.emplace_back(stack.null_terminate_cstr(entities[i].path().c_str()));
    if (entities[i] == self.picker_entity) {
      selected_entity = static_cast<i32>(i);
    }
  }

  UI::begin_properties();
  if (UI::property("Entity", &selected_entity, entity_labels.data(), static_cast<i32>(entity_labels.size()))) {
    self.picker_entity = entities[static_cast<usize>(selected_entity)];
    self.picker_component = 0;
    self.picker_member.clear();
  }
  UI::end_properties();

  if (UI::button(ICON_MDI_CAMERA " Add camera track")) {
    auto track = CinematicCameraTrack{};
    track.name = self.picker_entity.name().c_str();
    track.entity_path = self.picker_entity.path().c_str();
    self.camera_tracks.emplace_back(std::move(track));
    self.active_camera_track = self.camera_tracks.size() - 1;
    self.selection = {CinematicTrackKind::Camera, self.active_camera_track, ~0_sz};
    self.commit(true);
    ImGui::CloseCurrentPopup();
    return;
  }

  ImGui::Separator();

  // only components the entity actually has: the member walk needs a live instance to offset from
  auto components = std::vector<flecs::id>{};
  self.picker_entity.each([&](flecs::id id) {
    if (id.type_id() && scene->component_db.is_component_known(id)) {
      components.emplace_back(id);
    }
  });

  if (components.empty()) {
    ImGui::TextUnformatted("This entity has no reflected components.");
    return;
  }

  // the picker is held across frames, so a component the entity no longer carries has to fall back
  // to a real one before its index reaches the dropdown
  const auto known_component = std::ranges::any_of(components, [&](const flecs::id& id) {
    return id.raw_id() == self.picker_component;
  });
  if (!known_component) {
    self.picker_component = components.front().raw_id();
    self.picker_member.clear();
  }

  auto component_labels = std::vector<const c8*>{};
  auto selected_component = 0;
  component_labels.reserve(components.size());
  for (usize i = 0; i < components.size(); i++) {
    component_labels.emplace_back(stack.null_terminate_cstr(components[i].type_id().name().c_str()));
    if (components[i].raw_id() == self.picker_component) {
      selected_component = static_cast<i32>(i);
    }
  }

  UI::begin_properties();
  if (
    UI::property("Component", &selected_component, component_labels.data(), static_cast<i32>(component_labels.size()))
  ) {
    self.picker_component = components[static_cast<usize>(selected_component)].raw_id();
    self.picker_member.clear();
  }

  const auto members = collect_members(world, self.picker_entity, self.picker_component);
  if (members.empty()) {
    UI::end_properties();
    ImGui::TextUnformatted("This component has no animatable members.");
    return;
  }

  const auto known_member = std::ranges::any_of(members, [&](const CinematicMemberCollector::Entry& entry) {
    return entry.path == self.picker_member;
  });
  if (!known_member) {
    self.picker_member = members.front().path;
    self.picker_kind = members.front().kind;
  }

  auto member_labels = std::vector<const c8*>{};
  auto selected_member = 0;
  member_labels.reserve(members.size());
  for (usize i = 0; i < members.size(); i++) {
    member_labels.emplace_back(stack.null_terminate_cstr(members[i].path));
    if (members[i].path == self.picker_member) {
      selected_member = static_cast<i32>(i);
    }
  }

  if (UI::property("Member", &selected_member, member_labels.data(), static_cast<i32>(member_labels.size()))) {
    self.picker_member = members[static_cast<usize>(selected_member)].path;
    self.picker_kind = members[static_cast<usize>(selected_member)].kind;
  }
  UI::end_properties();

  if (UI::button(ICON_MDI_PLUS " Add property track")) {
    auto track = CinematicPropertyTrack{};
    track.name = fmt::format("{}.{}", self.picker_entity.name().c_str(), self.picker_member);
    track.entity_path = self.picker_entity.path().c_str();
    track.component_path = world.entity(self.picker_component).path().c_str();
    track.member_path = self.picker_member;
    track.kind = self.picker_kind;
    self.property_tracks.emplace_back(std::move(track));
    self.selection = {CinematicTrackKind::Property, self.property_tracks.size() - 1, ~0_sz};
    self.commit(true);
    ImGui::CloseCurrentPopup();
  }
}

auto CinematicEditorPanel::draw_track_list(this CinematicEditorPanel& self) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  if (UI::button(ICON_MDI_PLUS " Add track")) {
    ImGui::OpenPopup("new_cinematic_track");
  }

  if (ImGui::BeginPopup("new_cinematic_track")) {
    self.draw_new_track_popup();
    ImGui::EndPopup();
  }

  ImGui::Separator();

  auto dirty = false;
  auto camera_to_remove = ~0_sz;
  auto property_to_remove = ~0_sz;

  for (usize i = 0; i < self.camera_tracks.size(); i++) {
    auto& track = self.camera_tracks[i];
    ImGui::PushID(static_cast<i32>(i));

    dirty |= ImGui::Checkbox("##enabled", &track.enabled);
    ImGui::SameLine();

    const auto selected = self.selection.kind == CinematicTrackKind::Camera && self.selection.track == i;
    const auto label = stack.format_char(
      "{} {} ({})",
      self.active_camera_track == i ? ICON_MDI_RECORD : ICON_MDI_CAMERA,
      track.name.empty() ? "Camera" : track.name.c_str(),
      track.waypoints.size()
    );
    if (ImGui::Selectable(label, selected)) {
      self.selection = {CinematicTrackKind::Camera, i, ~0_sz};
      self.active_camera_track = i;
    }

    if (ImGui::BeginPopupContextItem("camera_track_ctx")) {
      if (ImGui::MenuItem("Record into this track")) {
        self.active_camera_track = i;
      }
      if (ImGui::MenuItem("Decimate waypoints")) {
        self.decimate_track(i);
      }
      if (ImGui::MenuItem("Delete track")) {
        camera_to_remove = i;
      }
      ImGui::EndPopup();
    }

    ImGui::PopID();
  }

  for (usize i = 0; i < self.property_tracks.size(); i++) {
    auto& track = self.property_tracks[i];
    ImGui::PushID(static_cast<i32>(self.camera_tracks.size() + i));

    dirty |= ImGui::Checkbox("##enabled", &track.enabled);
    ImGui::SameLine();

    const auto selected = self.selection.kind == CinematicTrackKind::Property && self.selection.track == i;
    const auto label = stack.format_char(
      "{} {} ({})",
      ICON_MDI_TUNE,
      track.name.empty() ? track.member_path.c_str() : track.name.c_str(),
      track.keys.size()
    );
    if (ImGui::Selectable(label, selected)) {
      self.selection = {CinematicTrackKind::Property, i, ~0_sz};
    }

    if (ImGui::BeginPopupContextItem("property_track_ctx")) {
      if (ImGui::MenuItem("Delete track")) {
        property_to_remove = i;
      }
      ImGui::EndPopup();
    }

    ImGui::PopID();
  }

  if (camera_to_remove != ~0_sz) {
    self.camera_tracks.erase(self.camera_tracks.begin() + static_cast<std::ptrdiff_t>(camera_to_remove));
    self.selection = {};
    self.active_camera_track = self.camera_tracks.empty() ? ~0_sz : 0;
    dirty = true;
  }

  if (property_to_remove != ~0_sz) {
    self.property_tracks.erase(self.property_tracks.begin() + static_cast<std::ptrdiff_t>(property_to_remove));
    self.selection = {};
    dirty = true;
  }

  if (dirty) {
    self.commit(true);
  }
}

auto CinematicEditorPanel::draw_timeline_settings(this CinematicEditorPanel& self) -> void {
  ZoneScoped;

  auto& settings = self.timeline_settings;

  ImGui::SeparatorText("Snapping");
  UI::begin_properties();
  UI::property("Snapping", &settings.snap_enabled, "Hold Alt while dragging to bypass it");
  UI::property("Snap to grid", &settings.snap_to_grid);
  UI::property<f32>("Grid step", &settings.grid_step, 0.01f, 60.0f, "Seconds between grid lines", 0.01f, "%.3f");
  UI::property("Snap to keys", &settings.snap_to_keys, "Also snaps to the start and to the duration");
  UI::property("Snap to playhead", &settings.snap_to_playhead);
  UI::property("Snap while scrubbing", &settings.snap_playhead);
  UI::property<f32>("Snap radius", &settings.snap_pixels, 2.0f, 40.0f, "Pixels", 0.5f, "%.0f px");
  UI::end_properties();

  ImGui::SeparatorText("View");
  UI::begin_properties();
  UI::property("Follow playhead", &settings.follow_playhead, "Scroll the view to keep the cursor visible");
  UI::property("Track names on rows", &settings.show_row_names);
  UI::property<f32>("Row height", &settings.row_height, 14.0f, 48.0f, nullptr, 0.5f, "%.0f px");
  UI::end_properties();

  ImGui::TextDisabled("Wheel zooms, middle drag pans, double click a key to seek to it.");

  if (UI::button(ICON_MDI_FIT_TO_PAGE_OUTLINE " Fit to duration")) {
    self.frame_view();
  }
}

auto CinematicEditorPanel::draw_timeline_header(this CinematicEditorPanel& self) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  if (UI::button(ICON_MDI_COG)) {
    ImGui::OpenPopup("timeline_settings");
  }
  UI::tooltip_hover("Timeline and snapping settings");

  if (ImGui::BeginPopup("timeline_settings")) {
    self.draw_timeline_settings();
    ImGui::EndPopup();
  }

  ImGui::SameLine();
  if (UI::toggle_button(ICON_MDI_MAGNET, self.timeline_settings.snap_enabled)) {
    self.timeline_settings.snap_enabled = !self.timeline_settings.snap_enabled;
  }
  UI::tooltip_hover("Snapping (hold Alt to bypass)");

  ImGui::SameLine();
  if (UI::button(ICON_MDI_MAGNIFY_MINUS)) {
    self.zoom_view(1.5f, self.view_start + self.view_span * 0.5f, 0.5f);
  }
  ImGui::SameLine();
  if (UI::button(ICON_MDI_MAGNIFY_PLUS)) {
    self.zoom_view(1.0f / 1.5f, self.view_start + self.view_span * 0.5f, 0.5f);
  }
  ImGui::SameLine();
  if (UI::button(ICON_MDI_FIT_TO_PAGE_OUTLINE)) {
    self.frame_view();
  }
  UI::tooltip_hover("Fit the whole duration");

  ImGui::SameLine();
  ImGui::TextDisabled("%s", stack.format_char("{:.2f}s - {:.2f}s", self.view_start, self.view_start + self.view_span));
}

auto CinematicEditorPanel::draw_timeline(this CinematicEditorPanel& self) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  const auto& settings = self.timeline_settings;
  const auto duration = glm::max(self.duration, 1e-3f);

  if (self.view_span <= 0.0f) {
    self.frame_view();
  }

  const auto region = ImGui::GetContentRegionAvail();
  const auto width = glm::max(region.x, 64.0f);
  const auto track_count = self.camera_tracks.size() + self.property_tracks.size();
  const auto rows_height = settings.row_height * static_cast<f32>(glm::max(track_count, 1_sz));
  const auto height = TIMELINE_RULER_HEIGHT + rows_height;

  const auto origin = ImGui::GetCursorScreenPos();
  const auto rows_y = origin.y + TIMELINE_RULER_HEIGHT;

  constexpr auto interaction_flags = ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                     ImGuiButtonFlags_MouseButtonMiddle;

  ImGui::SetCursorScreenPos(origin);
  ImGui::InvisibleButton("##timeline_ruler", ImVec2(width, TIMELINE_RULER_HEIGHT), interaction_flags);
  // claiming the wheel is what stops the enclosing child from scrolling out from under a zoom, and
  // ownership has to be re-registered every frame it is held on hover
  ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY, ImGuiInputFlags_CondHovered);
  const auto ruler_hovered = ImGui::IsItemHovered();
  const auto ruler_active = ImGui::IsItemActive();

  ImGui::SetCursorScreenPos(ImVec2(origin.x, rows_y));
  ImGui::InvisibleButton("##timeline_rows", ImVec2(width, rows_height), interaction_flags);
  ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY, ImGuiInputFlags_CondHovered);
  const auto rows_hovered = ImGui::IsItemHovered();
  const auto rows_active = ImGui::IsItemActive();

  auto& io = ImGui::GetIO();

  // every view change happens before the mapping is built, so nothing drawn this frame is laid out
  // against a range that a later line then moves
  if ((ruler_hovered || rows_hovered) && io.MouseWheel != 0.0f) {
    const auto fraction = glm::clamp((io.MousePos.x - origin.x) / width, 0.0f, 1.0f);
    self.zoom_view(std::pow(0.85f, io.MouseWheel), self.view_start + fraction * self.view_span, fraction);
  }

  if ((ruler_active || rows_active) && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
    self.view_start -= io.MouseDelta.x * (self.view_span / width);
  }

  if (settings.follow_playhead && (self.previewing || self.recording)) {
    if (self.current_time < self.view_start || self.current_time > self.view_start + self.view_span) {
      self.view_start = self.current_time - self.view_span * 0.1f;
    }
  }

  const auto slack = self.view_span * 0.25f;
  self.view_start = glm::clamp(self.view_start, -slack, glm::max(duration - self.view_span + slack, -slack));

  const auto seconds_per_pixel = self.view_span / width;
  const auto view_end = self.view_start + self.view_span;
  const auto to_x = [&](const f32 time) {
    return origin.x + (time - self.view_start) / seconds_per_pixel;
  };
  const auto to_time = [&](const f32 x) {
    return self.view_start + (x - origin.x) * seconds_per_pixel;
  };

  auto* draw_list = ImGui::GetWindowDrawList();
  const auto bottom_right = ImVec2(origin.x + width, origin.y + height);
  draw_list->PushClipRect(origin, bottom_right, true);

  draw_list->AddRectFilled(origin, bottom_right, IM_COL32(24, 24, 28, 255));
  draw_list->AddRectFilled(origin, ImVec2(origin.x + width, rows_y), IM_COL32(34, 34, 40, 255));

  // anything past the duration is not part of the shot, so it reads as out of bounds rather than as
  // more timeline to drop keys onto
  if (duration < view_end) {
    draw_list->AddRectFilled(ImVec2(to_x(duration), rows_y), bottom_right, IM_COL32(16, 16, 18, 255));
  }

  const auto major_step = nice_time_step(seconds_per_pixel, 72.0f);
  const auto minor_step = major_step * 0.2f;
  const auto decimals = major_step >= 1.0f ? 0 : (major_step >= 0.1f ? 1 : 2);

  // the snap grid is drawn only once it is coarse enough to read, otherwise a zoomed out view turns
  // into a solid block that says nothing about where a key would land
  if (settings.snap_enabled && settings.snap_to_grid && settings.grid_step / seconds_per_pixel >= 6.0f) {
    const auto first = static_cast<i64>(std::floor(self.view_start / settings.grid_step));
    const auto last = static_cast<i64>(std::ceil(view_end / settings.grid_step));
    for (auto i = first; i <= last; i++) {
      const auto x = to_x(static_cast<f32>(i) * settings.grid_step);
      draw_list->AddLine(ImVec2(x, rows_y), ImVec2(x, origin.y + height), IM_COL32(44, 44, 52, 255));
    }
  }

  {
    const auto first = static_cast<i64>(std::floor(self.view_start / minor_step));
    const auto last = static_cast<i64>(std::ceil(view_end / minor_step));
    for (auto i = first; i <= last; i++) {
      const auto time = static_cast<f32>(i) * minor_step;
      const auto x = to_x(time);
      const auto major = i % 5 == 0;

      draw_list->AddLine(
        ImVec2(x, major ? origin.y : rows_y - 6.0f),
        ImVec2(x, major ? origin.y + height : rows_y),
        major ? IM_COL32(70, 70, 80, 255) : IM_COL32(56, 56, 64, 255)
      );

      if (major) {
        draw_list->AddText(
          ImVec2(x + 3.0f, origin.y + 3.0f),
          IM_COL32(170, 170, 180, 255),
          stack.format_char("{:.{}f}s", time, decimals)
        );
      }
    }
  }

  // the row the mouse is over, and the key inside it if one is close enough to grab
  auto hover = CinematicSelection{};
  auto hover_row = false;
  auto hover_time = 0.0f;

  auto row = 0_sz;
  const auto draw_row = [&](
                          const CinematicTrackKind kind,
                          const usize track_index,
                          const std::string_view name,
                          const bool enabled,
                          const usize count,
                          auto&& time_of,
                          auto&& cut_at
                        ) {
    const auto top = rows_y + settings.row_height * static_cast<f32>(row);
    const auto bottom = top + settings.row_height;
    const auto center_y = (top + bottom) * 0.5f;
    const auto selected_track = self.selection.kind == kind && self.selection.track == track_index;

    if (selected_track) {
      draw_list->AddRectFilled(ImVec2(origin.x, top), ImVec2(origin.x + width, bottom), IM_COL32(52, 46, 30, 160));
    } else if (row % 2 == 1) {
      draw_list->AddRectFilled(ImVec2(origin.x, top), ImVec2(origin.x + width, bottom), IM_COL32(255, 255, 255, 8));
    }
    draw_list->AddLine(ImVec2(origin.x, bottom), ImVec2(origin.x + width, bottom), IM_COL32(48, 48, 54, 255));

    const auto dim = enabled ? 255 : 90;
    if (count >= 2) {
      draw_list->AddLine(
        ImVec2(to_x(time_of(0)), center_y),
        ImVec2(to_x(time_of(count - 1)), center_y),
        IM_COL32(96, 116, 150, dim),
        1.0f
      );
    }

    if (settings.show_row_names && !name.empty()) {
      draw_list
        ->AddText(ImVec2(origin.x + 4.0f, top + 2.0f), IM_COL32(160, 160, 175, 110), stack.null_terminate_cstr(name));
    }

    for (usize k = 0; k < count; k++) {
      const auto x = to_x(time_of(k));
      if (x < origin.x - 16.0f || x > origin.x + width + 16.0f) {
        continue;
      }

      const auto selected = selected_track && self.selection.key == k;
      const auto cut = cut_at(k);
      const auto color = selected ? IM_COL32(255, 190, 60, dim)
                         : cut    ? IM_COL32(255, 118, 104, dim)
                                  : IM_COL32(140, 190, 255, dim);

      if (cut) {
        // a cut is a boundary, not a pose to blend through, so it gets a bar rather than a diamond
        draw_list->AddRectFilled(ImVec2(x - 2.0f, top + 3.0f), ImVec2(x + 2.0f, bottom - 3.0f), color);
      } else {
        const ImVec2 diamond[4] = {
          {x, center_y - TIMELINE_KEY_RADIUS},
          {x + TIMELINE_KEY_RADIUS, center_y},
          {x, center_y + TIMELINE_KEY_RADIUS},
          {x - TIMELINE_KEY_RADIUS, center_y},
        };
        draw_list->AddConvexPolyFilled(diamond, 4, color);
      }
    }

    // the nearest key wins rather than the first one found, so two keys a few pixels apart still
    // pick the one actually under the cursor
    if (rows_hovered && io.MousePos.y >= top && io.MousePos.y < bottom) {
      hover_row = true;
      hover = {kind, track_index, ~0_sz};
      auto nearest_distance = TIMELINE_KEY_RADIUS + 4.0f;
      for (usize k = 0; k < count; k++) {
        const auto distance = glm::abs(io.MousePos.x - to_x(time_of(k)));
        if (distance < nearest_distance) {
          nearest_distance = distance;
          hover.key = k;
          hover_time = time_of(k);
        }
      }
    }

    row += 1;
  };

  for (usize i = 0; i < self.camera_tracks.size(); i++) {
    const auto& track = self.camera_tracks[i];
    draw_row(
      CinematicTrackKind::Camera,
      i,
      track.name.empty() ? std::string_view("Camera") : std::string_view(track.name),
      track.enabled,
      track.waypoints.size(),
      [&](const usize k) { return track.waypoints[k].time; },
      [&](const usize k) { return track.waypoints[k].cut; }
    );
  }

  for (usize i = 0; i < self.property_tracks.size(); i++) {
    const auto& track = self.property_tracks[i];
    draw_row(
      CinematicTrackKind::Property,
      i,
      track.name.empty() ? std::string_view(track.member_path) : std::string_view(track.name),
      track.enabled,
      track.keys.size(),
      [&](const usize k) { return track.keys[k].time; },
      [&](usize) { return false; }
    );
  }

  auto dirty = false;

  if (hover_row && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    self.selection = hover;
    if (hover.has_key()) {
      self.dragging = hover;
    } else if (hover.kind == CinematicTrackKind::Camera) {
      self.active_camera_track = hover.track;
    }
  }

  if (hover_row && hover.has_key() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    self.previewing = false;
    self.dragging = {};
    self.seek(hover_time);
  }

  if ((hover_row || ruler_hovered) && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    self.context_selection = hover;
    self.context_time = glm::max(self.snap_time(to_time(io.MousePos.x), seconds_per_pixel, {}, false), 0.0f);
    if (hover.has_key()) {
      self.selection = hover;
    }
    ImGui::OpenPopup("timeline_context");
  }

  if (self.dragging.has_key() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    const auto time = glm::max(self.snap_time(to_time(io.MousePos.x), seconds_per_pixel, self.dragging, false), 0.0f);

    // erase and reinsert at the sorted position rather than assigning in place, so the keys stay
    // ordered for sampling and the drag keeps following the same key when it crosses a neighbour
    const auto reinsert = [&](auto& keys, auto time_of) {
      if (keys[self.dragging.key].time == time) {
        return;
      }

      auto moved = keys[self.dragging.key];
      moved.time = time;
      keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(self.dragging.key));
      const auto at = static_cast<usize>(std::ranges::upper_bound(keys, time, {}, time_of) - keys.begin());
      keys.insert(keys.begin() + static_cast<std::ptrdiff_t>(at), moved);
      self.dragging.key = at;
      self.selection = self.dragging;
      dirty = true;
    };

    if (self.dragging.kind == CinematicTrackKind::Camera) {
      if (self.dragging.track < self.camera_tracks.size()) {
        auto& waypoints = self.camera_tracks[self.dragging.track].waypoints;
        if (self.dragging.key < waypoints.size()) {
          reinsert(waypoints, &CameraWaypoint::time);
        }
      }
    } else if (self.dragging.track < self.property_tracks.size()) {
      auto& keys = self.property_tracks[self.dragging.track].keys;
      if (self.dragging.key < keys.size()) {
        reinsert(keys, &CinematicKey::time);
      }
    }
  }

  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    self.dragging = {};
  }

  // scrubbing: the ruler always scrubs, and in the rows a drag that did not start on a key does
  const auto scrubbing = ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                         (ruler_active || (rows_active && !self.dragging.has_key()));
  if (scrubbing) {
    self.previewing = false;
    self.seek(self.snap_time(to_time(io.MousePos.x), seconds_per_pixel, {}, true));
  }

  const auto cursor_x = to_x(self.current_time);
  draw_list->AddLine(ImVec2(cursor_x, origin.y), ImVec2(cursor_x, origin.y + height), IM_COL32(255, 80, 80, 255), 2.0f);
  const ImVec2 handle[3] = {
    {cursor_x - 6.0f, origin.y},
    {cursor_x + 6.0f, origin.y},
    {cursor_x, origin.y + 9.0f},
  };
  draw_list->AddConvexPolyFilled(handle, 3, IM_COL32(255, 80, 80, 255));

  draw_list->PopClipRect();

  if (ImGui::BeginPopup("timeline_context")) {
    const auto& target = self.context_selection;

    ImGui::TextDisabled("%s", stack.format_char("{:.3f}s", self.context_time));
    ImGui::Separator();

    if (ImGui::MenuItem(ICON_MDI_CURSOR_DEFAULT_CLICK " Seek here")) {
      self.previewing = false;
      self.seek(self.context_time);
    }

    if (target.kind == CinematicTrackKind::Camera && target.track < self.camera_tracks.size()) {
      auto& track = self.camera_tracks[target.track];

      if (target.has_key() && target.key < track.waypoints.size()) {
        auto& waypoint = track.waypoints[target.key];
        if (ImGui::MenuItem(ICON_MDI_CONTENT_CUT " Cut to this waypoint", nullptr, waypoint.cut)) {
          waypoint.cut = !waypoint.cut;
          dirty = true;
        }
        UI::tooltip_hover("Hold the previous pose, then jump here with no interpolation");

        if (ImGui::MenuItem(ICON_MDI_DELETE " Delete waypoint")) {
          track.waypoints.erase(track.waypoints.begin() + static_cast<std::ptrdiff_t>(target.key));
          self.selection.key = ~0_sz;
          dirty = true;
        }
      } else if (ImGui::MenuItem(ICON_MDI_CAMERA_PLUS " Waypoint from editor camera")) {
        self.active_camera_track = target.track;
        self.seek(self.context_time);
        self.snapshot_waypoint();
      }
    } else if (target.kind == CinematicTrackKind::Property && target.track < self.property_tracks.size()) {
      auto& track = self.property_tracks[target.track];

      if (target.has_key() && target.key < track.keys.size()) {
        if (ImGui::MenuItem(ICON_MDI_DELETE " Delete key")) {
          track.keys.erase(track.keys.begin() + static_cast<std::ptrdiff_t>(target.key));
          self.selection.key = ~0_sz;
          dirty = true;
        }
      } else if (ImGui::MenuItem(ICON_MDI_KEY_PLUS " Key at this time")) {
        dirty |= self.key_property_track(target.track, self.context_time);
      }
    }

    ImGui::EndPopup();
  }

  if (dirty) {
    self.grow_duration_to_fit();
    self.commit();
  }
}

auto CinematicEditorPanel::draw_curve_view(this CinematicEditorPanel& self) -> void {
  ZoneScoped;

  if (self.selection.kind != CinematicTrackKind::Property || self.selection.track >= self.property_tracks.size()) {
    ImGui::TextUnformatted("Select a property track to plot it.");
    return;
  }

  const auto& track = self.property_tracks[self.selection.track];
  if (track.keys.size() < 2) {
    ImGui::TextUnformatted("Needs at least two keys.");
    return;
  }

  const auto component_count = value_kind_component_count(track.kind);
  constexpr auto SAMPLE_COUNT = 128;
  auto times = std::array<f32, SAMPLE_COUNT>{};
  auto values = std::array<std::array<f32, SAMPLE_COUNT>, 4>{};

  const auto start = track.keys.front().time;
  const auto end = track.keys.back().time;
  for (auto i = 0; i < SAMPLE_COUNT; i++) {
    const auto t = start + (end - start) * (static_cast<f32>(i) / static_cast<f32>(SAMPLE_COUNT - 1));
    times[static_cast<usize>(i)] = t;
    const auto sampled = cinematic::sample_property(track.keys, track.kind, t);
    for (auto c = 0_u32; c < component_count; c++) {
      values[c][static_cast<usize>(i)] = sampled[static_cast<i32>(c)];
    }
  }

  if (ImPlot::BeginPlot("##cinematic_curve", ImVec2(-1.0f, -1.0f))) {
    const c8* labels[] = {"x", "y", "z", "w"};
    ImPlot::SetupAxes("time", "value");
    for (auto c = 0_u32; c < component_count; c++) {
      ImPlot::PlotLine(labels[c], times.data(), values[c].data(), SAMPLE_COUNT);
    }
    ImPlot::EndPlot();
  }
}

auto CinematicEditorPanel::draw_inspector(this CinematicEditorPanel& self) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  if (!self.selection.has_track()) {
    ImGui::TextUnformatted("Select a track.");
    return;
  }

  auto dirty = false;

  auto easing_labels = std::array<const c8*, static_cast<usize>(Easing::Count)>{};
  for (usize i = 0; i < easing_labels.size(); i++) {
    easing_labels[i] = stack.null_terminate_cstr(easing_name(static_cast<Easing>(i)));
  }

  if (self.selection.kind == CinematicTrackKind::Camera) {
    if (self.selection.track >= self.camera_tracks.size()) {
      return;
    }

    auto& track = self.camera_tracks[self.selection.track];

    UI::begin_properties();
    dirty |= UI::input_text("Name", &track.name);
    UI::text("Target", track.entity_path);
    auto interp = static_cast<i32>(track.interp);
    const c8* interp_labels[] = {"Linear", "Catmull-Rom"};
    if (UI::property("Interpolation", &interp, interp_labels, 2)) {
      track.interp = static_cast<CameraInterp>(interp);
      dirty = true;
    }
    dirty |= UI::property("Constant speed", &track.constant_speed);
    dirty |= UI::property("Drive FOV", &track.drive_fov);
    UI::end_properties();

    if (track.constant_speed && cinematic::track_has_cuts(track)) {
      ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "Constant speed is ignored: this track has cuts.");
    }

    if (UI::button(ICON_MDI_CAMERA_SWITCH " Add camera switch track")) {
      self.add_camera_switch_track(self.selection.track);
    }
    UI::tooltip_hover(
      "Keys this camera's CameraComponent.active, so the shot can cut to another camera entirely "
      "instead of flying one across the scene. Key it false on the outgoing camera at the same time."
    );

    if (!self.selection.has_key() || self.selection.key >= track.waypoints.size()) {
      ImGui::TextUnformatted("Select a waypoint.");
    } else {
      auto& waypoint = track.waypoints[self.selection.key];
      auto euler = glm::degrees(glm::eulerAngles(waypoint.rotation));

      ImGui::SeparatorText("Waypoint");
      // draw_vec3_control emits a table row of its own, so it has to stay inside this block
      UI::begin_properties();
      dirty |= UI::property<f32>("Time", &waypoint.time, 0.0f, self.duration, nullptr, 0.01f, "%.3f");
      dirty |= UI::draw_vec3_control("Position", waypoint.position);
      if (UI::draw_vec3_control("Rotation", euler)) {
        waypoint.rotation = glm::quat(glm::radians(euler));
        dirty = true;
      }
      dirty |= UI::property<f32>("FOV", &waypoint.fov, 1.0f, 179.0f);
      dirty |= UI::property(
        "Cut",
        &waypoint.cut,
        "Hold the previous waypoint's pose until this time, then jump here with no interpolation"
      );
      auto easing = static_cast<i32>(waypoint.easing);
      if (UI::property("Easing", &easing, easing_labels.data(), static_cast<i32>(easing_labels.size()))) {
        waypoint.easing = static_cast<Easing>(easing);
        dirty = true;
      }
      UI::end_properties();

      if (UI::button(ICON_MDI_DELETE " Delete waypoint")) {
        track.waypoints.erase(track.waypoints.begin() + static_cast<std::ptrdiff_t>(self.selection.key));
        self.selection.key = ~0_sz;
        dirty = true;
      }
    }
  } else {
    if (self.selection.track >= self.property_tracks.size()) {
      return;
    }

    auto& track = self.property_tracks[self.selection.track];

    UI::begin_properties();
    dirty |= UI::input_text("Name", &track.name);
    UI::text("Entity", track.entity_path);
    UI::text("Component", track.component_path);
    UI::text("Member", track.member_path);
    UI::text("Kind", value_kind_name(track.kind));
    UI::end_properties();

    if (UI::button(ICON_MDI_KEY_PLUS " Key at cursor")) {
      dirty |= self.key_property_track(self.selection.track, self.current_time);
    }

    if (!self.selection.has_key() || self.selection.key >= track.keys.size()) {
      ImGui::TextUnformatted("Select a key.");
    } else {
      auto& key = track.keys[self.selection.key];

      ImGui::SeparatorText("Key");
      UI::begin_properties();
      dirty |= UI::property<f32>("Time", &key.time, 0.0f, self.duration, nullptr, 0.01f, "%.3f");

      // no range metadata exists anywhere in the reflection, so these stay unbounded drags
      switch (track.kind) {
        case CinematicValueKind::Float: dirty |= UI::property<f32>("Value", &key.value.x); break;
        case CinematicValueKind::Float2:
          dirty |= UI::property_vector("Value", *reinterpret_cast<glm::vec2*>(&key.value));
          break;
        case CinematicValueKind::Float3:
          dirty |= UI::property_vector("Value", *reinterpret_cast<glm::vec3*>(&key.value));
          break;
        case CinematicValueKind::Float4: dirty |= UI::property_vector("Value", key.value); break;
        case CinematicValueKind::Quat  : {
          auto rotation = glm::quat::wxyz(key.value.w, key.value.x, key.value.y, key.value.z);
          auto euler = glm::degrees(glm::eulerAngles(rotation));
          if (UI::property_vector("Value", euler, false, true, nullptr, 0.5f, -360.0f, 360.0f)) {
            rotation = glm::quat(glm::radians(euler));
            key.value = {rotation.x, rotation.y, rotation.z, rotation.w};
            dirty = true;
          }
        } break;
        case CinematicValueKind::Bool: {
          auto flag = key.value.x != 0.0f;
          if (UI::property("Value", &flag)) {
            key.value.x = flag ? 1.0f : 0.0f;
            dirty = true;
          }
        } break;
        case CinematicValueKind::Int :
        case CinematicValueKind::Enum: {
          auto integer = static_cast<i32>(glm::round(key.value.x));
          if (UI::property<i32>("Value", &integer)) {
            key.value.x = static_cast<f32>(integer);
            dirty = true;
          }
        } break;
        case CinematicValueKind::Count: break;
      }

      auto easing = static_cast<i32>(key.easing);
      if (UI::property("Easing", &easing, easing_labels.data(), static_cast<i32>(easing_labels.size()))) {
        key.easing = static_cast<Easing>(easing);
        dirty = true;
      }
      UI::end_properties();

      if (UI::button(ICON_MDI_DELETE " Delete key")) {
        track.keys.erase(track.keys.begin() + static_cast<std::ptrdiff_t>(self.selection.key));
        self.selection.key = ~0_sz;
        dirty = true;
      }
    }
  }

  if (dirty) {
    self.sort_active_keys();
    self.commit();
  }
}

auto CinematicEditorPanel::on_render(this CinematicEditorPanel& self, vuk::ImageAttachment) -> void {
  ZoneScoped;

  if (!self.on_begin()) {
    self.on_end();
    return;
  }

  if (!self.asset_uuid) {
    ImGui::TextUnformatted("Open a cinematic from the content browser to edit it.");
    self.on_end();
    return;
  }

  self.draw_toolbar();
  ImGui::Separator();

  if (ImGui::BeginTable("cinematic_layout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
    ImGui::TableSetupColumn("tracks", ImGuiTableColumnFlags_WidthFixed, self.track_list_width);
    ImGui::TableSetupColumn("body", ImGuiTableColumnFlags_WidthStretch);

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    if (ImGui::BeginChild("##track_list", ImVec2(0.0f, 0.0f))) {
      self.draw_track_list();
    }
    ImGui::EndChild();

    ImGui::TableSetColumnIndex(1);
    if (ImGui::BeginTabBar("cinematic_tabs")) {
      if (ImGui::BeginTabItem("Timeline")) {
        self.draw_timeline_header();
        if (ImGui::BeginChild("##timeline_child", ImVec2(0.0f, ImGui::GetContentRegionAvail().y * 0.5f))) {
          self.draw_timeline();
        }
        ImGui::EndChild();
        ImGui::Separator();
        if (ImGui::BeginChild("##inspector_child")) {
          self.draw_inspector();
        }
        ImGui::EndChild();
        ImGui::EndTabItem();
      }

      if (ImGui::BeginTabItem("Curves")) {
        self.draw_curve_view();
        ImGui::EndTabItem();
      }

      ImGui::EndTabBar();
    }

    ImGui::EndTable();
  }

  self.on_end();
}
} // namespace ox
