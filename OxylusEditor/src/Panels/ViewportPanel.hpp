#pragma once

#include <atomic>
#include <expected>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <memory>
#include <string>
#include <vir/Resource.hpp>

#include "EditorPanelState.hpp"
#include "UI/RuntimeConsole.hpp"
#include "Utils/PendingPick.hpp"
#include "Utils/SceneManager.hpp"
#include "Utils/SceneTarget.hpp"
#include "Utils/VertexSnap.hpp"

namespace ox {
struct ViewCapture {
  std::atomic<bool> is_done = false;
  // written before is_done is released
  std::string png = {};
  std::string error = {};
  u32 width = 0;
  u32 height = 0;
};

struct ViewPick {
  std::atomic<bool> is_done = false;
  option<flecs::entity_t> entity = nullopt;
  std::string error = {};
};

struct ViewCameraPose {
  glm::vec3 position = {};
  glm::vec2 yaw_pitch = {};
  f32 fov = 0.0f;
  f32 near_clip = 0.0f;
  f32 far_clip = 0.0f;
  glm::vec2 viewport_size = {};
};

enum class ViewGizmoMode : u8 { None, Translate, Rotate, Scale, Universal, Bounds };

enum class ViewGizmoSpace : u8 { Local, World };

struct ViewGizmoState {
  ViewGizmoMode mode = ViewGizmoMode::None;
  ViewGizmoSpace space = ViewGizmoSpace::Local;
  bool use_snap = false;
  f32 snap_amount = 0.0f;
  f32 rotate_snap_amount = 0.0f;
};

class ViewportPanel : public EditorPanelState {
public:
  flecs::entity editor_camera = {};
  // while set, the fly-cam mirrors this entity's pose instead of taking input, so the viewport looks
  // through a scene camera without changing which camera the renderer picks
  flecs::entity piloted_camera = {};

  bool performance_overlay_visible = true;
  bool is_viewport_focused = {};
  bool is_viewport_hovered = {};
  bool is_ui_capturing_mouse = {};
  bool is_menubar_hovered = {};
  bool is_focus_requested = false;

  ViewportPanel();
  ~ViewportPanel();

  auto on_render(this ViewportPanel& self, const SwapchainInfo& swapchain_info) -> void;
  auto on_update(this ViewportPanel& self) -> void;

  auto set_context(this ViewportPanel& self, const std::shared_ptr<EditorScene>& scene) -> void;
  auto get_scene(this const ViewportPanel& self) -> EditorScene* { return self.editor_scene.get(); }

  auto drag_drop(this const ViewportPanel& self) -> void;

  // runs a runtime console command against this viewport's scene and returns the lines it logged
  auto execute_console(this ViewportPanel& self, std::string_view command) -> std::vector<std::string>;

  u64 rendered_frames = 0;

  // the PNG lands once the viewport has rendered, copied back without stalling the GPU
  auto capture(this ViewportPanel& self, u32 max_size) -> std::shared_ptr<ViewCapture>;

  // yaw and pitch in degrees
  auto camera_pose(this const ViewportPanel& self) -> option<ViewCameraPose>;
  auto set_camera_pose(
    this ViewportPanel& self,
    option<glm::vec3> position,
    option<glm::vec2> yaw_pitch,
    option<glm::vec3> look_at,
    option<f32> fov
  ) -> std::expected<void, std::string>;
  auto focus_camera_on(this ViewportPanel& self, flecs::entity entity) -> std::expected<void, std::string>;
  auto look_through(this ViewportPanel& self, option<flecs::entity> camera) -> std::expected<void, std::string>;

  // uv covers the displayed image, (0, 0) is its top left
  auto request_pick(this ViewportPanel& self, glm::vec2 uv) -> std::shared_ptr<ViewPick>;

  auto gizmo_state(this const ViewportPanel& self) -> ViewGizmoState;
  auto set_gizmo(
    this ViewportPanel& self,
    option<ViewGizmoMode> mode,
    option<ViewGizmoSpace> space,
    option<bool> use_snap,
    option<f32> snap_amount,
    option<f32> rotate_snap_amount
  ) -> ViewGizmoState;

  // what the stages recorded into the scene's program read every frame
  glm::uvec2 picking_texel = {};
  bool is_picking = false;
  u32 picking_terrain_index = ~0_u32;
  std::vector<u32> highlight_indices = {};
  u32 highlight_terrain_index = ~0_u32;
  vir::Buffer highlight_indices_buffer = {};

private:
  auto resolve_pending_pick(this ViewportPanel& self, PendingPick& pick) -> option<u32>;
  auto finish_view_pick(this ViewportPanel& self, option<u32> texel_3d, option<u32> texel_2d) -> void;
  auto record_captures(this ViewportPanel& self) -> void;
  auto poll_captures(this ViewportPanel& self) -> void;
  auto place_camera(this ViewportPanel& self, glm::vec3 position, glm::vec2 yaw_pitch) -> void;

