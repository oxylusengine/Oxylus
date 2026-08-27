#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "Cinematic/Cinematic.hpp"
#include "Core/UUID.hpp"
#include "EditorPanelState.hpp"
#include "Scene/Scene.hpp"

namespace ox {
class ViewportPanel;

enum class CinematicTrackKind : u8 { Camera = 0, Property };

struct CinematicSelection {
  CinematicTrackKind kind = CinematicTrackKind::Camera;
  usize track = ~0_sz;
  usize key = ~0_sz;

  auto has_track(this const CinematicSelection& self) -> bool { return self.track != ~0_sz; }
  auto has_key(this const CinematicSelection& self) -> bool { return self.has_track() && self.key != ~0_sz; }
};

// unlike the animation panel this edits the live scene rather than a private preview, so scrubbing
// shows the real shot in the viewport
class CinematicEditorPanel : public EditorPanelState {
public:
  CinematicEditorPanel();
  ~CinematicEditorPanel();

  auto on_update(this CinematicEditorPanel& self) -> void;
  auto on_render(this CinematicEditorPanel& self, vuk::ImageAttachment swapchain_attachment) -> void;

  auto open_asset(this CinematicEditorPanel& self, const UUID& uuid) -> void;

  auto get_asset_uuid(this const CinematicEditorPanel& self) -> const UUID& { return self.asset_uuid; }
  // the viewport hands the selected waypoint an ImGuizmo handle, so it needs the live pointer
  auto selected_waypoint(this CinematicEditorPanel& self) -> CameraWaypoint*;
  // only a change to the track *set* invalidates the scene instance's cached bindings; moving a
  // key inside an existing track does not
  auto commit(this CinematicEditorPanel& self, bool rebind_tracks = false) -> void;

private:
  auto active_scene(this CinematicEditorPanel& self) -> Scene*;
  // clicking a button in this panel takes focus off the viewport, so the focused one is usually
  // null by the time a toolbar action runs
  auto editor_camera_viewport(this CinematicEditorPanel& self) -> ViewportPanel*;
  // the entity whose CinematicPlayerComponent references this asset, which is what actually applies
  // the tracks to the scene
  auto resolve_player(this CinematicEditorPanel& self) -> flecs::entity;
  auto create_player(this CinematicEditorPanel& self) -> void;
  auto seek(this CinematicEditorPanel& self, f32 time) -> void;

  auto draw_toolbar(this CinematicEditorPanel& self) -> void;
  auto draw_track_list(this CinematicEditorPanel& self) -> void;
  auto draw_timeline(this CinematicEditorPanel& self) -> void;
  auto draw_inspector(this CinematicEditorPanel& self) -> void;
  auto draw_curve_view(this CinematicEditorPanel& self) -> void;
  auto draw_new_track_popup(this CinematicEditorPanel& self) -> void;
  auto draw_path_overlay(this CinematicEditorPanel& self) -> void;

  auto snapshot_waypoint(this CinematicEditorPanel& self) -> void;
  auto capture_tick(this CinematicEditorPanel& self, f32 delta_time) -> void;
  // Ramer-Douglas-Peucker on position plus a rotation-angle threshold, so a long fly-through does
  // not leave a waypoint per capture tick
  auto decimate_track(this CinematicEditorPanel& self, usize track_index) -> void;
  auto sort_active_keys(this CinematicEditorPanel& self) -> void;
  auto grow_duration_to_fit(this CinematicEditorPanel& self) -> void;

  UUID asset_uuid = {};
  std::filesystem::path asset_path = {};

  std::string cinematic_name = {};
  f32 duration = 5.0f;
  bool loop = false;
  std::vector<CinematicCameraTrack> camera_tracks = {};
  std::vector<CinematicPropertyTrack> property_tracks = {};

  f32 current_time = 0.0f;
  bool previewing = false;
  bool recording = false;
  bool piloting = false;
  bool draw_path = true;
  f32 capture_interval = 1.0f / 15.0f;
  f32 capture_accumulator = 0.0f;
  f32 decimate_tolerance = 0.05f;
  f32 decimate_angle = glm::radians(2.0f);

  CinematicSelection selection = {};
  // which key the press landed on, so a drag elsewhere on the timeline scrubs instead of
  // flinging the selected key to the cursor
  CinematicSelection dragging = {};
  // which camera track snapshots and live capture append to
  usize active_camera_track = ~0_sz;

  // held across frames because the popup opens on one frame and is clicked on a later one
  flecs::entity picker_entity = {};
  flecs::entity_t picker_component = 0;
  std::string picker_member = {};
  CinematicValueKind picker_kind = CinematicValueKind::Float;

  f32 track_list_width = 260.0f;
};
} // namespace ox
