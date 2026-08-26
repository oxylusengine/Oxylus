#include "ParticleEditorPanel.hpp"

#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <icons/IconsMaterialDesignIcons.h>
#include <imgui-node-editor/imgui_node_editor.h>
#include <imgui.h>
#include <implot.h>
#include <vuk/vsl/Core.hpp>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
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

ParticleEditorPanel::ParticleEditorPanel() : EditorPanelState("Particle Editor", ICON_MDI_LAMP, false) {
  this->window_default_size = {1280, 720};

  auto config = ed::Config{};
  config.SettingsFile = nullptr;
  spawn_context = ed::CreateEditor(&config);
  update_context = ed::CreateEditor(&config);
}

ParticleEditorPanel::~ParticleEditorPanel() {
  if (spawn_context) {
    ed::DestroyEditor(spawn_context);
  }
  if (update_context) {
    ed::DestroyEditor(update_context);
  }

  if (preview_scene && preview_asset) {
    preview_emitter = {};
    preview_scene.reset();
  }
}

auto ParticleEditorPanel::active_graph(this ParticleEditorPanel& self) -> ParticleGraph& {
  return self.active_kind == ParticleProgramKind::Spawn ? self.spawn_graph : self.update_graph;
}

auto ParticleEditorPanel::open_asset(this ParticleEditorPanel& self, const UUID& uuid) -> void {
  ZoneScoped;

  auto& asset_man = App::mod<AssetManager>();

  {
    auto system = asset_man.get_particle_system(uuid);
    if (!system) {
      OX_LOG_ERROR("Particle system {} is not loaded.", uuid.str());
      return;
    }

    self.emitter_settings = system->emitter;
    self.render_settings = system->render;
    self.spawn_graph = system->spawn_graph;
    self.update_graph = system->update_graph;
    self.curves = system->curves;
    self.gradients = system->gradients;
    self.compile_error = system->compile_error;
  }

  if (auto asset = asset_man.get_asset(uuid)) {
    self.asset_path = asset->path;
  }

  self.asset_uuid = uuid;
  self.selected_node = ParticleNodeID::Invalid;
  self.spawn_positions_applied = false;
  self.update_positions_applied = false;
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
      system.spawn_graph = self.spawn_graph;
      system.update_graph = self.update_graph;
      system.curves = self.curves;
      system.gradients = self.gradients;
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

  self.preview_asset = self.asset_uuid;
  self.preview_emitter.set<ParticleSystemComponent>({
    .particle_system = self.asset_uuid,
    .play_on_awake = true,
  });
}

auto ParticleEditorPanel::on_update(this ParticleEditorPanel& self) -> void {
  ZoneScoped;

  // The preview scene ticks in `draw_preview`, immediately before its render.
}

