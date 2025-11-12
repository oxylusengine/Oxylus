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

  std::string last_save_scene_path = {};

  ViewportPanel();
  ~ViewportPanel() override = default;

  auto on_render(vuk::ImageAttachment swapchain_attachment) -> void override;

  auto set_context(const std::shared_ptr<EditorScene>& scene, SceneHierarchyPanel* scene_hierarchy_panel) -> void;
  auto get_scene() const -> EditorScene* { return editor_scene_.get(); }

  auto on_update() -> void override;

private:
  std::shared_ptr<EditorScene> editor_scene_ = nullptr;
  SceneHierarchyPanel* scene_hierarchy_panel_ = nullptr;
  bool draw_scene_stats = false;

  glm::vec2 viewport_size_ = {};
  glm::vec2 viewport_bounds_[2] = {};
  glm::vec2 viewport_panel_size_ = {};
  glm::vec2 viewport_position_ = {};
  glm::vec2 viewport_offset_ = {};
  glm::vec2 gizmo_position_ = glm::vec2(1.0f, 1.0f);
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
  void drag_drop_with_button();
  void transform_gizmos_button_group(ImVec2 start_cursor_pos);
  void scene_button_group(ImVec2 start_cursor_pos);
};
} // namespace ox
