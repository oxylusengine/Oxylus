#pragma once

#include "EditorPanel.hpp"
#include "Scene/Scene.hpp"
#include "SceneHierarchyPanel.hpp"
#include "Utils/SceneManager.hpp"

namespace ox {
class ViewportPanel : public EditorPanel {
public:
  flecs::entity editor_camera = {};

  bool performance_overlay_visible = true;
  bool fullscreen_viewport = false;
  bool is_viewport_focused = {};
  bool is_viewport_hovered = {};

  ViewportPanel();
  ~ViewportPanel() override;

  auto on_render(vuk::ImageAttachment swapchain_attachment) -> void override;
  auto on_update() -> void override;

  auto set_context(this ViewportPanel& self, const std::shared_ptr<EditorScene>& scene) -> void;
  auto get_scene(this const ViewportPanel& self) -> EditorScene* { return self.editor_scene_.get(); }

  void drag_drop(this const ViewportPanel& self);

private:
  std::shared_ptr<EditorScene> editor_scene_ = nullptr;
  bool draw_scene_stats = false;

  ImVec2 viewport_bounds_[2] = {};
  ImVec2 viewport_size = {};
  ImVec2 viewport_position_ = {};
  ImVec2 gizmo_position_ = ImVec2(1.0f, 1.0f);
  i32 gizmo_type_ = -1;
  i32 gizmo_mode_ = 0;

  bool draw_component_gizmos_ = true;
  f32 gizmo_icon_size_ = 32.f;
  bool draw_entity_highlighting_ = true;
  bool mouse_picking_enabled_ = true;

  std::vector<vuk::Unique<vuk::Buffer>> id_buffers = {};

  // Camera
  f32 _translation_dampening = 0.3f;
  f32 _rotation_dampening = 0.3f;
  glm::vec2 _locked_mouse_position = glm::vec2(0.0f);
  glm::vec3 _translation_velocity = glm::vec3(0);
  glm::vec2 _rotation_velocity = glm::vec2(0);

  void draw_settings_panel();
  void draw_gizmo_settings_panel();
  void draw_stats_overlay(bool draw_scene_stats) const;
  void draw_gizmos();
  auto mouse_picking_stages(RendererInstance* renderer_instance, glm::uvec2 picking_texel) -> void;
  auto grid_stage(RendererInstance* renderer_instance) -> void;
  void transform_gizmos_button_group(ImVec2 start_cursor_pos);
  void scene_button_group(ImVec2 start_cursor_pos);
};
} // namespace ox
