#pragma once

#include <memory>

#include "Asset/ParticleSystem.hpp"
#include "EditorPanelState.hpp"
#include "Scene/Scene.hpp"

namespace ax::NodeEditor {
struct EditorContext;
}

namespace ox {
class ParticleEditorPanel : public EditorPanelState {
public:
  ParticleEditorPanel();
  ~ParticleEditorPanel();

  auto on_update(this ParticleEditorPanel& self) -> void;
  auto on_render(this ParticleEditorPanel& self, vuk::ImageAttachment swapchain_attachment) -> void;

  // Loads the asset into the panel and shows it. The panel keeps a working copy; edits are pushed
  // back through AssetManager::edit_particle_system so every holder recompiles at once.
  auto open_asset(this ParticleEditorPanel& self, const UUID& uuid) -> void;

private:
  auto active_graph(this ParticleEditorPanel& self) -> ParticleGraph&;
  auto commit(this ParticleEditorPanel& self, bool recompile = true) -> void;
  auto draw_canvas(this ParticleEditorPanel& self) -> void;
  auto draw_inspector(this ParticleEditorPanel& self) -> void;
  // Plots one curve and lets its control points be dragged. Returns true when the curve changed.
  auto draw_curve_editor(this ParticleEditorPanel& self, usize curve_index, ParticleCurve& curve) -> bool;
  auto draw_preview(this ParticleEditorPanel& self, const vuk::ImageAttachment& swapchain_attachment) -> void;
  auto ensure_preview_scene(this ParticleEditorPanel& self) -> void;
  auto sync_preview_asset(this ParticleEditorPanel& self) -> void;

  UUID asset_uuid = {};
  std::filesystem::path asset_path = {};

  ParticleEmitterSettings emitter_settings = {};
  ParticleRenderSettings render_settings = {};
  ParticleGraph spawn_graph = {};
  ParticleGraph update_graph = {};
  std::vector<ParticleCurve> curves = {};
  std::vector<ParticleGradient> gradients = {};
  std::string compile_error = {};

  ParticleProgramKind active_kind = ParticleProgramKind::Spawn;
  ParticleNodeID selected_node = ParticleNodeID::Invalid;
  // Held across frames: the popup opens on one frame and is clicked on a later one.
  ParticleNodeID context_node = ParticleNodeID::Invalid;
  ParticleLinkID context_link = ParticleLinkID::Invalid;
  glm::vec2 context_screen_position = {};
  u64 context_pin = 0;
  usize dragged_curve = ~0_sz;

  f32 inspector_width = 360.0f;
  f32 preview_width = 380.0f;

  ax::NodeEditor::EditorContext* spawn_context = nullptr;
  ax::NodeEditor::EditorContext* update_context = nullptr;
  bool spawn_positions_applied = false;
  bool update_positions_applied = false;

  std::unique_ptr<Scene> preview_scene = nullptr;
  flecs::entity preview_emitter = {};
  flecs::entity preview_sky = {};
  glm::vec4 preview_background = {0.05f, 0.05f, 0.06f, 1.0f};
  UUID preview_asset = {};
  glm::vec2 preview_size = {360.0f, 420.0f};
  f32 preview_orbit_yaw = 0.0f;
  f32 preview_orbit_pitch = 0.2f;
  f32 preview_distance = 6.0f;
  f32 preview_speed = 1.0f;
  bool preview_playing = true;
};
} // namespace ox