  struct PickRequest {
    std::shared_ptr<ViewPick> pick = {};
    glm::vec2 uv = {};
  };

  struct CaptureRequest {
    std::shared_ptr<ViewCapture> capture = {};
    u32 max_size = 0;
  };

  struct CaptureReadback {
    std::vector<CaptureRequest> requests = {};
    BufferID buffer = BufferID::Invalid;
    vir::SubmitTicket ticket = {};
    vir::Format format = vir::Format::Undefined;
    u32 width = 0;
    u32 height = 0;
  };

  std::vector<PickRequest> pick_requests = {};
  bool is_view_pick_issued = false;
  std::vector<CaptureRequest> capture_requests = {};
  std::vector<CaptureReadback> capture_readbacks = {};
  // the editor camera before look_through took it over
  option<ViewCameraPose> piloted_return_pose = nullopt;

  PendingPick pending_pick_2d;
  PendingPick pending_pick_3d;
  std::weak_ptr<Scene> pending_pick_scene = {};

  std::shared_ptr<EditorScene> editor_scene = nullptr;
  bool draw_scene_stats = false;
  // Play was pressed and is held until a debugger attaches
  bool is_waiting_for_debugger = false;

  RuntimeConsole runtime_console = {};

  enum class AspectRatio : i32 {
    Auto = 0,
    _16x9,
    _16x10,
    _3x2,
    _4x3,
    _21x9,
    _32x9,
    _9x16,
  };
  AspectRatio viewport_aspect_ratio = AspectRatio::Auto;

  ImVec2 render_size = {};
  ImVec2 scaled_render_size = {};
  ImVec2 viewport_bounds_[2] = {};
  ImVec2 viewport_size = {};
  ImVec2 viewport_offset = {};
  ImVec2 viewport_position = {};
  ImVec2 gizmo_position = ImVec2(1.0f, 1.0f);
  i32 gizmo_type = -1;
  i32 gizmo_mode = 0;

  bool use_snap = true;
  f32 snap_amount = 1.f;
  f32 rotate_snap_amount = 45.f;

  VertexSnap vertex_snap = {};

  f32 volume_level = 0.f;

  bool terrain_brush_enabled = false;
  bool terrain_stroke_active = false;
  TerrainBrush terrain_brush = {};

  bool draw_component_gizmos = true;
  f32 gizmo_icon_size = 32.f;
  bool draw_entity_highlighting = true;
  bool mouse_picking_enabled = true;

  SceneTarget scene_target = {};

  // Camera
  f32 translation_dampening = 0.15f;
  f32 rotation_dampening = 0.08f;
  // Editor camera orientation state; the scene camera derives its basis from the transform rotation.
  glm::vec2 camera_yaw_pitch = glm::vec2(0.0f);
  glm::vec3 camera_position_target = glm::vec3(0.0f);
  glm::vec2 camera_yaw_pitch_target = glm::vec2(0.0f);
  glm::vec2 locked_mouse_position = glm::vec2(0.0f);
  glm::vec3 translation_velocity = glm::vec3(0);
  glm::vec2 rotation_velocity = glm::vec2(0);

  struct SettingsUI;
  ImGuiTextFilter settings_filter = {};
  bool show_advanced_settings = false;

  auto draw_settings_panel(this ViewportPanel& self) -> void;
  auto draw_render_settings(this ViewportPanel& self, SettingsUI& ui) -> void;
  auto draw_device_settings(this ViewportPanel& self, SettingsUI& ui) -> void;
  auto draw_viewport_settings(this ViewportPanel& self, SettingsUI& ui) -> void;
  auto draw_gizmo_settings_panel(this ViewportPanel& self) -> void;
  auto draw_snap_settings_panel(this ViewportPanel& self) -> void;
  auto draw_terrain_brush_settings_panel(this ViewportPanel& self) -> void;
  auto update_terrain_brush(this ViewportPanel& self, glm::vec2 viewport_uv) -> void;
  auto draw_sound_settings_panel(this ViewportPanel& self) -> void;
  auto draw_stats_overlay(this const ViewportPanel& self, bool draw_scene_stats) -> void;
  auto draw_gizmos(this ViewportPanel& self) -> void;
  auto mouse_picking_stages(this ViewportPanel& self, RendererInstance* renderer_instance, glm::uvec2 picking_texel)
    -> void;
  auto transform_gizmos_button_group(this ViewportPanel& self, ImVec2 start_cursor_pos) -> void;
  auto scene_button_group(this ViewportPanel& self, ImVec2 start_cursor_pos) -> void;
};
} // namespace ox
