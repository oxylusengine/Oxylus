#pragma once

#include <expected>
#include <flecs.h>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <memory>
#include <string>

#include "Core/Option.hpp"
#include "Core/Types.hpp"
#include "Panels/ViewportPanel.hpp"
#include "Utils/SceneManager.hpp"

namespace ox {
class Editor;

auto find_scene_viewport(Editor& editor, SceneID scene) -> ViewportPanel*;
// the focused viewport that shows a scene, else the first visible one
auto default_viewport(Editor& editor) -> ViewportPanel*;

auto capture_viewport(ViewportPanel& viewport, u32 max_size) -> std::shared_ptr<ViewCapture>;
auto set_camera_pose(
  ViewportPanel& viewport,
  option<glm::vec3> position,
  option<glm::vec2> yaw_pitch,
  option<glm::vec3> look_at,
  option<f32> fov
) -> std::expected<void, std::string>;
auto focus_camera_on(ViewportPanel& viewport, flecs::entity entity) -> std::expected<void, std::string>;
auto look_through(ViewportPanel& viewport, option<flecs::entity> camera) -> std::expected<void, std::string>;
auto request_pick(ViewportPanel& viewport, glm::vec2 uv) -> std::shared_ptr<ViewPick>;
auto set_gizmo(
  ViewportPanel& viewport,
  option<ViewGizmoMode> mode,
  option<ViewGizmoSpace> space,
  option<bool> use_snap,
  option<f32> snap_amount,
  option<f32> rotate_snap_amount
) -> ViewGizmoState;
} // namespace ox
