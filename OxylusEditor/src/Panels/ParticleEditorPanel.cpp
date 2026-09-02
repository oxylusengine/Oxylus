#include "ParticleEditorPanel.hpp"

#include <cmath>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <icons/IconsMaterialDesignIcons.h>
#include <imgui-node-editor/imgui_node_editor.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>
#include <vuk/vsl/Core.hpp>

#include "Asset/AssetImporter.hpp"
#include "Asset/AssetManager.hpp"
#include "Asset/AssetMeta.hpp"
#include "Core/App.hpp"
#include "Core/Input.hpp"
#include "Memory/Stack.hpp"
#include "Scene/Components.hpp"
#include "UI/PayloadData.hpp"
#include "UI/UI.hpp"
#include "Utils/EditorGrid.hpp"
#include "Utils/Log.hpp"

namespace ed = ax::NodeEditor;

namespace ox {
constexpr static u64 PARTICLE_PIN_ID_BASE = 0x0001'0000_u64;
constexpr static u64 PARTICLE_LINK_ID_BASE = 0x0100'0000_u64;
constexpr static u64 PARTICLE_PINS_PER_NODE = 64;

constexpr static f32 PARTICLE_LITERAL_RANGE = 1.0e6f;
constexpr static f32 PARTICLE_PREVIEW_GRID_DISTANCE = 200.0f;

constexpr static u32 PARTICLE_PREVIEW_RING_SEGMENTS = 48;

constexpr static ParticleProgramKind PARTICLE_PROGRAM_KINDS[] = {
  ParticleProgramKind::Emitter,
  ParticleProgramKind::Spawn,
  ParticleProgramKind::Update,
};

// Projects emitter-local line segments onto the preview image. The shapes mirror
// particle_emission_point in particles.slang, so what is drawn is what the emit shader samples.
struct ParticleShapeGizmo {
  glm::mat4 clip_from_local = glm::mat4(1.0f);
  ImDrawList* draw_list = nullptr;
  ImVec2 origin = {};
  ImVec2 size = {};
  u32 color = 0;

  auto to_screen(this const ParticleShapeGizmo& self, const glm::vec4& clip) -> ImVec2 {
    const auto ndc = glm::vec2(clip) / clip.w;
    return ImVec2(
      self.origin.x + (ndc.x * 0.5f + 0.5f) * self.size.x,
      self.origin.y + (ndc.y * 0.5f + 0.5f) * self.size.y
    );
  }

  auto line(this const ParticleShapeGizmo& self, const glm::vec3& from, const glm::vec3& to) -> void {
    constexpr auto near_w = 1.0e-4f;

    auto a = self.clip_from_local * glm::vec4(from, 1.0f);
    auto b = self.clip_from_local * glm::vec4(to, 1.0f);

    // both endpoints behind the eye: nothing to draw. one behind: pull it up to the near plane so
    // the segment does not wrap to the far side of the screen
    if (a.w < near_w && b.w < near_w) {
      return;
    }
    if (a.w < near_w) {
      a = glm::mix(a, b, (near_w - a.w) / (b.w - a.w));
    } else if (b.w < near_w) {
      b = glm::mix(b, a, (near_w - b.w) / (a.w - b.w));
    }

    self.draw_list->AddLine(self.to_screen(a), self.to_screen(b), self.color, UI::scale(1.0f));
  }

  // Traces center + u * cos(t) + v * sin(t) over [begin, end] radians.
  auto arc(
    this const ParticleShapeGizmo& self,
    const glm::vec3& center,
    const glm::vec3& u,
    const glm::vec3& v,
    const u32 segments,
    const f32 begin,
    const f32 end
  ) -> void {
    auto previous = center + u * std::cos(begin) + v * std::sin(begin);
    for (auto i = 1_u32; i <= segments; i++) {
      const auto t = begin + (end - begin) * (static_cast<f32>(i) / static_cast<f32>(segments));
      const auto current = center + u * std::cos(t) + v * std::sin(t);
      self.line(previous, current);
      previous = current;
    }
  }

