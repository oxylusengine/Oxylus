#include "Actions/ViewActions.hpp"

#include <tracy/Tracy.hpp>

#include "Editor.hpp"

namespace ox {
auto find_scene_viewport(Editor& editor, SceneID scene) -> ViewportPanel* {
  ZoneScoped;

  ViewportPanel* found = nullptr;
  editor.scene_manager.for_each_scene([&](SceneID id, EditorScene& editor_scene) {
    if (id == scene && found == nullptr) {
      found = editor.main_viewport_panel.find_viewport(&editor_scene);
    }
  });

  return found;
}

auto default_viewport(Editor& editor) -> ViewportPanel* {
  ZoneScoped;

  auto* focused = editor.main_viewport_panel.get_focused_viewport();
  if (focused != nullptr && focused->get_scene() != nullptr) {
    return focused;
  }

  for (auto* viewport : editor.main_viewport_panel.get_visible_viwports()) {
    if (viewport->get_scene() != nullptr) {
      return viewport;
    }
  }

  return nullptr;
}

auto capture_viewport(ViewportPanel& viewport, u32 max_size) -> std::shared_ptr<ViewCapture> {
  return viewport.capture(max_size);
}

auto set_camera_pose(
  ViewportPanel& viewport,
  option<glm::vec3> position,
  option<glm::vec2> yaw_pitch,
  option<glm::vec3> look_at,
  option<f32> fov
) -> std::expected<void, std::string> {
  return viewport.set_camera_pose(position, yaw_pitch, look_at, fov);
}

auto focus_camera_on(ViewportPanel& viewport, flecs::entity entity) -> std::expected<void, std::string> {
  return viewport.focus_camera_on(entity);
}

auto look_through(ViewportPanel& viewport, option<flecs::entity> camera) -> std::expected<void, std::string> {
  return viewport.look_through(camera);
}

auto request_pick(ViewportPanel& viewport, glm::vec2 uv) -> std::shared_ptr<ViewPick> {
  return viewport.request_pick(uv);
}

auto set_gizmo(
  ViewportPanel& viewport,
  option<ViewGizmoMode> mode,
  option<ViewGizmoSpace> space,
  option<bool> use_snap,
  option<f32> snap_amount,
  option<f32> rotate_snap_amount
) -> ViewGizmoState {
  return viewport.set_gizmo(mode, space, use_snap, snap_amount, rotate_snap_amount);
}
} // namespace ox
