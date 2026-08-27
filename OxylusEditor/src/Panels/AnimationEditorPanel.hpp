#pragma once

#include <array>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <memory>
#include <vuk/ImageAttachment.hpp>

#include "Core/UUID.hpp"
#include "EditorPanelState.hpp"
#include "Scene/Scene.hpp"

namespace ox {
constexpr static usize STUDIO_LIGHT_COUNT = 3;

class AnimationEditorPanel : public EditorPanelState {
public:
  AnimationEditorPanel();
  ~AnimationEditorPanel();

  auto on_update(this AnimationEditorPanel& self) -> void;
  auto on_render(this AnimationEditorPanel& self, vuk::ImageAttachment swapchain_attachment) -> void;

  // takes a clip or the model it came from, and resolves whichever half is missing
  auto open_asset(this AnimationEditorPanel& self, const UUID& uuid) -> void;

private:
  auto ensure_preview_scene(this AnimationEditorPanel& self) -> void;
  auto sync_preview_model(this AnimationEditorPanel& self) -> void;
  auto set_clip(this AnimationEditorPanel& self, const UUID& uuid) -> void;
  // orbit target and distance from the model's bind-pose bounds, so a rig of any scale fits
  auto frame_model(this AnimationEditorPanel& self) -> void;
  // places the three-point rig relative to the framed bounds
  auto update_studio_lights(this AnimationEditorPanel& self) -> void;
  // bone segments through the global DebugRenderer, drained by this scene's own renderer update
  auto draw_skeleton(this AnimationEditorPanel& self) -> void;
  auto draw_preview(this AnimationEditorPanel& self, const vuk::ImageAttachment& swapchain_attachment) -> void;
  auto draw_toolbar(this AnimationEditorPanel& self) -> void;

  UUID model_uuid = {};
  UUID clip_uuid = {};

  std::unique_ptr<Scene> preview_scene = nullptr;
  flecs::entity preview_animator = {};
  flecs::entity preview_sky = {};
  // key, fill and rim, evenly spaced above the model
  std::array<flecs::entity, STUDIO_LIGHT_COUNT> preview_lights = {};
  // what the scene was actually built from, so a reopen only rebuilds on a real change
  UUID preview_model_uuid = {};

  glm::vec4 preview_background = {0.05f, 0.05f, 0.06f, 1.0f};
  glm::vec2 preview_size = {640.0f, 480.0f};
  glm::vec3 preview_target = {0.0f, 0.0f, 0.0f};
  f32 preview_radius = 1.0f;
  f32 preview_orbit_yaw = 0.6f;
  f32 preview_orbit_pitch = 0.25f;
  f32 preview_distance = 6.0f;
  f32 preview_grid_distance = 100.0f;
  f32 preview_speed = 1.0f;
  f32 preview_light_intensity = 1.0f;
  bool preview_playing = true;
  bool preview_grid_enabled = true;
  bool preview_skeleton_enabled = false;
};
} // namespace ox