  auto ring(this const ParticleShapeGizmo& self, const glm::vec3& center, const glm::vec3& u, const glm::vec3& v)
    -> void {
    self.arc(center, u, v, PARTICLE_PREVIEW_RING_SEGMENTS, 0.0f, glm::two_pi<f32>());
  }
};

auto draw_particle_shape(
  const ParticleShapeGizmo& gizmo, const ParticleEmissionShape shape, const glm::vec3& shape_size, const f32 shape_angle
) -> void {
  const auto x = glm::vec3(shape_size.x, 0.0f, 0.0f);
  const auto y = glm::vec3(0.0f, shape_size.y, 0.0f);
  const auto z = glm::vec3(0.0f, 0.0f, shape_size.z);

  switch (shape) {
    case ParticleEmissionShape::Point: {
      // no extent to outline, so just mark the origin
      constexpr auto tick = 0.15f;
      gizmo.line({-tick, 0.0f, 0.0f}, {tick, 0.0f, 0.0f});
      gizmo.line({0.0f, -tick, 0.0f}, {0.0f, tick, 0.0f});
      gizmo.line({0.0f, 0.0f, -tick}, {0.0f, 0.0f, tick});
    } break;
    case ParticleEmissionShape::Sphere: {
      gizmo.ring({}, x, y);
      gizmo.ring({}, x, z);
      gizmo.ring({}, y, z);
    } break;
    case ParticleEmissionShape::Hemisphere: {
      gizmo.ring({}, x, z);
      gizmo.arc({}, x, y, PARTICLE_PREVIEW_RING_SEGMENTS / 2, 0.0f, glm::pi<f32>());
      gizmo.arc({}, z, y, PARTICLE_PREVIEW_RING_SEGMENTS / 2, 0.0f, glm::pi<f32>());
    } break;
    case ParticleEmissionShape::Box: {
      for (auto corner = 0_u32; corner < 8; corner++) {
        const auto from = glm::vec3(
          (corner & 1u) ? shape_size.x : -shape_size.x,
          (corner & 2u) ? shape_size.y : -shape_size.y,
          (corner & 4u) ? shape_size.z : -shape_size.z
        );

        // one edge per axis, only towards the higher corner, so each of the 12 is drawn once
        for (auto axis = 0_u32; axis < 3; axis++) {
          const auto bit = 1u << axis;
          if (corner & bit) {
            continue;
          }

          auto to = from;
          to[static_cast<i32>(axis)] = -to[static_cast<i32>(axis)];
          gizmo.line(from, to);
        }
      }
    } break;
    case ParticleEmissionShape::Circle: {
      gizmo.ring({}, x, z);
    } break;
    case ParticleEmissionShape::Cone: {
      const auto spread = std::tan(glm::radians(std::clamp(shape_angle, 0.0f, 89.0f)));
      const auto top_radius = shape_size.x + shape_size.y * spread;
      const auto top_center = glm::vec3(0.0f, shape_size.y, 0.0f);

      gizmo.ring({}, {shape_size.x, 0.0f, 0.0f}, {0.0f, 0.0f, shape_size.x});
      gizmo.ring(top_center, {top_radius, 0.0f, 0.0f}, {0.0f, 0.0f, top_radius});

      for (auto i = 0_u32; i < 4; i++) {
        const auto angle = glm::half_pi<f32>() * static_cast<f32>(i);
        const auto direction = glm::vec3(std::cos(angle), 0.0f, std::sin(angle));
        gizmo.line(direction * shape_size.x, top_center + direction * top_radius);
      }
    } break;
    default: break;
  }
}

auto particle_node_editor_id(const ParticleNodeID id) -> u64 { return std::to_underlying(id) + 1; }

auto particle_input_pin_id(const ParticleNodeID id, const u32 pin) -> u64 {
  return PARTICLE_PIN_ID_BASE + particle_node_editor_id(id) * PARTICLE_PINS_PER_NODE + pin;
}

auto particle_output_pin_id(const ParticleNodeID id) -> u64 {
  return PARTICLE_PIN_ID_BASE + particle_node_editor_id(id) * PARTICLE_PINS_PER_NODE + PARTICLE_PINS_PER_NODE - 1;
}

auto particle_pin_owner(const u64 pin_id) -> ParticleNodeID {
  if (pin_id < PARTICLE_PIN_ID_BASE) {
    return ParticleNodeID::Invalid;
  }

  return static_cast<ParticleNodeID>((pin_id - PARTICLE_PIN_ID_BASE) / PARTICLE_PINS_PER_NODE - 1);
}

auto particle_pin_slot(const u64 pin_id) -> u32 {
  return static_cast<u32>((pin_id - PARTICLE_PIN_ID_BASE) % PARTICLE_PINS_PER_NODE);
}

auto remap_particle_sampler_indices(ParticleGraph& graph, const ParticleNodeType type, const u32 removed) -> void {
  for (auto& node : graph.nodes) {
    if (node.type != type) {
      continue;
    }

    if (node.index > removed) {
      node.index -= 1;
    } else if (node.index == removed) {
      node.index = 0;
    }
  }
}

auto particle_node_header_color(const std::string_view category) -> ImVec4 {
  if (category == "Attribute") {
    return {0.44f, 0.32f, 0.70f, 1.0f};
  }
  if (category == "Input") {
    return {0.14f, 0.52f, 0.58f, 1.0f};
  }
  if (category == "Vector") {
    return {0.76f, 0.45f, 0.18f, 1.0f};
  }
  if (category == "Output") {
    return {0.70f, 0.26f, 0.32f, 1.0f};
  }

  return {0.20f, 0.42f, 0.72f, 1.0f};
}

auto draw_particle_node_header(
  ImDrawList& draw_list, const ImVec2& min, const ImVec2& max, const ImVec4& color, const f32 rounding
) -> void {
  constexpr auto tail_alpha = 0.05f;

  const auto first_vertex = draw_list.VtxBuffer.Size;
  draw_list.AddRectFilled(min, max, IM_COL32_WHITE, rounding, ImDrawFlags_RoundCornersTop);
  const auto last_vertex = draw_list.VtxBuffer.Size;

  const auto width = std::max(max.x - min.x, 1.0f);
  for (auto i = first_vertex; i < last_vertex; i++) {
    auto& vertex = draw_list.VtxBuffer[i];
    const auto t = std::clamp((vertex.pos.x - min.x) / width, 0.0f, 1.0f);
    vertex.col = ImGui::ColorConvertFloat4ToU32({color.x, color.y, color.z, color.w + (tail_alpha - color.w) * t});
  }
}

auto particle_pin_is_output(const u64 pin_id) -> bool {
  return particle_pin_slot(pin_id) == PARTICLE_PINS_PER_NODE - 1;
}

auto particle_kind_index(const ParticleProgramKind kind) -> usize {
  return static_cast<usize>(std::to_underlying(kind));
}

// the id after ### is what imgui stores the dock position under, so it has to stay put
auto particle_kind_window_id(const ParticleProgramKind kind) -> const c8* {
  switch (kind) {
    case ParticleProgramKind::Emitter: return ICON_MDI_FOUNTAIN " Emitter###particle_graph_emitter";
    case ParticleProgramKind::Update : return ICON_MDI_AUTORENEW " Update###particle_graph_update";
    default                          : return ICON_MDI_CREATION " Spawn###particle_graph_spawn";
  }
}

auto particle_kind_summary(const ParticleProgramKind kind) -> const c8* {
  switch (kind) {
    case ParticleProgramKind::Emitter: return "Decides how many particles to spawn this frame.";
    case ParticleProgramKind::Update : return "Moves and ages every particle that is alive.";
    default                          : return "Gives a new particle its starting look and motion.";
  }
}

auto particle_kind_description(const ParticleProgramKind kind) -> const c8* {
  switch (kind) {
    case ParticleProgramKind::Emitter:
      return "Runs once per emitter per frame and decides how many particles are born. This is where "
             "bursts, intervals and rate changes live.";
    case ParticleProgramKind::Update: return "Runs once per living particle, every frame.";
    default                         : return "Runs once per particle, at birth. Sets the starting attributes.";
  }
}

ParticleEditorPanel::ParticleEditorPanel() : EditorPanelState("Particle Editor", ICON_MDI_SHIMMER, false) {
  this->window_default_size = {1280, 720};

  auto config = ed::Config{};
  config.SettingsFile = nullptr;
  for (auto& context : graph_contexts) {
    context = ed::CreateEditor(&config);
  }
}

ParticleEditorPanel::~ParticleEditorPanel() {
  for (auto* context : graph_contexts) {
    if (context) {
      ed::DestroyEditor(context);
    }
  }

  // the preview scene releases the ref its emitter component holds
  if (preview_scene && preview_asset) {
    preview_emitter = {};
    preview_scene.reset();
  }

  if (asset_uuid) {
    App::mod<AssetManager>().unload_asset(asset_uuid);
  }
}

auto ParticleEditorPanel::sync_node_editor_scale(this ParticleEditorPanel& self) -> void {
  const auto ui_scale = App::get_ui_scale();
  if (std::abs(self.applied_node_editor_scale - ui_scale) <= 0.0001f) {
    return;
  }

  const auto previous_ui_scale = self.applied_node_editor_scale > 0.0f ? self.applied_node_editor_scale : 1.0f;
  const auto scale_ratio = ui_scale / previous_ui_scale;
  self.inspector_width *= scale_ratio;
  self.preview_width *= scale_ratio;

  auto* previous_context = ed::GetCurrentEditor();
  for (auto* context : self.graph_contexts) {
    ed::SetCurrentEditor(context);
    auto style = ed::Style{};
    style.NodePadding.x *= ui_scale;
    style.NodePadding.y *= ui_scale;
    style.NodePadding.z *= ui_scale;
    style.NodePadding.w *= ui_scale;
    style.NodeRounding *= ui_scale;
    style.NodeBorderWidth *= ui_scale;
    style.HoveredNodeBorderWidth *= ui_scale;
    style.HoverNodeBorderOffset *= ui_scale;
    style.SelectedNodeBorderWidth *= ui_scale;
    style.SelectedNodeBorderOffset *= ui_scale;
    style.PinRounding *= ui_scale;
    style.PinBorderWidth *= ui_scale;
    style.LinkStrength *= ui_scale;
    style.FlowMarkerDistance *= ui_scale;
    style.FlowSpeed *= ui_scale;
    style.PivotSize.x *= ui_scale;
    style.PivotSize.y *= ui_scale;
    style.PinRadius *= ui_scale;
    style.PinArrowSize *= ui_scale;
    style.PinArrowWidth *= ui_scale;
    style.GroupRounding *= ui_scale;
    style.GroupBorderWidth *= ui_scale;
    ed::GetStyle() = style;
  }
  ed::SetCurrentEditor(previous_context);

  self.applied_node_editor_scale = ui_scale;
}

auto ParticleEditorPanel::active_graph(this ParticleEditorPanel& self) -> ParticleGraph& {
  return self.graph_for(self.active_kind);
}

auto ParticleEditorPanel::graph_for(this ParticleEditorPanel& self, const ParticleProgramKind kind) -> ParticleGraph& {
  switch (kind) {
    case ParticleProgramKind::Emitter: return self.emitter_graph;
    case ParticleProgramKind::Update : return self.update_graph;
    default                          : return self.spawn_graph;
  }
}

auto ParticleEditorPanel::open_asset(this ParticleEditorPanel& self, const UUID& uuid) -> void {
  ZoneScoped;

  auto& asset_man = App::mod<AssetManager>();

  // the panel holds its own ref for as long as it shows the asset, so opening one from the content
  // browser does not depend on an entity keeping it alive
  if (!asset_man.load_asset(uuid)) {
    OX_LOG_ERROR("Couldn't load particle system {}.", uuid.str());
    return;
  }

  {
    auto system = asset_man.get_particle_system(uuid);
    if (!system) {
      OX_LOG_ERROR("Particle system {} is not loaded.", uuid.str());
      asset_man.unload_asset(uuid);
      return;
    }

    self.emitter_settings = system->emitter;
    self.render_settings = system->render;
    self.emitter_graph = system->emitter_graph;
    self.spawn_graph = system->spawn_graph;
    self.update_graph = system->update_graph;
    self.curves = system->curves;
    self.gradients = system->gradients;
    self.parameters = system->parameters;
    self.compile_error = system->compile_error;
  }

  if (auto asset = asset_man.get_asset(uuid)) {
    self.asset_path = asset->path;
  }

  const auto previous_asset = self.asset_uuid;
  self.asset_uuid = uuid;
  if (previous_asset) {
    asset_man.unload_asset(previous_asset);
  }

  self.selected_node = ParticleNodeID::Invalid;
  self.graph_positions_applied = {};
  self.visible = true;

  self.sync_preview_asset();
}

auto ParticleEditorPanel::commit(this ParticleEditorPanel& self, const bool recompile) -> void {
  ZoneScoped;

  if (!self.asset_uuid) {
    return;
  }

  auto& asset_man = App::mod<AssetManager>();
  asset_man.edit_particle_system(
    self.asset_uuid,
    [&self](ParticleSystem& system) {
      system.emitter = self.emitter_settings;
      system.render = self.render_settings;
      system.emitter_graph = self.emitter_graph;
      system.spawn_graph = self.spawn_graph;
      system.update_graph = self.update_graph;
      system.curves = self.curves;
      system.gradients = self.gradients;
      system.parameters = self.parameters;
    },
    recompile
  );

  if (!recompile) {
    return;
  }

  if (auto system = asset_man.get_particle_system(self.asset_uuid)) {
    self.compile_error = system->compile_error;
  }
}

auto ParticleEditorPanel::ensure_preview_scene(this ParticleEditorPanel& self) -> void {
  ZoneScoped;

  if (self.preview_scene) {
    return;
  }

  self.preview_scene = std::make_unique<Scene>("ParticlePreviewScene");
  auto& scene = *self.preview_scene;

  self.preview_emitter = scene.create_entity("preview_emitter", true);
  self.preview_emitter.set<TransformComponent>({});

  const auto sun_direction = glm::normalize(glm::vec3(-0.45f, -0.78f, -0.44f));
  self.preview_sky = scene.create_entity("preview_sun", true);
  self.preview_sky.set<TransformComponent>({.rotation = glm::quatLookAt(sun_direction, glm::vec3(0.0f, 1.0f, 0.0f))})
    .set<LightComponent>({
      .type = LightComponent::LightType::Directional,
      .intensity = 5.0f,
      .cast_shadows = false,
    })
    .set<SkyComponent>({
      .solid_color = self.preview_background,
      .ambient_color = glm::vec3(0.05f),
      .texture = UUID(nullptr),
    });

  scene.create_entity("preview_camera", true)
    .set<CameraComponent>({.fov = 60.0f, .far_clip = 200.0f, .near_clip = 0.05f})
    .set<TransformComponent>({});

  scene.renderer_cvar.cvar_enable_debug_renderer.set(false);
  scene.renderer_cvar.cvar_draw_bounding_boxes.set(false);
  scene.renderer_cvar.cvar_bloom_enable.set(false);
  scene.renderer_cvar.cvar_vbgtao_enable.set(false);
  scene.renderer_cvar.cvar_contact_shadows_enabled.set(false);
  scene.renderer_cvar.cvar_ddgi_enable.set(false);
}

auto ParticleEditorPanel::sync_preview_asset(this ParticleEditorPanel& self) -> void {
  ZoneScoped;

  self.ensure_preview_scene();

  if (self.preview_asset == self.asset_uuid) {
    return;
  }

  auto& asset_man = App::mod<AssetManager>();
  const auto previous = self.preview_asset;
  if (self.asset_uuid) {
    asset_man.load_asset(self.asset_uuid);
  }

  self.preview_asset = self.asset_uuid;
  self.preview_emitter.set<ParticleSystemComponent>({
    .particle_system = self.asset_uuid,
    .play_on_awake = true,
  });

  // the scene releases this again when the component goes away, so only the swap is ours to undo
  if (previous) {
    asset_man.unload_asset(previous);
  }
}

auto ParticleEditorPanel::on_update(this ParticleEditorPanel& self) -> void {
  ZoneScoped;

  // The preview scene ticks in `draw_preview`, immediately before its render.
}

auto ParticleEditorPanel::draw_canvas(this ParticleEditorPanel& self, const ParticleProgramKind kind) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  const auto index = particle_kind_index(kind);
  auto* context = self.graph_contexts[index];
  auto* positions_applied = &self.graph_positions_applied[index];
  auto& graph = self.graph_for(kind);