auto ParticleEditorPanel::draw_canvas(this ParticleEditorPanel& self) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  auto* context = self.active_kind == ParticleProgramKind::Spawn ? self.spawn_context : self.update_context;
  auto& positions_applied = self.active_kind == ParticleProgramKind::Spawn ? self.spawn_positions_applied
                                                                           : self.update_positions_applied;
  auto& graph = self.active_graph();

  ed::SetCurrentEditor(context);
  ed::Begin("particle_graph_canvas");

  for (const auto& node : graph.nodes) {
    const auto& desc = particle_node_desc(node.type);
    const auto node_id = ed::NodeId(particle_node_editor_id(node.id));

    if (!positions_applied) {
      ed::SetNodePosition(node_id, ImVec2(node.canvas_position.x, node.canvas_position.y));
    }

    ed::BeginNode(node_id);
    ImGui::TextUnformatted(desc.name.data(), desc.name.data() + desc.name.size());
    const auto header_bottom = ImGui::GetItemRectMax().y;
    ImGui::Dummy(ImVec2(140.0f, 2.0f));

    for (auto pin = 0_u32; pin < desc.input_count; pin++) {
      ed::BeginPin(ed::PinId(particle_input_pin_id(node.id, pin)), ed::PinKind::Input);
      ImGui::TextUnformatted(stack.format_char("{} in {}", ICON_MDI_CIRCLE_SMALL, pin));
      ed::EndPin();
    }

    if (desc.has_output) {
      ImGui::Indent(90.0f);
      ed::BeginPin(ed::PinId(particle_output_pin_id(node.id)), ed::PinKind::Output);
      ImGui::TextUnformatted(stack.format_char("out {}", ICON_MDI_CIRCLE_SMALL));
      ed::EndPin();
      ImGui::Unindent(90.0f);
    }

    ed::EndNode();

    // Drawn after the node so the band can span its final width, and into the background list so it
    // sits under the title.
    if (ImGui::IsItemVisible()) {
      const auto node_min = ImGui::GetItemRectMin();
      const auto node_max = ImGui::GetItemRectMax();
      const auto& style = ed::GetStyle();
      const auto inset = style.NodeBorderWidth * 0.5f;

      if (auto* background = ed::GetNodeBackgroundDrawList(node_id)) {
        draw_particle_node_header(
          *background,
          {node_min.x + inset, node_min.y + inset},
          {node_max.x - inset, header_bottom + style.NodePadding.y * 0.5f},
          particle_node_header_color(desc.category),
          style.NodeRounding
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

  positions_applied = true;

  // Only a structural change needs the bytecode rebuilt; dragging a node is layout, not behaviour.
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

  if (const auto selected_count = ed::GetSelectedObjectCount(); selected_count > 0) {
    auto selected = std::vector<ed::NodeId>(static_cast<usize>(selected_count));
    const auto count = ed::GetSelectedNodes(selected.data(), selected_count);
    self.selected_node = count > 0 ? static_cast<ParticleNodeID>(selected[0].Get() - 1) : ParticleNodeID::Invalid;
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

    constexpr static std::string_view categories[] = {"Input", "Attribute", "Math", "Vector", "Output"};
    for (const auto category : categories) {
      if (!ImGui::BeginMenu(stack.null_terminate_cstr(category))) {
        continue;
      }

      for (auto type = 0_u32; type < static_cast<u32>(ParticleNodeType::Count); type++) {
        const auto& desc = particle_node_desc(static_cast<ParticleNodeType>(type));
        if (desc.category != category) {
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

  if (ImGui::CollapsingHeader("Selected Node", ImGuiTreeNodeFlags_DefaultOpen)) {
    auto& graph = self.active_graph();
    auto* node = const_cast<ParticleNode*>(graph.find_node(self.selected_node));
    if (!node) {
      ImGui::TextUnformatted("No node selected.");
    } else {
      const auto& desc = particle_node_desc(node->type);
      ImGui::TextUnformatted(desc.name.data(), desc.name.data() + desc.name.size());

      // An output node with nothing plugged in still writes -- and writes its literal, every frame,
      // over whatever the spawn program set. Worth saying out loud.
      if (is_particle_output_node(node->type)) {
        const auto connected = std::ranges::any_of(graph.links, [node](const ParticleLink& link) {
          return link.to_node == node->id && link.to_pin == 0;
        });

        if (!connected) {
          ImGui::TextColored(
            ImVec4(0.95f, 0.75f, 0.30f, 1.0f),
            "Nothing connected: writes the value below\nover this attribute every frame."
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
          nullptr,
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
        if (UI::property("Curve", &index, names.data(), static_cast<i32>(names.size()))) {
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
        if (UI::property("Gradient", &index, names.data(), static_cast<i32>(names.size()))) {
          node->index = static_cast<u32>(index);
          modified = true;
        }
      }

      if (node->type == ParticleNodeType::Random) {
        auto stream = static_cast<i32>(node->index);
        if (UI::property("Stream", &stream, 0, 63)) {
          node->index = static_cast<u32>(std::max(stream, 0));
          modified = true;
        }
      }
      UI::end_properties();
    }
  }

  if (ImGui::CollapsingHeader("Emitter", ImGuiTreeNodeFlags_DefaultOpen)) {
    auto& settings = self.emitter_settings;

    UI::begin_properties();
    modified |= UI::property("Capacity", &settings.capacity, 1_u32, 1u << 20);
    modified |= UI::property("Spawn Rate", &settings.spawn_rate, 0.0f, 10000.0f);
    modified |= UI::property("Duration", &settings.duration, 0.0f, 1000.0f);
    modified |= UI::property("Start Delay", &settings.start_delay, 0.0f, 1000.0f);
    modified |= UI::property("Looping", &settings.looping);
    modified |= UI::property_vector("Lifetime", settings.lifetime, false, true, nullptr, 0.05f, 0.0f, 100.0f);

    static const c8* shape_names[] = {"Point", "Sphere", "Hemisphere", "Box", "Circle", "Cone"};
    auto shape = static_cast<i32>(settings.shape);
    if (UI::property("Shape", &shape, shape_names, static_cast<i32>(std::size(shape_names)))) {
      settings.shape = static_cast<ParticleEmissionShape>(shape);
      modified = true;
    }
    modified |= UI::property_vector("Shape Size", settings.shape_size, false, true, nullptr, 0.05f, 0.0f, 100.0f);
    modified |= UI::property("Cone Angle", &settings.shape_angle, 0.0f, 89.0f);

    static const c8* space_names[] = {"World", "Local"};
    auto space = static_cast<i32>(settings.simulation_space);
    if (UI::property("Simulation Space", &space, space_names, static_cast<i32>(std::size(space_names)))) {
      settings.simulation_space = static_cast<ParticleSimulationSpace>(space);
      modified = true;
    }
    modified |= UI::property("Seed", &settings.seed);
    UI::end_properties();

    ImGui::TextUnformatted("Bursts");
    for (usize i = 0; i < settings.bursts.size(); i++) {
      ImGui::PushID(static_cast<i32>(i));
      UI::begin_properties();
      modified |= UI::property("Time", &settings.bursts[i].time, 0.0f, 1000.0f);
      modified |= UI::property("Count", &settings.bursts[i].count, 0_u32, 100000_u32);
      modified |= UI::property("Cycles", &settings.bursts[i].cycles, 0_u32, 10000_u32);
      modified |= UI::property("Interval", &settings.bursts[i].interval, 0.0f, 1000.0f);
      UI::end_properties();
      if (UI::button("Remove Burst")) {
        settings.bursts.erase(settings.bursts.begin() + static_cast<std::ptrdiff_t>(i));
        modified = true;
        ImGui::PopID();
        break;
      }
      ImGui::PopID();
    }

    if (UI::button("Add Burst")) {
      settings.bursts.emplace_back();
      modified = true;
    }
  }

  if (ImGui::CollapsingHeader("Rendering", ImGuiTreeNodeFlags_DefaultOpen)) {
    auto& settings = self.render_settings;

    UI::begin_properties();
    static const c8* mode_names[] = {"Billboard", "Mesh"};
    auto mode = static_cast<i32>(settings.render_mode);
    if (UI::property("Render Mode", &mode, mode_names, static_cast<i32>(std::size(mode_names)))) {
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
    if (UI::property("Billboard", &billboard, billboard_names, static_cast<i32>(std::size(billboard_names)))) {
      settings.billboard = static_cast<ParticleBillboardMode>(billboard);
      modified = true;
    }

    static const c8* blend_names[] = {"Alpha Blend", "Additive"};
    auto blend = static_cast<i32>(settings.blend);
    if (UI::property("Blend", &blend, blend_names, static_cast<i32>(std::size(blend_names)))) {
      settings.blend = static_cast<ParticleBlendMode>(blend);
      modified = true;
    }

    modified |= UI::property_vector("Flipbook", settings.flipbook, false, true, nullptr, 1.0f, 1.0f, 32.0f);
    modified |= UI::property("Soft Particle Distance", &settings.soft_particle_distance, 0.0f, 100.0f);
    modified |= UI::property("Velocity Stretch", &settings.velocity_stretch, 0.0f, 10.0f);
    modified |= UI::property("Restitution", &settings.restitution, 0.0f, 1.0f);
    modified |= UI::property("Sort", &settings.sort);
    modified |= UI::property("Depth Collision", &settings.depth_collision);
    UI::end_properties();

    const auto asset_slot = [&asset_man, &modified](const c8* label, UUID& uuid) {
      auto text = uuid ? uuid.str() : std::string("None");
      const auto clear_width = ImGui::GetFrameHeight();

      ImGui::TextUnformatted(label);
      ImGui::PushID(label);
      ImGui::Button(text.c_str(), ImVec2(-(clear_width + ImGui::GetStyle().ItemSpacing.x), 0.0f));
      if (ImGui::BeginDragDropTarget()) {
        if (const auto* payload = ImGui::AcceptDragDropPayload(PayloadData::DRAG_DROP_SOURCE)) {
          const auto* data = PayloadData::from_payload(payload);
          if (!data->get_str().empty()) {
            if (const auto imported = asset_man.import_asset(data->str)) {
              if (asset_man.load_asset(imported)) {
                if (uuid) {
                  asset_man.unload_asset(uuid);
                }
                uuid = imported;
                modified = true;
              }
            }
          }
        }
        ImGui::EndDragDropTarget();
      }

      ImGui::SameLine();
      ImGui::BeginDisabled(!uuid);
      if (UI::button(ICON_MDI_TRASH_CAN, ImVec2(clear_width, 0.0f), "Detach")) {
        asset_man.unload_asset(uuid);
        uuid = UUID(nullptr);
        modified = true;
      }
      ImGui::EndDisabled();
      ImGui::PopID();
    };

    asset_slot("Material", settings.material);
    if (settings.render_mode == ParticleRenderMode::Mesh) {
      asset_slot("Mesh", settings.mesh);
    }
  }

  if (ImGui::CollapsingHeader("Curves")) {
    const auto row_button_width = ImGui::GetFrameHeight();

    for (usize i = 0; i < self.curves.size(); i++) {
      ImGui::PushID(static_cast<i32>(i));
      auto& curve = self.curves[i];

      UI::begin_properties();
      modified |= UI::input_text("Name", &curve.name);
      UI::end_properties();

      modified |= self.draw_curve_editor(i, curve);

      auto removed_point = self.curves[i].points.size();
      if (ImGui::TreeNode("Points")) {
        for (usize point = 0; point < curve.points.size(); point++) {
          ImGui::PushID(static_cast<i32>(point));
          const auto field_width = (ImGui::GetContentRegionAvail().x - row_button_width -
                                    ImGui::GetStyle().ItemSpacing.x * 2.0f) *
                                   0.5f;
          ImGui::SetNextItemWidth(field_width);
          modified |= ImGui::DragFloat("##t", &curve.points[point].x, 0.01f, 0.0f, 1.0f);
          ImGui::SameLine();
          ImGui::SetNextItemWidth(field_width);
          modified |= ImGui::DragFloat(
            "##value",
            &curve.points[point].y,
            0.01f,
            -PARTICLE_LITERAL_RANGE,
            PARTICLE_LITERAL_RANGE
          );
          ImGui::SameLine();
          if (UI::button(ICON_MDI_TRASH_CAN, ImVec2(row_button_width, 0.0f), "Remove point")) {
            removed_point = point;
          }
          ImGui::PopID();
        }

        if (UI::button("Add Point")) {
          curve.points.emplace_back(1.0f, 1.0f);
          modified = true;
        }

        ImGui::TreePop();
      }

      if (removed_point < curve.points.size()) {
        curve.points.erase(curve.points.begin() + static_cast<std::ptrdiff_t>(removed_point));
        modified = true;
      }

      const auto remove_curve = UI::button(stack.format_char("{} Remove Curve", ICON_MDI_TRASH_CAN));
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

    if (UI::button("Add Curve")) {
      self.curves.emplace_back();
      modified = true;
    }
  }

  if (ImGui::CollapsingHeader("Gradients")) {
    const auto row_button_width = ImGui::GetFrameHeight();

    for (usize i = 0; i < self.gradients.size(); i++) {
      ImGui::PushID(static_cast<i32>(i));
      auto& gradient = self.gradients[i];

      UI::begin_properties();
      modified |= UI::input_text("Name", &gradient.name);
      UI::end_properties();

      auto removed_key = gradient.keys.size();
      for (usize key = 0; key < gradient.keys.size(); key++) {
        ImGui::PushID(static_cast<i32>(key));
        const auto field_width = (ImGui::GetContentRegionAvail().x - row_button_width -
                                  ImGui::GetStyle().ItemSpacing.x * 2.0f) *
                                 0.5f;
        ImGui::SetNextItemWidth(field_width);
        modified |= ImGui::DragFloat("##time", &gradient.keys[key].t, 0.01f, 0.0f, 1.0f);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(field_width);
        modified |= ImGui::ColorEdit4("##color", glm::value_ptr(gradient.keys[key].color));
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

      if (UI::button("Add Key")) {
        gradient.keys.emplace_back();
        modified = true;
      }
      ImGui::SameLine();
      const auto remove_gradient = UI::button(stack.format_char("{} Remove Gradient", ICON_MDI_TRASH_CAN));
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

    if (UI::button("Add Gradient")) {
      self.gradients.emplace_back();
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

  constexpr auto plot_height = 140.0f;
  constexpr auto grab_radius = 7.0f;
  constexpr auto point_radius = 4.0f;

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
    line_spec.LineWeight = 2.0f;

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
  }

  return modified;
}

auto ParticleEditorPanel::draw_preview(this ParticleEditorPanel& self, const vuk::ImageAttachment& swapchain_attachment)
  -> void {
  ZoneScoped;

  self.ensure_preview_scene();

  const auto available = ImGui::GetContentRegionAvail();
  self.preview_size = {std::max(available.x, 32.0f), std::max(available.y - 64.0f, 32.0f)};

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
  // full update every frame -- `RendererInstance::render` asserts that `update` ran this frame.
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
  ImGui::SetNextItemWidth(120.0f);
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
    App::mod<AssetManager>().export_asset(self.asset_uuid, self.asset_path);
  }
  ImGui::SameLine();

  if (ImGui::RadioButton("Spawn", self.active_kind == ParticleProgramKind::Spawn)) {
    self.active_kind = ParticleProgramKind::Spawn;
    self.selected_node = ParticleNodeID::Invalid;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("Update", self.active_kind == ParticleProgramKind::Update)) {
    self.active_kind = ParticleProgramKind::Update;
    self.selected_node = ParticleNodeID::Invalid;
  }

  if (!self.compile_error.empty()) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%s", self.compile_error.c_str());
  }

  const auto region = ImGui::GetContentRegionAvail();
  constexpr auto splitter_width = 6.0f;
  constexpr auto min_column_width = 200.0f;

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

  ImGui::BeginChild("particle_canvas", ImVec2(canvas_width, 0.0f), ImGuiChildFlags_Borders);
  self.draw_canvas();
  ImGui::EndChild();

  drag_splitter("##canvas_splitter", self.inspector_width);
  ImGui::BeginChild("particle_inspector", ImVec2(self.inspector_width, 0.0f), ImGuiChildFlags_Borders);
  self.draw_inspector();
  ImGui::EndChild();

  drag_splitter("##inspector_splitter", self.preview_width);
  ImGui::BeginChild(
    "particle_preview",
    ImVec2(self.preview_width, 0.0f),
    ImGuiChildFlags_Borders,
    ImGuiWindowFlags_NoScrollWithMouse
  );
  self.draw_preview(swapchain_attachment);
  ImGui::EndChild();

  self.on_end();
}
} // namespace ox
