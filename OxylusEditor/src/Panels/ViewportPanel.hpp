#pragma once

#include "EditorPanel.hpp"
#include "Scene/Scene.hpp"
#include "SceneHierarchyPanel.hpp"

namespace ox {
class ViewportPanel : public EditorPanel {
public:
  flecs::entity editor_camera = {};

  bool performance_overlay_visible = true;
  bool fullscreen_viewport = false;
  bool is_viewport_focused = {};
  bool is_viewport_hovered = {};

  ViewportPanel();
  ~ViewportPanel() override = default;

  void on_render(vuk::ImageAttachment swapchain_attachment) override;

  void set_context(Scene* scene, SceneHierarchyPanel& scene_hierarchy_panel);

  void on_update() override;

private:
  void draw_settings_panel();
  void draw_gizmo_settings_panel();
  void draw_stats_overlay(vuk::Extent3D extent, bool draw_scene_stats);
  void draw_gizmos();
  auto mouse_picking_stages(RendererInstance* renderer_instance, glm::uvec2 picking_texel) -> void;
  auto grid_stage(RendererInstance* renderer_instance) -> void;

  Scene* scene_ = nullptr;
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
};
} // namespace ox