  // several graphs can be on screen at once, so the inspector follows whichever one has focus
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
    self.active_kind = kind;
  }

  ed::SetCurrentEditor(context);
  ed::Begin("particle_graph_canvas");

  for (const auto& node : graph.nodes) {
    const auto& desc = particle_node_desc(node.type);
    const auto node_id = ed::NodeId(particle_node_editor_id(node.id));

    if (!*positions_applied) {
      ed::SetNodePosition(node_id, ImVec2(node.canvas_position.x, node.canvas_position.y));
    }

    ed::BeginNode(node_id);
    ImGui::TextUnformatted(desc.name.data(), desc.name.data() + desc.name.size());
    const auto header_bottom = ImGui::GetItemRectMax().y;
    ImGui::Dummy(UI::scale(ImVec2(140.0f, 2.0f)));

    for (auto pin = 0_u32; pin < desc.input_count; pin++) {
      ed::BeginPin(ed::PinId(particle_input_pin_id(node.id, pin)), ed::PinKind::Input);
      ImGui::TextUnformatted(stack.format_char("{} in {}", ICON_MDI_CIRCLE_SMALL, pin));
      ed::EndPin();
    }

    if (desc.has_output) {
      ImGui::Indent(UI::scale(90.0f));
      ed::BeginPin(ed::PinId(particle_output_pin_id(node.id)), ed::PinKind::Output);
      ImGui::TextUnformatted(stack.format_char("out {}", ICON_MDI_CIRCLE_SMALL));
      ed::EndPin();
      ImGui::Unindent(UI::scale(90.0f));
    }

    ed::EndNode();

    // Drawn after the node so the band can span its final width, and into the background list so it
    // sits under the title.
    if (ImGui::IsItemVisible()) {
      const auto node_min = ImGui::GetItemRectMin();
      const auto node_max = ImGui::GetItemRectMax();
      const auto& style = ed::GetStyle();
      // the node border is stroked half a pixel inside the bounds and centered on that path, so its
      // inner edge is here. the header sits on a channel above the border and would paint over it
      const auto inset = style.NodeBorderWidth * 0.5f + 0.5f;
      // same arc centers as the border, one inset smaller, so the corners nest instead of bulging out
      const auto rounding = std::max(style.NodeRounding - inset, 0.0f);

      if (auto* background = ed::GetNodeBackgroundDrawList(node_id)) {
        draw_particle_node_header(
          *background,
          {node_min.x + inset, node_min.y + inset},
          {node_max.x - inset, header_bottom + style.NodePadding.y * 0.5f},
          particle_node_header_color(desc.category),
          rounding
        );
      }
    }
  }

  for (const auto& link : graph.links) {
    ed::Link(
      ed::LinkId(PARTICLE_LINK_ID_BASE + std::to_underlying(link.id) + 1),
      ed::PinId(particle_output_pin_id(link.from_node)),
      ed::PinId(particle_input_pin_id(link.to_node, link.to_pin))
    );
  }

  *positions_applied = true;

  auto structure_modified = false;
  auto layout_modified = false;

  if (ed::BeginCreate()) {
    auto start_pin = ed::PinId();
    auto end_pin = ed::PinId();
    if (ed::QueryNewLink(&start_pin, &end_pin) && start_pin && end_pin) {
      auto from = start_pin.Get();
      auto to = end_pin.Get();
      if (particle_pin_is_output(to)) {
        std::swap(from, to);
      }

      const auto from_node = particle_pin_owner(from);
      const auto to_node = particle_pin_owner(to);
      const auto valid = particle_pin_is_output(from) && !particle_pin_is_output(to) && from_node != to_node &&
                         graph.find_node(from_node) && graph.find_node(to_node);

      if (valid && ed::AcceptNewItem()) {
        // An input pin's slot *is* its pin index; only the output pin sits at the reserved top slot.
        structure_modified |= graph.add_link(from_node, to_node, particle_pin_slot(to)) != ParticleLinkID::Invalid;
      } else if (!valid) {
        ed::RejectNewItem();
      }
    }
  }
  ed::EndCreate();

  if (ed::BeginDelete()) {
    auto link_id = ed::LinkId();
    while (ed::QueryDeletedLink(&link_id)) {
      if (ed::AcceptDeletedItem()) {
        graph.remove_link(static_cast<ParticleLinkID>(link_id.Get() - PARTICLE_LINK_ID_BASE - 1));
        structure_modified = true;
      }
    }

    auto node_id = ed::NodeId();
    while (ed::QueryDeletedNode(&node_id)) {
      if (ed::AcceptDeletedItem()) {
        graph.remove_node(static_cast<ParticleNodeID>(node_id.Get() - 1));
        structure_modified = true;
      }
    }
  }
  ed::EndDelete();

  // Node positions live in the asset, so drags have to be read back out of the editor.
  for (auto& node : graph.nodes) {
    const auto position = ed::GetNodePosition(ed::NodeId(particle_node_editor_id(node.id)));
    if (position.x != node.canvas_position.x || position.y != node.canvas_position.y) {
      node.canvas_position = {position.x, position.y};
      layout_modified = true;
    }
  }

  // node ids are graph-local, so a stale id from another graph would resolve to the wrong node here
  if (self.active_kind == kind) {
    auto picked = ParticleNodeID::Invalid;
    if (const auto selected_count = ed::GetSelectedObjectCount(); selected_count > 0) {
      const auto selected = stack.alloc<ed::NodeId>(static_cast<usize>(selected_count));
      if (ed::GetSelectedNodes(selected.data(), selected_count) > 0) {
        picked = static_cast<ParticleNodeID>(selected[0].Get() - 1);
      }
    }
    self.selected_node = picked;
  }

  auto input = App::mod<Input>();
  if (
    input.get_key_held(ScanCode::LeftControl) &&
    (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) || ed::IsBackgroundClicked())
  ) {
    App::get_window().set_cursor_override(WindowCursor::Hand);
  }

  ed::Suspend();

  auto context_node_id = ed::NodeId();
  auto context_pin_id = ed::PinId();
  auto context_link_id = ed::LinkId();
  if (ed::ShowNodeContextMenu(&context_node_id)) {
    self.context_node = static_cast<ParticleNodeID>(context_node_id.Get() - 1);
    ImGui::OpenPopup("particle_node_context");
  } else if (ed::ShowPinContextMenu(&context_pin_id)) {
    self.context_pin = context_pin_id.Get();
    ImGui::OpenPopup("particle_pin_context");
  } else if (ed::ShowLinkContextMenu(&context_link_id)) {
    self.context_link = static_cast<ParticleLinkID>(context_link_id.Get() - PARTICLE_LINK_ID_BASE - 1);
    ImGui::OpenPopup("particle_link_context");
  } else if (ed::ShowBackgroundContextMenu()) {
    // Captured now because the popup outlives the click, and the new node is placed where the menu
    // was opened rather than wherever the cursor drifted to.
    const auto mouse = ImGui::GetMousePos();
    self.context_screen_position = {mouse.x, mouse.y};
    ImGui::OpenPopup("particle_add_node");
  }

  if (ImGui::BeginPopup("particle_node_context")) {
    if (const auto* node = graph.find_node(self.context_node)) {
      const auto& desc = particle_node_desc(node->type);
      ImGui::TextUnformatted(desc.name.data(), desc.name.data() + desc.name.size());
      ImGui::Separator();
    }

    if (ImGui::MenuItem(stack.format_char("{} Delete Node", ICON_MDI_TRASH_CAN))) {
      // Routed through the editor so it lands in the same delete pass as pressing Del.
      ed::DeleteNode(ed::NodeId(particle_node_editor_id(self.context_node)));
    }

    ImGui::EndPopup();
  }

  if (ImGui::BeginPopup("particle_pin_context")) {
    // Right-clicking the pin itself is the obvious way to unwire something; the link's own hit area
    // is a few pixels of curve and easy to miss.
    const auto owner = particle_pin_owner(self.context_pin);
    const auto is_output = particle_pin_is_output(self.context_pin);
    const auto slot = particle_pin_slot(self.context_pin);

    const auto attached = [&](const ParticleLink& link) {
      return is_output ? link.from_node == owner : link.to_node == owner && link.to_pin == slot;
    };
    const auto connected = std::ranges::any_of(graph.links, attached);

    ImGui::BeginDisabled(!connected);
    if (ImGui::MenuItem(stack.format_char("{} Disconnect", ICON_MDI_TRASH_CAN))) {
      std::erase_if(graph.links, attached);
      structure_modified = true;
    }
    ImGui::EndDisabled();

    ImGui::EndPopup();
  }

  if (ImGui::BeginPopup("particle_link_context")) {
    if (ImGui::MenuItem(stack.format_char("{} Delete Link", ICON_MDI_TRASH_CAN))) {
      ed::DeleteLink(ed::LinkId(PARTICLE_LINK_ID_BASE + std::to_underlying(self.context_link) + 1));
    }

    ImGui::EndPopup();
  }

  if (ImGui::BeginPopup("particle_add_node")) {
    const auto canvas_position = ed::ScreenToCanvas(
      ImVec2(self.context_screen_position.x, self.context_screen_position.y)
    );

    constexpr static std::string_view categories[] = {"Input", "Attribute", "Trigger", "Math", "Vector", "Output"};
    for (const auto category : categories) {
      // a category with nothing valid in this program would open onto an empty menu
      const auto has_any = [&] {
        for (auto type = 0_u32; type < static_cast<u32>(ParticleNodeType::Count); type++) {
          const auto& desc = particle_node_desc(static_cast<ParticleNodeType>(type));
          if (desc.category == category && particle_kind_allows(desc.kinds, kind)) {
            return true;
          }
        }
        return false;
      }();

      if (!has_any) {
        continue;
      }

      if (!ImGui::BeginMenu(stack.null_terminate_cstr(category))) {
        continue;
      }

      for (auto type = 0_u32; type < static_cast<u32>(ParticleNodeType::Count); type++) {
        const auto& desc = particle_node_desc(static_cast<ParticleNodeType>(type));
        if (desc.category != category || !particle_kind_allows(desc.kinds, kind)) {
          continue;
        }

        if (ImGui::MenuItem(stack.null_terminate_cstr(desc.name))) {
          const auto id = graph.add_node(static_cast<ParticleNodeType>(type), {canvas_position.x, canvas_position.y});
          ed::SetNodePosition(ed::NodeId(particle_node_editor_id(id)), canvas_position);
          self.selected_node = id;
          structure_modified = true;
        }
      }

      ImGui::EndMenu();
    }

    ImGui::EndPopup();
  }
  ed::Resume();

  ed::End();
  ed::SetCurrentEditor(nullptr);

  if (structure_modified || layout_modified) {
    self.commit(structure_modified);
  }
}

