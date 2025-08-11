#pragma once

#include <vuk/runtime/vk/PipelineInstance.hpp>
#include <vuk/runtime/vk/Query.hpp>

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

  void on_render(vuk::Extent3D extent, vuk::Format format) override;

  void set_context(const std::shared_ptr<Scene>& scene, SceneHierarchyPanel& scene_hierarchy_panel);

  void on_update() override;

private:
  void draw_settings_panel();
  void draw_performance_overlay();
  void draw_gizmos();

  std::shared_ptr<Scene> _scene = {};
  SceneHierarchyPanel* _scene_hierarchy_panel = nullptr;

  glm::vec2 _viewport_size = {};
  glm::vec2 _viewport_bounds[2] = {};
  glm::vec2 _viewport_panel_size = {};
  glm::vec2 _viewport_position = {};
  glm::vec2 _viewport_offset = {};
  glm::vec2 _gizmo_position = glm::vec2(1.0f, 1.0f);
  int _gizmo_type = -1;
  int _gizmo_mode = 0;

  std::vector<vuk::Unique<vuk::Buffer>> id_buffers = {};

  // Camera
  float _translation_dampening = 0.6f;
  float _rotation_dampening = 0.3f;
  glm::vec2 _locked_mouse_position = glm::vec2(0.0f);
  glm::vec3 _translation_velocity = glm::vec3(0);
  glm::vec2 _rotation_velocity = glm::vec2(0);
};
} // namespace ox
