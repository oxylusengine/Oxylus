#pragma once

#include "EditorPanelState.hpp"
#include "Scene/Scene.hpp"
#include "SceneHierarchyPanel.hpp"
#include "Utils/SceneManager.hpp"

namespace ox {
class ViewportPanel : public EditorPanelState {
public:
  flecs::entity editor_camera = {};

  bool performance_overlay_visible = true;
  bool is_viewport_focused = {};
  bool is_viewport_hovered = {};

  ViewportPanel();
  ~ViewportPanel();

  auto on_render(this ViewportPanel& self, vuk::ImageAttachment swapchain_attachment) -> void;
  auto on_update(this ViewportPanel& self) -> void;

  auto set_context(this ViewportPanel& self, const std::shared_ptr<EditorScene>& scene) -> void;
  auto get_scene(this const ViewportPanel& self) -> EditorScene* { return self.editor_scene.get(); }

  void drag_drop(this const ViewportPanel& self);

private:
  std::shared_ptr<EditorScene> editor_scene = nullptr;
  bool draw_scene_stats = false;

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
  ImVec2 viewport_bounds_[2] = {};
  ImVec2 viewport_size = {};
  ImVec2 viewport_offset = {};
  ImVec2 viewport_position = {};
  ImVec2 gizmo_position = ImVec2(1.0f, 1.0f);
  i32 gizmo_type = -1;
  i32 gizmo_mode = 0;

  bool draw_component_gizmos = true;
  f32 gizmo_icon_size = 32.f;
  bool draw_entity_highlighting = true;
  bool mouse_picking_enabled = true;

  std::vector<vuk::Unique<vuk::Buffer>> id_buffers = {};

  // Camera
  f32 translation_dampening = 0.3f;
  f32 rotation_dampening = 0.3f;
  glm::vec2 locked_mouse_position = glm::vec2(0.0f);
  glm::vec3 translation_velocity = glm::vec3(0);
  glm::vec2 rotation_velocity = glm::vec2(0);

  void draw_settings_panel(this ViewportPanel& self);
  void draw_gizmo_settings_panel(this ViewportPanel& self);
  void draw_stats_overlay(this const ViewportPanel& self, bool draw_scene_stats);
  void draw_gizmos(this ViewportPanel& self);
  auto mouse_picking_stages(this ViewportPanel& self, RendererInstance* renderer_instance, glm::uvec2 picking_texel) -> void;
  auto grid_stage(this ViewportPanel& self, RendererInstance* renderer_instance) -> void;
  void transform_gizmos_button_group(this ViewportPanel& self, ImVec2 start_cursor_pos);
  void scene_button_group(this ViewportPanel& self, ImVec2 start_cursor_pos);
};
} // namespace ox