auto ParticleEditorPanel::draw_inspector(this ParticleEditorPanel& self) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  auto modified = false;
  auto& asset_man = App::mod<AssetManager>();

  const auto node_open = ImGui::CollapsingHeader("Selected Node", ImGuiTreeNodeFlags_DefaultOpen);
  UI::tooltip_hover(
    "Values of the node picked in the graph above. Inputs with nothing wired into them fall back "
    "to the literals shown here."
  );
  if (node_open) {
    auto& graph = self.active_graph();
    auto* node = const_cast<ParticleNode*>(graph.find_node(self.selected_node));
    if (!node) {
      ImGui::TextUnformatted("No node selected.");
      UI::tooltip_hover("Click a node in the graph above to edit it here.");
    } else {
      const auto& desc = particle_node_desc(node->type);
      ImGui::TextUnformatted(desc.name.data(), desc.name.data() + desc.name.size());
      UI::tooltip_hover(stack.format_char("{} node.", desc.category));

      // An output node with nothing plugged in still writes. Every frame it puts its literal over
      // whatever the spawn program set. Worth saying out loud.
      if (is_particle_output_node(node->type)) {
        const auto connected = std::ranges::any_of(graph.links, [node](const ParticleLink& link) {
          return link.to_node == node->id && link.to_pin == 0;
        });

        if (!connected) {
          ImGui::TextColored(
            ImVec4(0.95f, 0.75f, 0.30f, 1.0f),
            "Nothing connected: writes the value below\nover this attribute every frame."
          );
          UI::tooltip_hover(
            "Wire something into this node's input, or delete the node, if you meant whatever the "
            "spawn program set to survive."
          );
        }
      }

      UI::begin_properties();
      for (auto i = 0_u32; i < node->params.size(); i++) {
        // property_vector defaults to a 0..1 drag range, which would make a size above 1 or a
        // negative gravity impossible to author.
        modified |= UI::property_vector(
          stack.format_char("Value {}", i),
          node->params[i],
          false,
          true,
          stack.format_char("Literal used for input {} when nothing is connected to that pin.", i),
          0.01f,
          -PARTICLE_LITERAL_RANGE,
          PARTICLE_LITERAL_RANGE
        );
      }

      if (node->type == ParticleNodeType::Curve && !self.curves.empty()) {
        auto index = static_cast<i32>(node->index);
        auto names = stack.alloc<const c8*>(self.curves.size());
        for (usize i = 0; i < self.curves.size(); i++) {
          names[i] = stack.null_terminate_cstr(self.curves[i].name);
        }
        if (
          UI::property(
            "Curve",
            &index,
            names.data(),
            static_cast<i32>(names.size()),
            "Which curve this node samples. Curves are defined in the Curves section and baked into a lookup "
            "texture the GPU reads."
          )
        ) {
          node->index = static_cast<u32>(index);
          modified = true;
        }
      }

      if (node->type == ParticleNodeType::Gradient && !self.gradients.empty()) {
        auto index = static_cast<i32>(node->index);
        auto names = stack.alloc<const c8*>(self.gradients.size());
        for (usize i = 0; i < self.gradients.size(); i++) {
          names[i] = stack.null_terminate_cstr(self.gradients[i].name);
        }
        if (
          UI::property(
            "Gradient",
            &index,
            names.data(),
            static_cast<i32>(names.size()),
            "Which gradient this node samples. Gradients are defined in the Gradients section and share the "
            "curve lookup texture."
          )
        ) {
          node->index = static_cast<u32>(index);
          modified = true;
        }
      }

      if (node->type == ParticleNodeType::ReadParameter) {
        if (self.parameters.empty()) {
          UI::text(
            "Parameter",
            "none defined",
            "Add a parameter in the Parameters section before this node has anything to read."
          );
        } else {
          auto index = static_cast<i32>(node->index);
          auto names = stack.alloc<const c8*>(self.parameters.size());
          for (usize i = 0; i < self.parameters.size(); i++) {
            names[i] = stack.null_terminate_cstr(self.parameters[i].name);
          }
          if (
            UI::property(
              "Parameter",
              &index,
              names.data(),
              static_cast<i32>(names.size()),
              "Which runtime parameter slot this node reads. The game writes these through "
              "Scene::set_particle_parameter."
            )
          ) {
            node->index = static_cast<u32>(index);
            modified = true;
          }
        }
      }

      switch (node->type) {
        case ParticleNodeType::Interval:
          UI::text(
            "Fires",
            "every Value 0 seconds",
            "Hands back how many whole periods elapsed this frame, so a "
            "period shorter than a frame still fires more than once."
          );
          break;
        case ParticleNodeType::Once:
          UI::text(
            "Fires",
            "once at Value 0 seconds",
            "Measured from the emitter's start delay. Restarting the "
            "emitter re-arms it."
          );
          break;
        case ParticleNodeType::OnRising:
          UI::text(
            "Fires",
            "when Value 0 crosses Value 1",
            "Only on the frame the input goes from below the "
            "threshold to at or above it, so it will not "
            "re-fire until it drops back."
          );
          break;
        case ParticleNodeType::ReadPulse:
          UI::text(
            "Reads",
            "bursts queued by gameplay",
            "Whatever scene:emit_particle_burst queued since the "
            "last frame. Having this node anywhere in the graph "
            "means the graph owns those bursts instead of them "
            "spawning directly."
          );
          break;
        case ParticleNodeType::ReadCycleTime:
          UI::text(
            "Reads",
            "cycle position, 0 to 1",
            "Position within the emitter's Duration. Stays at 0 when "
            "Duration is 0."
          );
          break;
        default: break;
      }

      if (node->type == ParticleNodeType::Random) {
        auto stream = static_cast<i32>(node->index);
        if (
          UI::property(
            "Stream",
            &stream,
            0,
            63,
            1.0f,
            "Random stream index. Two Random nodes on the same stream draw identical numbers for a given "
            "particle; different streams are independent."
          )
        ) {
          node->index = static_cast<u32>(std::max(stream, 0));
          modified = true;
        }
      }
      UI::end_properties();
    }
  }

  const auto emitter_open = ImGui::CollapsingHeader("Emitter", ImGuiTreeNodeFlags_DefaultOpen);
  UI::tooltip_hover(
    "How many particles are born, when, and where. Every entity using this asset shares these "
    "settings."
  );
  if (emitter_open) {
    auto& settings = self.emitter_settings;

    UI::begin_properties();
    modified |= UI::property(
      "Capacity",
      &settings.capacity,
      1_u32,
      1u << 20,
      1.0f,
      "Maximum particles alive at once. The GPU pool is allocated at this size and spawns are dropped once it "
      "is full."
    );
    modified |= UI::property(
      "Spawn Rate",
      &settings.spawn_rate,
      0.0f,
      10000.0f,
      "Particles emitted per second while the emitter is playing. Scaled at runtime by the component's "
      "Emission Rate Scale."
    );
    modified |= UI::property(
      "Duration",
      &settings.duration,
      0.0f,
      1000.0f,
      "Length of one emission cycle in seconds. With Looping off, spawning stops once it elapses."
    );
    modified |= UI::property(
      "Start Delay",
      &settings.start_delay,
      0.0f,
      1000.0f,
      "Seconds to wait after the emitter starts before the first particle spawns."
    );
    modified |= UI::property(
      "Looping",
      &settings.looping,
      "Restart the cycle once Duration elapses, re-arming every burst."
    );
    modified |= UI::property_vector(
      "Lifetime",
      settings.lifetime,
      false,
      true,
      "Minimum and maximum seconds a particle lives. Each one picks a random value in this range.",
      0.05f,
      0.0f,
      100.0f
    );

    static const c8* shape_names[] = {"Point", "Sphere", "Hemisphere", "Box", "Circle", "Cone"};
    auto shape = static_cast<i32>(settings.shape);
    if (
      UI::property(
        "Shape",
        &shape,
        shape_names,
        static_cast<i32>(std::size(shape_names)),
        "The volume particles spawn in and the direction they start moving. Outlined in the preview when the "
        "shape overlay is on."
      )
    ) {
      settings.shape = static_cast<ParticleEmissionShape>(shape);
      modified = true;
    }
    modified |= UI::property_vector(
      "Shape Size",
      settings.shape_size,
      false,
      true,
      "Per-axis extent of the shape: radii for Sphere and Hemisphere, half-extents for Box, X and Z radii for "
      "Circle. Cone uses X as the base radius and Y as the height it fills, and Point ignores it entirely.",
      0.05f,
      0.0f,
      100.0f
    );
    modified |= UI::property(
      "Cone Angle",
      &settings.shape_angle,
      0.0f,
      89.0f,
      "Half-angle in degrees the cone widens by over its height, and the angle particles fly out at. Used by "
      "the Cone shape only."
    );

    static const c8* space_names[] = {"World", "Local"};
    auto space = static_cast<i32>(settings.simulation_space);
    if (
      UI::property(
        "Simulation Space",
        &space,
        space_names,
        static_cast<i32>(std::size(space_names)),
        "World leaves particles where they spawned, so a moving emitter trails them behind it. Local stores "
        "them relative to the emitter, so the whole plume moves and rotates with it."
      )
    ) {
      settings.simulation_space = static_cast<ParticleSimulationSpace>(space);
      modified = true;
    }
    modified |= UI::property(
      "Seed",
      &settings.seed,
      0_u32,
      0_u32,
      1.0f,
      "Base random seed, combined with the component's own seed so two entities sharing this asset can differ."
    );
    UI::end_properties();
  }

  const auto rendering_open = ImGui::CollapsingHeader("Rendering", ImGuiTreeNodeFlags_DefaultOpen);
  UI::tooltip_hover("How particles are drawn once they exist. Nothing here changes the simulation.");
  if (rendering_open) {
    auto& settings = self.render_settings;

    UI::begin_properties();
    static const c8* mode_names[] = {"Billboard", "Mesh"};
    auto mode = static_cast<i32>(settings.render_mode);
    if (
      UI::property(
        "Render Mode",
        &mode,
        mode_names,
        static_cast<i32>(std::size(mode_names)),
        "Billboard draws a camera-facing quad per particle. Mesh draws the assigned model per particle."
      )
    ) {
      settings.render_mode = static_cast<ParticleRenderMode>(mode);
      modified = true;
    }

    static const c8* billboard_names[] = {
      "Face Camera",
      "Velocity Stretched",
      "Horizontal Plane",
      "Vertical Plane",
    };
    auto billboard = static_cast<i32>(settings.billboard);
    if (
      UI::property(
        "Billboard",
        &billboard,
        billboard_names,
        static_cast<i32>(std::size(billboard_names)),
        "Which way each quad faces: at the camera, leaning along its own motion, flat on the ground, or "
        "standing upright and turning to follow the camera."
      )
    ) {
      settings.billboard = static_cast<ParticleBillboardMode>(billboard);
      modified = true;
    }

    static const c8* blend_names[] = {"Alpha Blend", "Additive"};
    auto blend = static_cast<i32>(settings.blend);
    if (
      UI::property(
        "Blend",
        &blend,
        blend_names,
        static_cast<i32>(std::size(blend_names)),
        "Alpha Blend composites the particle over the scene. Additive only brightens it, which suits fire, "
        "sparks and glows."
      )
    ) {
      settings.blend = static_cast<ParticleBlendMode>(blend);
      modified = true;
    }

    modified |= UI::property_vector(
      "Flipbook",
      settings.flipbook,
      false,
      true,
      "Columns and rows of the material's sprite atlas. The frame is stepped automatically across the "
      "particle's lifetime. 1 x 1 means a single image.",
      1.0f,
      1.0f,
      32.0f
    );
    modified |= UI::property(
      "Soft Particle Distance",
      &settings.soft_particle_distance,
      0.0f,
      100.0f,
      "Fade the particle out as it approaches scene geometry, over this distance in world units. 0 disables "
      "the fade and the hard intersection edge comes back."
    );
    modified |= UI::property(
      "Velocity Stretch",
      &settings.velocity_stretch,
      0.0f,
      10.0f,
      "How far to elongate the quad along its velocity. Only used by the Velocity Stretched billboard mode."
    );
    modified |= UI::property(
      "Restitution",
      &settings.restitution,
      0.0f,
      1.0f,
      "Fraction of speed kept when a particle bounces. Only used when Depth Collision is on."
    );
    modified |= UI::property(
      "Sort",
      &settings.sort,
      "Sort particles back to front so alpha blending layers correctly. Sorting is frame-wide, so turning "
      "it on here sorts every emitter's particles. The renderer's own particle sort setting can still "
      "override it off."
    );
    modified |= UI::property(
      "Depth Collision",
      &settings.depth_collision,
      "Bounce particles off scene geometry sampled from the depth buffer. Only what the camera can see is "
      "solid, so particles pass through anything off screen or occluded."
    );
    UI::end_properties();

    const auto asset_slot = [&asset_man, &modified](const c8* label, UUID& uuid, const c8* tooltip) {
      auto text = uuid ? uuid.str() : std::string("None");
      const auto clear_width = ImGui::GetFrameHeight();

      ImGui::TextUnformatted(label);
      UI::tooltip_hover(tooltip);
      ImGui::PushID(label);
      ImGui::Button(text.c_str(), ImVec2(-(clear_width + ImGui::GetStyle().ItemSpacing.x), 0.0f));
      UI::tooltip_hover("Drag an asset here from the content browser to assign it.");
      // no load/unload here: `edit_particle_system` hands the sub-asset refs over on commit, once
      // per holder of the system
      if (ImGui::BeginDragDropTarget()) {
        if (const auto* payload = ImGui::AcceptDragDropPayload(PayloadData::DRAG_DROP_SOURCE)) {
          const auto* data = PayloadData::from_payload(payload);
          if (!data->get_str().empty()) {
            if (const auto imported = import_asset(asset_man, data->str)) {
              uuid = imported;
              modified = true;
            }
          }
        }
        ImGui::EndDragDropTarget();
      }

      ImGui::SameLine();
      ImGui::BeginDisabled(!uuid);
      if (UI::button(ICON_MDI_TRASH_CAN, ImVec2(clear_width, 0.0f), "Detach")) {
        uuid = UUID(nullptr);
        modified = true;
      }
      ImGui::EndDisabled();
      ImGui::PopID();
    };

    asset_slot(
      "Material",
      settings.material,
      "Material every particle of this emitter is shaded with. Its albedo texture is what the flipbook grid "
      "indexes into."
    );
    if (settings.render_mode == ParticleRenderMode::Mesh) {
      asset_slot("Mesh", settings.mesh, "Model drawn per particle. The first mesh of the model is used.");
    }
  }

  const auto curves_open = ImGui::CollapsingHeader("Curves");
  UI::tooltip_hover(
    "Scalar ramps a Curve node samples, usually by a particle's normalized age. Baked into one "
    "lookup texture alongside the gradients."
  );
  if (curves_open) {
    const auto row_button_width = ImGui::GetFrameHeight();

    for (usize i = 0; i < self.curves.size(); i++) {
      ImGui::PushID(static_cast<i32>(i));
      auto& curve = self.curves[i];

      UI::begin_properties();
      modified |= UI::input_text(
        "Name",
        &curve.name,
        0,
        nullptr,
        nullptr,
        "Name shown in the Curve node's dropdown. Only the row order reaches the GPU, so renaming is safe."
      );
      UI::end_properties();

      modified |= self.draw_curve_editor(i, curve);

      auto removed_point = self.curves[i].points.size();
      const auto points_open = ImGui::TreeNode("Points");
      UI::tooltip_hover("Exact control point values. Dragging in the plot above edits the same data.");
      if (points_open) {
        for (usize point = 0; point < curve.points.size(); point++) {
          ImGui::PushID(static_cast<i32>(point));
          const auto field_width = (ImGui::GetContentRegionAvail().x - row_button_width -
                                    ImGui::GetStyle().ItemSpacing.x * 2.0f) *
                                   0.5f;
          ImGui::SetNextItemWidth(field_width);
          modified |= ImGui::DragFloat("##t", &curve.points[point].x, 0.01f, 0.0f, 1.0f);
          UI::tooltip_hover(
            "Position along the curve, 0 to 1. Whatever feeds the Curve node is clamped to this range."
          );
          ImGui::SameLine();
          ImGui::SetNextItemWidth(field_width);
          modified |= ImGui::DragFloat(
            "##value",
            &curve.points[point].y,
            0.01f,
            -PARTICLE_LITERAL_RANGE,
            PARTICLE_LITERAL_RANGE
          );
          UI::tooltip_hover("Value the curve returns at this position.");
          ImGui::SameLine();
          if (UI::button(ICON_MDI_TRASH_CAN, ImVec2(row_button_width, 0.0f), "Remove point")) {
            removed_point = point;
          }
          ImGui::PopID();
        }

        if (UI::button("Add Point", ImVec2(0.0f, 0.0f), "Append a control point at the end of the curve")) {
          curve.points.emplace_back(1.0f, 1.0f);
          modified = true;
        }

        ImGui::TreePop();
      }

      if (removed_point < curve.points.size()) {
        curve.points.erase(curve.points.begin() + static_cast<std::ptrdiff_t>(removed_point));
        modified = true;
      }

      const auto remove_curve = UI::button(
        stack.format_char("{} Remove Curve", ICON_MDI_TRASH_CAN),
        ImVec2(0.0f, 0.0f),
        "Delete this curve. Curve nodes pointing past it shift down to keep sampling the same row."
      );
      ImGui::Separator();
      ImGui::PopID();

      if (remove_curve) {
        self.curves.erase(self.curves.begin() + static_cast<std::ptrdiff_t>(i));
        remap_particle_sampler_indices(self.spawn_graph, ParticleNodeType::Curve, static_cast<u32>(i));
        remap_particle_sampler_indices(self.update_graph, ParticleNodeType::Curve, static_cast<u32>(i));
        modified = true;
        break;
      }
    }

    if (UI::button("Add Curve", ImVec2(0.0f, 0.0f), "Add a curve a Curve node can sample over a particle's life")) {
      self.curves.emplace_back();
      modified = true;
    }
  }

  const auto gradients_open = ImGui::CollapsingHeader("Gradients");
  UI::tooltip_hover(
    "Colour ramps a Gradient node samples, usually by a particle's normalized age. Share the "
    "lookup texture with the curves."
  );
  if (gradients_open) {
    const auto row_button_width = ImGui::GetFrameHeight();

    for (usize i = 0; i < self.gradients.size(); i++) {
      ImGui::PushID(static_cast<i32>(i));
      auto& gradient = self.gradients[i];

      UI::begin_properties();
      modified |= UI::input_text(
        "Name",
        &gradient.name,
        0,
        nullptr,
        nullptr,
        "Name shown in the Gradient node's dropdown. Only the row order reaches the GPU, so renaming is safe."
      );
      UI::end_properties();

      auto removed_key = gradient.keys.size();
      for (usize key = 0; key < gradient.keys.size(); key++) {
        ImGui::PushID(static_cast<i32>(key));
        const auto field_width = (ImGui::GetContentRegionAvail().x - row_button_width -
                                  ImGui::GetStyle().ItemSpacing.x * 2.0f) *
                                 0.5f;
        ImGui::SetNextItemWidth(field_width);
        modified |= ImGui::DragFloat("##time", &gradient.keys[key].t, 0.01f, 0.0f, 1.0f);
        UI::tooltip_hover("Position of this key along the gradient, 0 to 1.");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(field_width);
        modified |= ImGui::ColorEdit4("##color", glm::value_ptr(gradient.keys[key].color));
        UI::tooltip_hover("Colour at this key. Alpha rides along to the particle, so it doubles as a fade.");
        ImGui::SameLine();
        if (UI::button(ICON_MDI_TRASH_CAN, ImVec2(row_button_width, 0.0f), "Remove key")) {
          removed_key = key;
        }
        ImGui::PopID();
      }

      if (removed_key < gradient.keys.size()) {
        gradient.keys.erase(gradient.keys.begin() + static_cast<std::ptrdiff_t>(removed_key));
        modified = true;
      }

      if (UI::button("Add Key", ImVec2(0.0f, 0.0f), "Append a colour key to this gradient")) {
        gradient.keys.emplace_back();
        modified = true;
      }
      ImGui::SameLine();
      const auto remove_gradient = UI::button(
        stack.format_char("{} Remove Gradient", ICON_MDI_TRASH_CAN),
        ImVec2(0.0f, 0.0f),
        "Delete this gradient. Gradient nodes pointing past it shift down to keep sampling the same row."
      );
      ImGui::Separator();
      ImGui::PopID();

      if (remove_gradient) {
        self.gradients.erase(self.gradients.begin() + static_cast<std::ptrdiff_t>(i));
        remap_particle_sampler_indices(self.spawn_graph, ParticleNodeType::Gradient, static_cast<u32>(i));
        remap_particle_sampler_indices(self.update_graph, ParticleNodeType::Gradient, static_cast<u32>(i));
        modified = true;
        break;
      }
    }

    if (
      UI::button(
        "Add Gradient",
        ImVec2(0.0f, 0.0f),
        "Add a colour ramp a Gradient node can sample over a particle's life"
      )
    ) {
      self.gradients.emplace_back();
      modified = true;
    }
  }

  const auto parameters_open = ImGui::CollapsingHeader("Parameters");
  UI::tooltip_hover("Runtime slots gameplay can drive per entity. A Parameter node reads one by index.");
  if (parameters_open) {
    ImGui::TextWrapped(
      "Slots the game writes at runtime. A Parameter node reads one, the value below is what an "
      "emitter uses until something overrides it."
    );
    UI::tooltip_hover(
      "From gameplay: scene:set_particle_parameter(entity, name_or_index, value). The first write "
      "flips the entity's Override Parameters flag and seeds the rest from these defaults."
    );

    for (usize i = 0; i < self.parameters.size(); i++) {
      ImGui::PushID(static_cast<i32>(i));
      auto& parameter = self.parameters[i];

      UI::begin_properties();
      modified |= UI::input_text(
        "Name",
        &parameter.name,
        0,
        nullptr,
        nullptr,
        "The name gameplay passes to Scene::set_particle_parameter, and what the Parameter node's dropdown "
        "shows. The GPU only ever sees the slot index, so renaming costs nothing but breaks by-name lookups."
      );
      modified |= UI::property_vector(
        "Default",
        parameter.default_value,
        false,
        true,
        "Value every emitter uses until it overrides this parameter. The preview always runs on these, so "
        "this is what you author against.",
        0.01f,
        -PARTICLE_LITERAL_RANGE,
        PARTICLE_LITERAL_RANGE
      );
      UI::end_properties();

      const auto remove_parameter = UI::button(
        stack.format_char("{} Remove Parameter", ICON_MDI_TRASH_CAN),
        ImVec2(0.0f, 0.0f),
        "Delete this parameter. Parameter nodes pointing past it shift down to keep reading the same slot."
      );
      ImGui::Separator();
      ImGui::PopID();

      if (remove_parameter) {
        self.parameters.erase(self.parameters.begin() + static_cast<std::ptrdiff_t>(i));
        remap_particle_sampler_indices(self.spawn_graph, ParticleNodeType::ReadParameter, static_cast<u32>(i));
        remap_particle_sampler_indices(self.update_graph, ParticleNodeType::ReadParameter, static_cast<u32>(i));
        modified = true;
        break;
      }
    }

    if (
      self.parameters.size() < GPU::PARTICLE_USER_PARAM_COUNT &&
      UI::button(
        "Add Parameter",
        ImVec2(0.0f, 0.0f),
        stack.format_char("Add a runtime slot the game can drive. {} slots maximum.", GPU::PARTICLE_USER_PARAM_COUNT)
      )
    ) {
      self.parameters.emplace_back();
      modified = true;
    }
  }

  if (modified) {
    self.commit();
  }
}

auto ParticleEditorPanel::draw_curve_editor(
  this ParticleEditorPanel& self, const usize curve_index, ParticleCurve& curve
) -> bool {
  ZoneScoped;

  const auto plot_height = UI::scale(140.0f);
  const auto grab_radius = UI::scale(7.0f);
  const auto point_radius = UI::scale(4.0f);

  auto modified = false;

  // let the vertical range follow curves that extend beyond 1
  auto min_value = 0.0f;
  auto max_value = 1.0f;
  for (const auto& point : curve.points) {
    min_value = std::min(min_value, point.y);
    max_value = std::max(max_value, point.y);
  }

  const auto padding = std::max(max_value - min_value, 0.001f) * 0.1f;
  min_value -= padding;
  max_value += padding;

  // keep a stable scale while dragging so auto-ranging cannot amplify pointer movement
  if (self.dragged_curve == curve_index) {
    min_value = self.dragged_curve_range.x;
    max_value = self.dragged_curve_range.y;
  }

  constexpr auto plot_flags = ImPlotFlags_CanvasOnly;
  constexpr auto axis_flags = ImPlotAxisFlags_Lock | ImPlotAxisFlags_NoMenus | ImPlotAxisFlags_NoSideSwitch;

  if (ImPlot::BeginPlot("##curve_plot", ImVec2(-1.0f, plot_height), plot_flags)) {
    ImPlot::SetupAxes(nullptr, nullptr, axis_flags, axis_flags);
    ImPlot::SetupAxesLimits(0.0, 1.0, min_value, max_value, ImPlotCond_Always);
    ImPlot::SetupFinish();
    const auto plot_size = ImPlot::GetPlotSize();

    auto removed_point = curve.points.size();
    auto active_point = curve.points.size();
    auto point_hovered = false;
    auto point_held = false;

    // Invisible drag handles let ImPlot own hit-testing and plot-space conversion. Markers are
    // rendered afterwards so the sampled line never draws over them.
    for (usize i = 0; i < curve.points.size(); i++) {
      auto x = static_cast<f64>(curve.points[i].x);
      auto y = static_cast<f64>(curve.points[i].y);
      auto hovered = false;
      auto held = false;

      const auto dragging = ImPlot::DragPoint(
        static_cast<i32>(i),
        &x,
        &y,
        ImVec4(0.0f, 0.0f, 0.0f, 0.0f),
        grab_radius,
        ImPlotDragToolFlags_NoFit,
        nullptr,
        &hovered,
        &held
      );

      if (dragging) {
        App::get_window().set_cursor_override(WindowCursor::Crosshair);

        const auto mouse_delta = ImGui::GetIO().MouseDelta;
        const auto value_span = std::max(max_value - min_value, 0.001f);
        curve.points[i] = {
          std::clamp(curve.points[i].x + mouse_delta.x / std::max(plot_size.x, 1.0f), 0.0f, 1.0f),
          std::clamp(
            curve.points[i].y - mouse_delta.y / std::max(plot_size.y, 1.0f) * value_span,
            -PARTICLE_LITERAL_RANGE,
            PARTICLE_LITERAL_RANGE
          ),
        };
        modified = true;
      }

      if (hovered || held) {
        active_point = i;
      }
      point_hovered |= hovered;
      point_held |= held;

      if (held) {
        if (self.dragged_curve != curve_index) {
          self.dragged_curve_range = {min_value, max_value};
        }
        self.dragged_curve = curve_index;

        const auto range_span = self.dragged_curve_range.y - self.dragged_curve_range.x;
        const auto follow_margin = range_span * 0.05f;
        auto range_shift = 0.0f;
        if (curve.points[i].y < self.dragged_curve_range.x + follow_margin) {
          range_shift = curve.points[i].y - (self.dragged_curve_range.x + follow_margin);
        } else if (curve.points[i].y > self.dragged_curve_range.y - follow_margin) {
          range_shift = curve.points[i].y - (self.dragged_curve_range.y - follow_margin);
        }
        self.dragged_curve_range += glm::vec2(range_shift);
      }

      // A curve needs two points to interpolate between.
      if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && curve.points.size() > 2) {
        removed_point = i;
      }
    }

    if (removed_point < curve.points.size()) {
      curve.points.erase(curve.points.begin() + static_cast<std::ptrdiff_t>(removed_point));
      active_point = curve.points.size();
      modified = true;
    }

    if (!point_held && self.dragged_curve == curve_index) {
      // Keep the stored order matching the visual order once the drag settles.
      std::ranges::sort(curve.points, [](const glm::vec2& a, const glm::vec2& b) { return a.x < b.x; });
      self.dragged_curve = ~0_sz;
      modified = true;
    }

    if (ImPlot::IsPlotHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !point_hovered) {
      const auto mouse = ImPlot::GetPlotMousePos();
      curve.points.emplace_back(
        std::clamp(static_cast<f32>(mouse.x), 0.0f, 1.0f),
        std::clamp(static_cast<f32>(mouse.y), -PARTICLE_LITERAL_RANGE, PARTICLE_LITERAL_RANGE)
      );
      std::ranges::sort(curve.points, [](const glm::vec2& a, const glm::vec2& b) { return a.x < b.x; });
      modified = true;
    }

    auto line_spec = ImPlotSpec{};
    line_spec.LineColor = ImVec4(120.0f / 255.0f, 190.0f / 255.0f, 1.0f, 1.0f);
    line_spec.LineWeight = UI::scale(2.0f);

    // Sample at the atlas resolution, so the plot is exactly what gets baked.
    ImPlot::PlotLineG(
      "##curve",
      [](const i32 index, void* data) -> ImPlotPoint {
        const auto& sampled_curve = *static_cast<const ParticleCurve*>(data);
        const auto t = static_cast<f32>(index) / static_cast<f32>(ParticleSystem::CURVE_ATLAS_WIDTH - 1);
        return {t, sampled_curve.sample(t)};
      },
      &curve,
      static_cast<i32>(ParticleSystem::CURVE_ATLAS_WIDTH),
      line_spec
    );

    if (!curve.points.empty()) {
      auto point_spec = ImPlotSpec{};
      point_spec.Marker = ImPlotMarker_Circle;
      point_spec.MarkerSize = point_radius;
      point_spec.MarkerLineColor = ImVec4(0.0f, 0.0f, 0.0f, 0.63f);
      point_spec.MarkerFillColor = ImVec4(230.0f / 255.0f, 230.0f / 255.0f, 235.0f / 255.0f, 1.0f);

      ImPlot::PlotScatterG(
        "##control_points",
        [](const i32 index, void* data) -> ImPlotPoint {
          const auto& points = *static_cast<const std::vector<glm::vec2>*>(data);
          return {points[index].x, points[index].y};
        },
        &curve.points,
        static_cast<i32>(curve.points.size()),
        point_spec
      );
    }

    if (active_point < curve.points.size()) {
      const auto active_x = static_cast<f64>(curve.points[active_point].x);
      const auto active_y = static_cast<f64>(curve.points[active_point].y);
      auto active_spec = ImPlotSpec{};
      active_spec.Marker = ImPlotMarker_Circle;
      active_spec.MarkerSize = point_radius;
      active_spec.MarkerLineColor = ImVec4(0.0f, 0.0f, 0.0f, 0.63f);
      active_spec.MarkerFillColor = ImVec4(1.0f, 220.0f / 255.0f, 130.0f / 255.0f, 1.0f);
      ImPlot::PlotScatter("##active_point", &active_x, &active_y, 1, active_spec);
    }

    // Suppressed mid-drag: the tooltip would sit on top of the point being moved.
    if (ImPlot::IsPlotHovered() && !point_held) {
      const auto mouse = ImPlot::GetPlotMousePos();
      ImGui::SetTooltip("t %.3f  value %.3f\nDouble-click to add, right-click a point to remove", mouse.x, mouse.y);
    }

    ImPlot::EndPlot();
    UI::tooltip_hover(
      "Drag a control point to move it. Double-click empty space to add one, right-click a point "
      "to delete it. The curve always keeps at least two points."
    );
  }

  return modified;
}

auto ParticleEditorPanel::draw_shape_overlay(
  this ParticleEditorPanel& self, const ImVec2 image_min, const ImVec2 image_size
) -> void {
  ZoneScoped;

  if (
    !self.preview_scene || !self.preview_emitter.has<TransformComponent>() || image_size.x <= 0.0f ||
    image_size.y <= 0.0f
  ) {
    return;
  }

  auto clip_from_world = glm::mat4(0.0f);
  self.preview_scene->world.query_builder<const CameraComponent>().build().each(
    [&clip_from_world](const CameraComponent& cc) {
      clip_from_world = cc.get_projection_matrix() * cc.get_view_matrix();
    }
  );

  if (clip_from_world == glm::mat4(0.0f)) {
    return;
  }

  auto* draw_list = ImGui::GetWindowDrawList();
  draw_list->PushClipRect(image_min, ImVec2(image_min.x + image_size.x, image_min.y + image_size.y), true);

  const auto gizmo = ParticleShapeGizmo{
    .clip_from_local = clip_from_world * Scene::get_world_transform(self.preview_emitter),
    .draw_list = draw_list,
    .origin = image_min,
    .size = image_size,
    .color = ImGui::GetColorU32(ImVec4(0.30f, 0.85f, 1.0f, 0.60f)),
  };

  draw_particle_shape(
    gizmo,
    self.emitter_settings.shape,
    self.emitter_settings.shape_size,
    self.emitter_settings.shape_angle
  );

  draw_list->PopClipRect();
}

auto ParticleEditorPanel::draw_preview(this ParticleEditorPanel& self, const vuk::ImageAttachment& swapchain_attachment)
  -> void {
  ZoneScoped;

  self.ensure_preview_scene();

  const auto available = ImGui::GetContentRegionAvail();
  const auto minimum_preview_size = UI::scale(32.0f);
  self.preview_size = {
    std::max(available.x, minimum_preview_size),
    std::max(available.y - UI::scale(64.0f), minimum_preview_size),
  };

  auto attachment_info = swapchain_attachment;
  attachment_info.extent = vuk::Extent3D{
    static_cast<u32>(self.preview_size.x),
    static_cast<u32>(self.preview_size.y),
    1u,
  };

  auto attachment = vuk::declare_ia("particle preview", attachment_info);
  attachment = vuk::clear_image(
    std::move(attachment),
    vuk::ClearColor(
      self.preview_background.r,
      self.preview_background.g,
      self.preview_background.b,
      self.preview_background.a
    )
  );

  const auto orbit = glm::vec3(
    std::cos(self.preview_orbit_pitch) * std::sin(self.preview_orbit_yaw),
    std::sin(self.preview_orbit_pitch),
    std::cos(self.preview_orbit_pitch) * std::cos(self.preview_orbit_yaw)
  );
  const auto camera_position = orbit * self.preview_distance + glm::vec3(0.0f, 1.0f, 0.0f);

  self.preview_scene->world.query_builder<TransformComponent, CameraComponent>().build().each(
    [&](TransformComponent& tc, CameraComponent& cc) {
      tc.position = camera_position;
      tc.rotation = glm::quatLookAt(glm::normalize(-camera_position + glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(0, 1, 0));
      cc.position = camera_position;
    }
  );

  // Re-read every frame so the colour picker below takes effect without rebuilding the scene.
  if (self.preview_sky.has<SkyComponent>()) {
    self.preview_sky.get_mut<SkyComponent>().solid_color = self.preview_background;
  }

  // Pause and speed ride on the component the emitter tick already reads, so the scene still gets a
  // full update every frame. `RendererInstance::render` asserts that `update` ran this frame.
  if (self.preview_emitter.has<ParticleSystemComponent>()) {
    self.preview_emitter.get_mut<ParticleSystemComponent>().simulation_speed = self.preview_playing ? self.preview_speed
                                                                                                    : 0.0f;
  }

  // Paired with the render below the way ThumbnailManager pairs them: the panel can be opened from
  // inside another panel's on_render, by which point update_all has already run past it.
  self.preview_scene->runtime_update(App::get_timestep());

  if (self.preview_grid_enabled) {
    if (auto* renderer_instance = self.preview_scene->get_renderer_instance(); renderer_instance != nullptr) {
      add_editor_grid_stage(*renderer_instance, PARTICLE_PREVIEW_GRID_DISTANCE);
    }
  }

  auto image = self.preview_scene->render(
    std::move(attachment),
    glm::ivec2(0),
    glm::ivec2(static_cast<i32>(self.preview_size.x), static_cast<i32>(self.preview_size.y)),
    glm::ivec2(static_cast<i32>(self.preview_size.x), static_cast<i32>(self.preview_size.y)),
    false
  );

  const auto image_size = ImVec2(self.preview_size.x, self.preview_size.y);
  const auto image_cursor = ImGui::GetCursorPos();
  UI::image(std::move(image), image_size);

  // An image is not an interactive item, so a drag over it would reach ImGui as a window move. The
  // invisible button claims the mouse for the orbit instead.
  ImGui::SetCursorPos(image_cursor);
  ImGui::InvisibleButton("##preview_input", image_size, ImGuiButtonFlags_MouseButtonLeft);

  if (ImGui::IsItemActive()) {
    const auto delta = ImGui::GetIO().MouseDelta;
    self.preview_orbit_yaw -= delta.x * 0.01f;
    self.preview_orbit_pitch = std::clamp(self.preview_orbit_pitch + delta.y * 0.01f, -1.5f, 1.5f);
  }

  if (ImGui::IsItemHovered()) {
    self.preview_distance = std::clamp(self.preview_distance - ImGui::GetIO().MouseWheel * 0.5f, 0.5f, 100.0f);
  }

  if (self.preview_shape_enabled) {
    self.draw_shape_overlay(ImGui::GetItemRectMin(), image_size);
  }

  if (UI::button(self.preview_playing ? ICON_MDI_PAUSE : ICON_MDI_PLAY)) {
    self.preview_playing = !self.preview_playing;
  }
  ImGui::SameLine();
  if (UI::button(ICON_MDI_RESTART)) {
    self.preview_asset = {};
    self.preview_emitter.remove<ParticleSystemComponent>();
    self.sync_preview_asset();
  }
  ImGui::SameLine();
  if (UI::toggle_button(ICON_MDI_GRID, self.preview_grid_enabled)) {
    self.preview_grid_enabled = !self.preview_grid_enabled;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
    ImGui::SetTooltip("Toggle grid");
  }
  ImGui::SameLine();
  if (UI::toggle_button(ICON_MDI_SHAPE_OUTLINE, self.preview_shape_enabled)) {
    self.preview_shape_enabled = !self.preview_shape_enabled;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
    ImGui::SetTooltip("Toggle emission shape");
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(UI::scale(120.0f));
  ImGui::SliderFloat("Speed", &self.preview_speed, 0.0f, 4.0f);
  ImGui::SameLine();
  ImGui::ColorEdit3(
    "Background",
    glm::value_ptr(self.preview_background),
    ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoAlpha
  );

  auto alive_particles = 0_u32;
  if (const auto* renderer_instance = self.preview_scene->get_renderer_instance(); renderer_instance != nullptr) {
    alive_particles = self.emitter_settings.capacity;
  }
  ImGui::Text("Pool capacity: %u", alive_particles);
}

auto ParticleEditorPanel::on_render(this ParticleEditorPanel& self, const vuk::ImageAttachment swapchain_attachment)
  -> void {
  ZoneScoped;

  self.sync_node_editor_scale();

  if (!self.on_begin()) {
    self.on_end();
    return;
  }

  if (!self.asset_uuid) {
    ImGui::TextUnformatted("Open a particle system from the content browser to edit it.");
    self.on_end();
    return;
  }

  if (UI::button(ICON_MDI_CONTENT_SAVE " Save") && !self.asset_path.empty()) {
    export_asset(App::mod<AssetManager>(), self.asset_uuid, self.asset_path);
  }

  if (!self.compile_error.empty()) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%s", self.compile_error.c_str());
  }

  const auto region = ImGui::GetContentRegionAvail();
  const auto splitter_width = UI::scale(6.0f);
  const auto min_column_width = UI::scale(200.0f);

  // A splitter takes width off the column to its right, so the canvas absorbs whatever is left.
  const auto drag_splitter = [&](const c8* id, f32& width) {
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::InvisibleButton(id, ImVec2(splitter_width, region.y));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    if (ImGui::IsItemActive()) {
      width -= ImGui::GetIO().MouseDelta.x;
    }
    ImGui::SameLine(0.0f, 0.0f);
  };

  const auto max_column_width = std::max(region.x - min_column_width * 2.0f - splitter_width * 2.0f, min_column_width);
  self.inspector_width = std::clamp(self.inspector_width, min_column_width, max_column_width);
  self.preview_width = std::clamp(self.preview_width, min_column_width, max_column_width);

  auto canvas_width = region.x - self.inspector_width - self.preview_width - splitter_width * 2.0f;
  if (canvas_width < min_column_width) {
    // Shrink the preview first; it is the column the user resizes most and the least layout-bound.
    const auto overflow = min_column_width - canvas_width;
    const auto from_preview = std::min(overflow, self.preview_width - min_column_width);
    self.preview_width -= from_preview;
    self.inspector_width = std::max(self.inspector_width - (overflow - from_preview), min_column_width);
    canvas_width = std::max(region.x - self.inspector_width - self.preview_width - splitter_width * 2.0f, 1.0f);
  }

  // the graphs are windows docked into this space rather than tab items, so a tab can be dragged out
  // and two programs edited side by side
  const auto dockspace_id = ImGui::GetID("particle_graph_dockspace");
  if (!self.graph_dock_built) {
    self.graph_dock_built = true;
    // a node restored from imgui.ini already carries the artist's layout
    if (ImGui::DockBuilderGetNode(dockspace_id) == nullptr) {
      ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
      ImGui::DockBuilderSetNodeSize(dockspace_id, ImVec2(canvas_width, std::max(region.y, 1.0f)));
      for (const auto kind : PARTICLE_PROGRAM_KINDS) {
        ImGui::DockBuilderDockWindow(particle_kind_window_id(kind), dockspace_id);
      }
      ImGui::DockBuilderFinish(dockspace_id);
    }
  }
  ImGui::DockSpace(dockspace_id, ImVec2(canvas_width, 0.0f));

  drag_splitter("##canvas_splitter", self.preview_width);
  ImGui::BeginChild(
    "particle_preview",
    ImVec2(self.preview_width, 0.0f),
    ImGuiChildFlags_Borders,
    ImGuiWindowFlags_NoScrollWithMouse
  );
  self.draw_preview(swapchain_attachment);
  ImGui::EndChild();

  drag_splitter("##preview_splitter", self.inspector_width);
  ImGui::BeginChild("particle_inspector", ImVec2(self.inspector_width, 0.0f), ImGuiChildFlags_Borders);
  self.draw_inspector();
  ImGui::EndChild();

  self.on_end();

  // submitted after the host window: imgui undocks a window that begins before the node it lives in
  for (const auto kind : PARTICLE_PROGRAM_KINDS) {
    constexpr auto window_flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::Begin(particle_kind_window_id(kind), nullptr, window_flags)) {
      ImGui::TextDisabled("%s %s", ICON_MDI_INFORMATION_OUTLINE, particle_kind_summary(kind));
      UI::tooltip_hover(particle_kind_description(kind));
      self.draw_canvas(kind);
    }
    ImGui::End();
  }
}
} // namespace ox
