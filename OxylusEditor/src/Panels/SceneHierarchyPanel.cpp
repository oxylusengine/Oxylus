#include "SceneHierarchyPanel.hpp"

#include <algorithm>
#include <array>
#include <glm/geometric.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/trigonometric.hpp>
#include <icons/IconsMaterialDesignIcons.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>
#include <span>
#include <string_view>

#include "Asset/AssetImporter.hpp"
#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Core/VFS.hpp"
#include "Editor.hpp"
#include "Memory/Stack.hpp"
#include "Render/DebugRenderer.hpp"
#include "Scene/ComponentRegistry.hpp"
#ifdef OX_DOTNET
  #include "Scripting/DotNetScripting.hpp"
#endif
#include "UI/PayloadData.hpp"
#include "UI/UI.hpp"
#include "Utils/ImGuiScoped.hpp"

namespace ox {
static constexpr auto COMPONENT_SEARCH_PREFIX = std::string_view("component:");

struct ComponentSearchAlias {
  const c8* name;
  const c8* description;
  bool (*matches)(flecs::entity);
};

template <auto Member, auto Value>
static auto matches_component_value(flecs::entity entity) -> bool {
  const auto* component = entity.try_get<refl::member_owner_t<Member>>();
  return component && (component->*Member) == Value;
}

static constexpr auto COMPONENT_SEARCH_ALIASES = std::to_array<ComponentSearchAlias>({
  {"DirectionalLightComponent",
   "Lights with type Directional",
   matches_component_value<&LightComponent::type, LightComponent::Directional>},
  {"SpotLightComponent", "Lights with type Spot", matches_component_value<&LightComponent::type, LightComponent::Spot>},
  {"PointLightComponent",
   "Lights with type Point",
   matches_component_value<&LightComponent::type, LightComponent::Point>},
  {"PerspectiveCameraComponent",
   "Cameras with perspective projection",
   matches_component_value<&CameraComponent::projection, CameraComponent::Perspective>},
  {"OrthographicCameraComponent",
   "Cameras with orthographic projection",
   matches_component_value<&CameraComponent::projection, CameraComponent::Orthographic>},
  {"StaticRigidBodyComponent",
   "Rigid bodies with type Static",
   matches_component_value<&RigidBodyComponent::type, RigidBodyComponent::Static>},
  {"KinematicRigidBodyComponent",
   "Rigid bodies with type Kinematic",
   matches_component_value<&RigidBodyComponent::type, RigidBodyComponent::Kinematic>},
  {"DynamicRigidBodyComponent",
   "Rigid bodies with type Dynamic",
   matches_component_value<&RigidBodyComponent::type, RigidBodyComponent::Dynamic>},
  {"SensorRigidBodyComponent",
   "Rigid bodies with Is Sensor enabled",
   matches_component_value<&RigidBodyComponent::is_sensor, true>},
  {"ConvexMeshColliderComponent",
   "Mesh colliders configured to use a convex hull",
   matches_component_value<&MeshColliderComponent::convex, true>},
  {"TriangleMeshColliderComponent",
   "Mesh colliders configured to use triangles",
   matches_component_value<&MeshColliderComponent::convex, false>},
  {"AudioSource3DComponent",
   "Audio sources with spatialization enabled",
   matches_component_value<&AudioSourceComponent::spatialization, true>},
  {"AudioSource2DComponent",
   "Audio sources with spatialization disabled",
   matches_component_value<&AudioSourceComponent::spatialization, false>},
  {"FrontWheelDriveVehicleComponent",
   "Vehicles with front-wheel drive",
   matches_component_value<&VehicleComponent::drive_mode, VehicleComponent::FrontWheelDrive>},
  {"RearWheelDriveVehicleComponent",
   "Vehicles with rear-wheel drive",
   matches_component_value<&VehicleComponent::drive_mode, VehicleComponent::RearWheelDrive>},
  {"AllWheelDriveVehicleComponent",
   "Vehicles with all-wheel drive",
   matches_component_value<&VehicleComponent::drive_mode, VehicleComponent::AllWheelDrive>},
});

static auto draw_component_search_help(f32 button_size) -> void {
  UI::button(ICON_MDI_HELP_CIRCLE_OUTLINE "###HierarchySearchHelp", {button_size, button_size});
  if (!ImGui::BeginItemTooltip()) {
    return;
  }

  ImGui::TextUnformatted("Plain text matches entity names. Component queries accept partial names and ignore case.");
  ImGui::TextUnformatted("Aliases match a setting on an existing component.");

  constexpr auto table_flags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings;
  if (ImGui::BeginTable("ComponentSearchExamples", 2, table_flags)) {
    ImGui::TableSetupColumn("Query");
    ImGui::TableSetupColumn("Matches");
    ImGui::TableHeadersRow();

    const auto example = [](const c8* name, const c8* description) -> void {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("%s%s", COMPONENT_SEARCH_PREFIX.data(), name);
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(description);
    };
    example("CameraComponent", "Any camera");
    example("LightComponent", "Any light type");
    example("Collider", "Any collider component");
    for (const auto& alias : COMPONENT_SEARCH_ALIASES) {
      example(alias.name, alias.description);
    }
    ImGui::EndTable();
  }
  ImGui::EndTooltip();
}

static auto component_search_term(std::string_view query) -> option<std::string_view> {
  constexpr auto whitespace = std::string_view(" \t");
  const auto first = query.find_first_not_of(whitespace);
  if (first == std::string_view::npos) {
    return nullopt;
  }

  query.remove_prefix(first);
  if (
    query.size() < COMPONENT_SEARCH_PREFIX.size() ||
    ImStrnicmp(query.data(), COMPONENT_SEARCH_PREFIX.data(), COMPONENT_SEARCH_PREFIX.size()) != 0
  ) {
    return nullopt;
  }

  query.remove_prefix(COMPONENT_SEARCH_PREFIX.size());
  const auto name_start = query.find_first_not_of(whitespace);
  if (name_start == std::string_view::npos) {
    return std::string_view{};
  }
  return query.substr(name_start, query.find_last_not_of(whitespace) - name_start + 1);
}

struct ComponentSearch {
  std::span<const flecs::id> components = {};
  std::span<const ComponentSearchAlias* const> aliases = {};

  auto matches(this const ComponentSearch& self, flecs::entity entity) -> bool {
    return std::ranges::any_of(self.components, [entity](flecs::id component) { return entity.has(component); }) ||
           std::ranges::any_of(self.aliases, [entity](const auto* alias) { return alias->matches(entity); });
  }
};

static auto resolve_component_search(
  std::span<const flecs::id> registered_components, std::string_view term, memory::ScopedStack& stack
) -> ComponentSearch {
  if (term.empty()) {
    return {};
  }

  const auto matches_name = [term](const c8* name) {
    return name && ImStristr(name, nullptr, term.data(), term.data() + term.size()) != nullptr;
  };

  auto ids = stack.alloc<flecs::id>(registered_components.size());
  usize count = 0;
  for (const auto component : registered_components) {
    if (matches_name(component.entity().name().c_str())) {
      ids[count++] = component;
    }
  }

  auto aliases = stack.alloc<const ComponentSearchAlias*>(COMPONENT_SEARCH_ALIASES.size());
  usize alias_count = 0;
  for (const auto& alias : COMPONENT_SEARCH_ALIASES) {
    if (matches_name(alias.name)) {
      aliases[alias_count++] = &alias;
    }
  }
  return {.components = ids.first(count), .aliases = aliases.first(alias_count)};
}

static auto has_mesh_in_hierarchy(flecs::entity entity) -> bool {
  if (entity.has<MeshComponent>()) {
    return true;
  }

  auto found = false;
  entity.children([&found](flecs::entity child) { found = found || has_mesh_in_hierarchy(child); });
  return found;
}

static auto add_mesh_colliders_to_hierarchy(flecs::entity entity) -> void {
  if (entity.has<MeshComponent>() && !entity.has<MeshColliderComponent>()) {
    entity.add<MeshColliderComponent>();
  }

  entity.children([](flecs::entity child) { add_mesh_colliders_to_hierarchy(child); });
}

static auto is_script_selected(const EditorContext& context) -> bool {
  return context.type == EditorContext::Type::Script && context.str.has_value();
}

static auto remove_selected_script(Scene& scene, EditorContext& context) -> void {
  scene.remove_script(ScriptKey{.type_name = context.str.value()});
  context.reset();
}

auto SceneHierarchyPanel::SelectedEntity::set(this SelectedEntity& self, flecs::entity e) -> void {
  self.entity = e;
  App::mod<Editor>().get_context().reset(EditorContext::Type::Entity, nullopt, e);
}

auto SceneHierarchyPanel::SelectedEntity::get(this SelectedEntity& self) -> flecs::entity {
  if (!self.entity.is_alive()) {
    self.entity = flecs::entity::null();
  }

  return self.entity;
}

auto SceneHierarchyPanel::SelectedEntity::reset(this SelectedEntity& self) -> void {
  self.entity = flecs::entity::null();
  App::mod<Editor>().get_context().reset();
}

SceneHierarchyPanel::SceneHierarchyPanel() : EditorPanelState("Scene Hierarchy", ICON_MDI_VIEW_LIST, true) {}

auto SceneHierarchyPanel::on_update(this SceneHierarchyPanel& self) -> void {
  auto& editor = App::mod<Editor>();
  auto& editor_context = editor.get_context();
  auto& undo_redo_system = editor.undo_redo_system;

  if (editor_context.type == EditorContext::Type::Entity) {
    if (editor_context.entity.has_value())
      self.selected_entity_.set(editor_context.entity.value());
  } else {
    self.selected_entity_.entity = flecs::entity::null();
  }

  const auto selected = self.selected_entity_.get();
  if (selected != flecs::entity::null()) {
    if (auto* cam = selected.try_get<CameraComponent>(); cam && self.scene_) {
      const auto proj = cam->get_projection_matrix() * cam->get_view_matrix();
      self.scene_->debug_renderer.draw_frustum(proj, glm::vec4(0, 1, 0, 1), cam->near_clip, cam->far_clip);
    }
    if (auto* light = selected.try_get<LightComponent>(); light && self.scene_ && selected.has<TransformComponent>()) {
      constexpr auto color = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
      if (light->type == LightComponent::Point) {
        self.scene_->debug_renderer.draw_sphere(light->radius, Scene::get_world_position(selected), color);
      } else if (light->type == LightComponent::Spot && light->radius > 0.0f) {
        const auto world_transform = Scene::get_world_transform(selected);
        const auto forward = -glm::vec3(world_transform[2]);
        if (glm::dot(forward, forward) > 0.0f) {
          const auto direction = glm::normalize(forward);
          const auto rotation = glm::rotation(glm::vec3(0.0f, 0.0f, 1.0f), direction);
          const auto position = glm::vec3(world_transform[3]);
          auto& debug_renderer = self.scene_->debug_renderer;

          const auto draw_cutoff = [&](f32 cutoff, const glm::vec4& cone_color) {
            // use the shader's cosine cutoff and end the rim at the light's radial range
            const auto half_angle = glm::acos(glm::cos(cutoff));
            if (half_angle == 0.0f) {
              debug_renderer.draw_line(position, position + direction * light->radius, 1.0f, cone_color);
              return;
            }

            const auto length = light->radius * glm::cos(half_angle);
            debug_renderer.draw_cone(32, 4, 2.0f * half_angle, length, position, rotation, cone_color);
          };
          draw_cutoff(light->outer_cone_angle, color);
          draw_cutoff(light->inner_cone_angle, glm::vec4(1.0f, 0.85f, 0.0f, 1.0f));
        }
      }
    }

    if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) && ImGui::IsKeyPressed(ImGuiKey_D) && self.scene_) {
      self.selected_entity_.set(self.scene_->duplicate_entity(self.selected_entity_.get()));
    }
    if (
      ImGui::IsKeyPressed(ImGuiKey_Delete) && (self.table_hovered_ || editor.main_viewport_panel.get_focused_viewport())
    ) {
      self.deleted_entity_ = self.selected_entity_.get();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F2)) {
      self.renaming_entity_ = self.selected_entity_.get();
    }
  }

  if (
    self.scene_ && is_script_selected(editor_context) && ImGui::IsKeyPressed(ImGuiKey_Delete) &&
    (self.table_hovered_ || self.table_hovered_scripts)
  ) {
    remove_selected_script(*self.scene_, editor_context);
  }

  if (self.deleted_entity_) {
    auto command_id = fmt::format("delete entity {}", self.deleted_entity_.name().c_str());
    undo_redo_system->execute_command<EntityDeleteCommand>(self.scene_, self.deleted_entity_, "", command_id);
    self.selected_entity_.reset();
  }
}

auto SceneHierarchyPanel::on_render(this SceneHierarchyPanel& self, const SwapchainInfo& swapchain_info) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  ImGuiScoped::StyleVar cellpad(ImGuiStyleVar_CellPadding, {0, 0});

  if (ImGui::Begin(self.id.c_str(), &self.visible, ImGuiWindowFlags_NoCollapse)) {
    if (!self.scene_) {
      const auto warning_text = "No scene!";
      const auto text_width = ImGui::CalcTextSize(warning_text).x;
      ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - text_width) * 0.5f);
      ImGui::SetCursorPosY(ImGui::GetContentRegionAvail().y * 0.5f);
      ImGui::Text(warning_text);

      ImGui::End();
      return;
    }

    constexpr ImGuiTableFlags table_flags = ImGuiTableFlags_ContextMenuInBody | ImGuiTableFlags_BordersInner |
                                            ImGuiTableFlags_ScrollY;
    const float line_height = ImGui::GetTextLineHeight();

    float filter_cursor_pos_x = ImGui::GetCursorPosX();
    if (ImGui::TreeNodeEx("Scripts", ImGuiTreeNodeFlags_Framed)) {
      self.scripts_filter_.Draw(
        "###HierarchyFilter",
        ImGui::GetContentRegionAvail().x - (ImGui::CalcTextSize(ICON_MDI_PLUS).x + UI::scale(20.0f))
      );
      ImGui::SameLine();

      if (UI::button(stack.format_char("{}###AddScript", ICON_MDI_PLUS)))
        ImGui::OpenPopup("scene_h_scripts_context_window");

      if (
        ImGui::BeginPopupContextWindow(
          "scene_h_scripts_context_window",
          ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems
        )
      ) {
        self.draw_scripts_context_menu();
        ImGui::EndPopup();
      }

      if (!self.scripts_filter_.IsActive()) {
        ImGui::SameLine();
        ImGui::SetCursorPosX(filter_cursor_pos_x + ImGui::GetFontSize() * 0.5f);
        auto search_txt = fmt::format("  {} Search scripts...", ICON_MDI_MAGNIFY);
        ImGui::TextUnformatted(search_txt.c_str());
      }

      if (ImGui::BeginTable("ScriptsTable", 1, table_flags, ImVec2(0.0f, UI::scale(100.0f)))) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_NoClip);

        auto& editor_context = App::mod<Editor>().get_context();

        for (const auto& [key, script] : self.scene_->get_scripts()) {
          auto system_name = script->display_name();
          if (self.scripts_filter_.IsActive() && !self.scripts_filter_.PassFilter(system_name.c_str())) {
            continue;
          }

          ImGui::TableNextRow();

          ImGui::TableNextColumn();

          const auto is_selected = is_script_selected(editor_context) && editor_context.str.value() == key.type_name;

          ImGuiTreeNodeFlags flags = (is_selected ? ImGuiTreeNodeFlags_Selected : 0);
          flags |= ImGuiTreeNodeFlags_OpenOnArrow;
          flags |= ImGuiTreeNodeFlags_SpanFullWidth;
          flags |= ImGuiTreeNodeFlags_FramePadding;
          flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

          const bool highlight = is_selected;
          if (highlight) {
            ImVec4 active_color = ImGui::GetStyleColorVec4(ImGuiCol_Tab);
            ImVec4 hovered_color = ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered);
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(active_color));
            ImGui::PushStyleColor(ImGuiCol_Header, active_color);
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, is_selected ? active_color : hovered_color);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
          }

          ImGui::TreeNodeEx(script.get(), flags, "%s %s", ICON_MDI_SCRIPT, system_name.c_str());

          if (highlight)
            ImGui::PopStyleColor(3);

          // Select
          if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            self.selected_entity_.reset();
            editor_context.reset(EditorContext::Type::Script, key.type_name);
          }
        }

        if (
          ImGui::BeginPopupContextWindow(
            "scene_h_scripts_context_window",
            ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems
          )
        ) {
          self.selected_entity_.reset();
          self.draw_scripts_context_menu();
          ImGui::EndPopup();
        }

        ImGui::EndTable();

        if (ImGui::BeginPopupContextItem()) {
          if (ImGui::MenuItem("Remove", "Del")) {
            if (is_script_selected(editor_context)) {
              remove_selected_script(*self.scene_, editor_context);
            }
          }

          ImGui::EndPopup();
        }

        self.table_hovered_scripts = ImGui::IsItemHovered();

        if (ImGui::IsItemClicked()) {
          self.selected_entity_.reset();
        }
      }
      ImGui::TreePop();
    }

    if (ImGui::TreeNodeEx("Entities", ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_DefaultOpen)) {
      const auto button_size = ImGui::GetFrameHeight();
      const auto buttons_width = 2.0f * (button_size + ImGui::GetStyle().ItemSpacing.x);
      ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x - buttons_width));
      if (
        ImGui::InputTextWithHint(
          "###HierarchyFilter",
          ICON_MDI_MAGNIFY " Search name or component:...",
          self.entities_filter_.InputBuf,
          sizeof(self.entities_filter_.InputBuf)
        )
      ) {
        self.entities_filter_.Build();
      }
      ImGui::SameLine();
      draw_component_search_help(button_size);
      ImGui::SameLine();

      if (UI::button(ICON_MDI_PLUS "###AddEntity", {button_size, button_size}))
        ImGui::OpenPopup("scene_h_entities_context_window");

      if (
        ImGui::BeginPopupContextWindow(
          "scene_h_entities_context_window",
          ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems
        )
      ) {
        self.draw_entities_context_menu();
        ImGui::EndPopup();
      }

      const ImVec2 cursor_pos = ImGui::GetCursorPos();
      const ImVec2 region = ImGui::GetContentRegionAvail();
      if (region.x != 0.0f && region.y != 0.0f) {
        ImGui::InvisibleButton("##DragDropTargetBehindTable", region);
        if (ImGui::BeginDragDropTarget()) {
          if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(PayloadData::DRAG_DROP_SOURCE)) {
            const auto* payload_data = PayloadData::from_payload(payload);
            const auto path = payload_data->get_path();
            if (path.extension() == ".oxparticle") {
              auto asset = import_asset(App::mod<AssetManager>(), path);
              if (asset) {
                auto new_entity = self.scene_->create_particle_system_entity(asset);
                if (new_entity != flecs::entity::null()) {
                  self.selected_entity_.set(new_entity);
                }
              }
            }
          }
          ImGui::EndDragDropTarget();
        }
      }

      ImGui::SetCursorPos(cursor_pos);
      if (ImGui::BeginTable("HierarchyTable", 3, table_flags)) {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_NoClip);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, line_height * 3.0f);

        const auto vis_colon = fmt::format("  {}", ICON_MDI_EYE_OUTLINE);
        ImGui::TableSetupColumn(vis_colon.c_str(), ImGuiTableColumnFlags_WidthFixed, line_height * 2.0f);

        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        const auto
          entities = self.scene_->world.query_builder<TransformComponent>().with(flecs::Disabled).optional().build();
        if (const auto term = component_search_term(self.entities_filter_.InputBuf)) {
          const auto search = resolve_component_search(self.scene_->component_db.get_components(), *term, stack);
          bool found_match = false;
          const auto draw_matches = [&](this const auto& visit, flecs::entity entity) -> void {
            if (entity.has<Hidden>()) {
              return;
            }
            if (search.matches(entity)) {
              ImGuiScoped::ID result_id(reinterpret_cast<const void*>(entity.raw_id()));
              self.draw_entity_node(entity, EntityView::SearchResult);
              found_match = true;
            }
            entity.children([&visit](flecs::entity child) { visit(child); });
          };
          if (!search.components.empty() || !search.aliases.empty()) {
            entities.each([&draw_matches](flecs::entity entity, TransformComponent) {
              if (entity.parent() == flecs::entity::null()) {
                draw_matches(entity);
              }
            });
          }
          if (!found_match) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", term->empty() ? "Enter a component name" : "No matching entities");
          }
        } else {
          entities.each([&self](flecs::entity entity, TransformComponent) {
            if (entity.parent() == flecs::entity::null()) {
              self.draw_entity_node(entity);
            }
          });
        }
        ImGui::PopStyleVar();

        if (
          ImGui::BeginPopupContextWindow(
            "scene_h_entities_context_window",
            ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems
          )
        ) {
          self.selected_entity_.reset();
          self.draw_entities_context_menu();
          ImGui::EndPopup();
        }

        ImGui::EndTable();

        self.table_hovered_ = ImGui::IsItemHovered();

        if (ImGui::IsItemClicked()) {
          self.selected_entity_.reset();
        }
      }

      if (ImGui::IsMouseDown(0) && self.window_hovered_) {
        self.selected_entity_.reset();
      }

      if (self.dragged_entity_ != flecs::entity::null() && self.dragged_entity_target_ != flecs::entity::null()) {
        self.dragged_entity_.child_of(self.dragged_entity_target_);
        self.dragged_entity_ = flecs::entity::null();
        self.dragged_entity_target_ = flecs::entity::null();
      }

      ImGui::TreePop();
    }

    self.window_hovered_ = ImGui::IsWindowHovered();
  }

  ImGui::End();
}

auto SceneHierarchyPanel::draw_entity_node(
  this SceneHierarchyPanel& self,
  flecs::entity entity,
  EntityView view,
  u32 depth,
  bool force_expand_tree,
  bool is_part_of_prefab
) -> ImRect {
  ZoneScoped;

  if (entity.has<Hidden>())
    return {0, 0, 0, 0};

  ImGui::TableNextRow();
  ImGui::TableNextColumn();

  const auto is_search_result = view == EntityView::SearchResult;
  const auto child_count = is_search_result ? 0 : self.scene_->world.count(flecs::ChildOf, entity);

  if (
    !is_search_result && self.entities_filter_.IsActive() && !self.entities_filter_.PassFilter(entity.name().c_str())
  ) {
    entity.children([&self](flecs::entity child) { self.draw_entity_node(child); });
    return {0, 0, 0, 0};
  }

  const auto is_selected = self.selected_entity_.get().id() == entity.id();

  ImGuiTreeNodeFlags flags = (is_selected ? ImGuiTreeNodeFlags_Selected : 0);
  flags |= ImGuiTreeNodeFlags_OpenOnArrow;
  flags |= ImGuiTreeNodeFlags_SpanFullWidth;
  flags |= ImGuiTreeNodeFlags_FramePadding;

  if (child_count == 0) {
    flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
  }

  if (is_selected) {
    ImVec4 active_color = ImGui::GetStyleColorVec4(ImGuiCol_Tab);
    ImVec4 hovered_color = ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered);
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(active_color));
    ImGui::PushStyleColor(ImGuiCol_Header, active_color);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, is_selected ? active_color : hovered_color);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
  }

  if (force_expand_tree)
    ImGui::SetNextItemOpen(true);

  // const bool prefab_color_applied = is_part_of_prefab && !is_selected;
  // if (prefab_color_applied)
  //   ImGui::PushStyleColor(ImGuiCol_Text, header_selected_color);

  const bool opened = ImGui::TreeNodeEx(
    reinterpret_cast<void*>(entity.raw_id()),
    flags,
    "%s %s",
    ICON_MDI_CUBE_OUTLINE,
    entity.name().c_str()
  );

  if (is_selected)
    ImGui::PopStyleColor(3);

  // Select
  if (!ImGui::IsItemToggledOpen() && ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
    self.selected_entity_.set(entity);
  }

  if (is_search_result && ImGui::BeginItemTooltip()) {
    ImGui::TextUnformatted(entity.path().c_str());
    ImGui::EndTooltip();
  }

  // Expand recursively
  if (ImGui::IsItemToggledOpen() && (ImGui::IsKeyDown(ImGuiKey_LeftAlt) || ImGui::IsKeyDown(ImGuiKey_RightAlt)))
    force_expand_tree = opened;

  bool entity_deleted = false;

  if (ImGui::BeginPopupContextItem()) {
    if (ImGui::MenuItem("Rename", "F2"))
      self.renaming_entity_ = entity;
    if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
      self.selected_entity_.set(self.scene_->duplicate_entity(entity));
    }
    if (ImGui::MenuItem("Delete", "Del"))
      entity_deleted = true;

    if (has_mesh_in_hierarchy(entity) && ImGui::MenuItem("Add Mesh Colliders")) {
      // deferred so adding components doesn't move entities out from under the children() walk
      entity.world().defer([&entity] {
        add_mesh_colliders_to_hierarchy(entity);
        if (!entity.has<RigidBodyComponent>()) {
          entity.set<RigidBodyComponent>({.type = RigidBodyComponent::Static});
        }
      });
    }

    ImGui::Separator();

    self.draw_entities_context_menu();

    ImGui::EndPopup();
  }

  ImVec2 vertical_line_start = ImGui::GetCursorScreenPos();
  vertical_line_start.x -= 0.5f;
  vertical_line_start.y -= ImGui::GetFrameHeight() * 0.5f;

  // Drag Drop
  {
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* entity_payload = ImGui::AcceptDragDropPayload("Entity")) {
        self.dragged_entity_ = *static_cast<flecs::entity*>(entity_payload->Data);
        self.dragged_entity_target_ = entity;
      } else if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(PayloadData::DRAG_DROP_SOURCE)) {
        const auto* payload_data = PayloadData::from_payload(payload);
        const auto path = payload_data->get_path();
        if (path.extension() == ".oxparticle") {
          auto asset = import_asset(App::mod<AssetManager>(), path);
          if (asset) {
            auto new_entity = self.scene_->create_particle_system_entity(asset);
            if (new_entity != flecs::entity::null()) {
              new_entity.child_of(entity);
              self.selected_entity_.set(new_entity);
            }
          }
        }
      }

      ImGui::EndDragDropTarget();
    }

    if (ImGui::BeginDragDropSource()) {
      ImGui::SetDragDropPayload("Entity", &entity, sizeof(flecs::entity));
      ImGui::TextUnformatted(entity.name().c_str());
      ImGui::EndDragDropSource();
    }
  }

  if (entity.id() == self.renaming_entity_.id()) {
    static bool renaming = false;
    if (!renaming) {
      renaming = true;
      ImGui::SetKeyboardFocusHere();
    }

    std::string name{entity.name()};
    if (ImGui::InputText("##Tag", &name)) {
      entity.set_name(name.c_str());
    }

    if (ImGui::IsItemDeactivated()) {
      renaming = false;
      self.renaming_entity_ = flecs::entity::null();
    }
  }

  ImGui::TableNextColumn();

  ImGui::PushStyleColor(ImGuiCol_Button, {0, 0, 0, 0});
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0, 0, 0, 0});
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0, 0, 0, 0});

  const float button_size_x = ImGui::GetContentRegionAvail().x;
  const float frame_height = ImGui::GetFrameHeight();
  ImGui::PushID(entity.name());
  ImGui::Button(is_part_of_prefab ? "Prefab" : "Entity", {button_size_x, frame_height});
  ImGui::PopID();
  // Select
  if (ImGui::IsItemDeactivated() && ImGui::IsItemHovered() && !ImGui::IsItemToggledOpen()) {
    self.selected_entity_.set(entity);
  }

  ImGui::TableNextColumn();
  // Visibility Toggle
  {
    ImGui::Text("  %s", entity.enabled() ? ICON_MDI_EYE_OUTLINE : ICON_MDI_EYE_OFF_OUTLINE);

    if (ImGui::IsItemHovered() && (ImGui::IsMouseDragging(0) || ImGui::IsItemClicked())) {
      entity.enabled() ? entity.disable() : entity.enable();
    }
  }

  ImGui::PopStyleColor(3);

  // if (prefab_color_applied)
  //   ImGui::PopStyleColor();

  // Open
  const ImRect node_rect = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
  {
    if (opened && child_count > 0 && !entity_deleted) {
      ImColor tree_line_color;
      depth %= 4;
      switch (depth) {
        case 0 : tree_line_color = ImColor(254, 112, 246); break;
        case 1 : tree_line_color = ImColor(142, 112, 254); break;
        case 2 : tree_line_color = ImColor(112, 180, 254); break;
        case 3 : tree_line_color = ImColor(48, 134, 198); break;
        default: tree_line_color = ImColor(255, 255, 255); break;
      }

      entity.children([&self, depth, force_expand_tree, is_part_of_prefab, vertical_line_start, tree_line_color](
                        const flecs::entity child
                      ) {
        const float horizontal_tree_line_size = UI::scale(
          self.scene_->world.count(flecs::ChildOf, child) > 0 ? 9.0f : 18.0f
        );
        // chosen arbitrarily
        const ImRect child_rect = self.draw_entity_node(
          child,
          EntityView::Hierarchy,
          depth + 1,
          force_expand_tree,
          is_part_of_prefab
        );

        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        ImVec2 vertical_line_end = vertical_line_start;
        const float line_thickness = UI::scale(1.5f);
        const float midpoint = (child_rect.Min.y + child_rect.Max.y) / 2.0f;
        draw_list->AddLine(
          ImVec2(vertical_line_start.x, midpoint),
          ImVec2(vertical_line_start.x + horizontal_tree_line_size, midpoint),
          tree_line_color,
          line_thickness
        );
        vertical_line_end.y = midpoint;
        draw_list->AddLine(vertical_line_start, vertical_line_end, tree_line_color, line_thickness);
      });
    }

    if (opened && child_count > 0)
      ImGui::TreePop();
  }

  // PostProcess Actions
  if (entity_deleted)
    self.deleted_entity_ = entity;

  return node_rect;
}

auto SceneHierarchyPanel::draw_entities_context_menu(this SceneHierarchyPanel& self) -> void {
  ZoneScoped;

  const bool has_context = self.selected_entity_.get() != flecs::entity::null();

  flecs::entity to_select = flecs::entity::null();

  ImGuiScoped::StyleVar styleVar1(ImGuiStyleVar_ItemInnerSpacing, UI::scale(ImVec2(0.0f, 5.0f)));
  ImGuiScoped::StyleVar styleVar2(ImGuiStyleVar_ItemSpacing, UI::scale(ImVec2(1.0f, 5.0f)));
  if (ImGui::BeginMenu("Create")) {
    if (ImGui::MenuItem("New Entity")) {
      to_select = self.scene_->create_entity("entity", true);
    }

    if (ImGui::MenuItem("Sprite")) {
      to_select = self.scene_->create_entity("sprite", true).add<SpriteComponent>();
    }

    if (ImGui::MenuItem("Camera")) {
      to_select = self.scene_->create_entity("camera", true);
      to_select.add<CameraComponent>().get_mut<TransformComponent>().rotation.y = glm::radians(-90.f);
    }

    if (ImGui::MenuItem("Terrain")) {
      to_select = self.scene_->create_entity("terrain", true);
      to_select //
        .add<TransformComponent>()
        .set<TerrainComponent>({
          .domain_size = 0.7f,
          .height_frequency = 1.1f,
          .height_amplitude = 3.335f,
          .height_lacunarity = 0.4f,
          .height_octaves = 6,
          .seed = 246,
          .erosion_scale = 0.450f,
          .erosion_strength = 0.220f,
          .ridge_rounding = 0.2f,
          .crease_rounding = -0.2f,
          .erosion_octaves = 16,
        });
    }

    if (ImGui::MenuItem("Probe Volume")) {
      to_select = self.scene_->create_entity("probe_volume", true).add<ProbeVolumeComponent>();
    }

    if (ImGui::MenuItem("Decal")) {
      to_select = self.scene_->create_entity("decal", true).add<DecalComponent>();
      auto& decal_transform = to_select.get_mut<TransformComponent>();
      decal_transform.rotation = glm::angleAxis(-glm::half_pi<f32>(), glm::vec3(1.0f, 0.0f, 0.0f));
      decal_transform.scale = glm::vec3(2.0f, 2.0f, 1.0f);
    }

    if (ImGui::BeginMenu("Light")) {
      if (ImGui::MenuItem("Light")) {
        to_select = self.scene_->create_entity("light", true).add<LightComponent>();
      }
      if (ImGui::MenuItem("Sun")) {
        to_select = self.scene_->create_entity("sun", true)
                      .set<LightComponent>(LightComponent{.type = LightComponent::Directional, .intensity = 10.f})
                      .add<AtmosphereComponent>();
      }
      ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Audio")) {
      if (ImGui::MenuItem("Audio Source")) {
        to_select = self.scene_->create_entity("audio_source", true).add<AudioSourceComponent>();
        ImGui::CloseCurrentPopup();
      }
      if (ImGui::MenuItem("Audio Listener")) {
        to_select = self.scene_->create_entity("audio_listener", true).add<AudioListenerComponent>();
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Effects")) {
      if (ImGui::MenuItem("Particle System")) {
        to_select = self.scene_->create_entity("particle_system", true).add<ParticleSystemComponent>();
      }
      ImGui::EndMenu();
    }

    ImGui::EndMenu();
  }

  if (has_context && to_select != flecs::entity::null())
    to_select.child_of(self.selected_entity_.get());

  if (to_select != flecs::entity::null()) {
    self.selected_entity_.set(to_select);
  }
}

auto SceneHierarchyPanel::draw_scripts_context_menu(this SceneHierarchyPanel& self) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

#ifdef OX_DOTNET
  if (ImGui::BeginMenu(stack.format_char("{} Add C# script", ICON_MDI_LANGUAGE_CSHARP))) {
    auto is_any_listed = false;
    if (App::has_mod<DotNetScripting>()) {
      for (const auto& script_type : App::mod<DotNetScripting>().script_types) {
        if (script_type.kind != OxScriptKind::SceneBehaviour) {
          continue;
        }

        const auto is_in_scene = self.scene_->get_scripts().contains(ScriptKey{.type_name = script_type.name});
        if (is_in_scene) {
          continue;
        }

        is_any_listed = true;
        if (ImGui::MenuItem(script_type.name.c_str())) {
          if (auto result = self.scene_->add_script_type(script_type.name); !result) {
            OX_LOG_ERROR("{}", result.error());
          }
        }
      }
    }

    if (!is_any_listed) {
      ImGui::BeginDisabled();
      ImGui::TextUnformatted("C# scripts aren't built yet");
      ImGui::EndDisabled();
    }

    ImGui::EndMenu();
  }
#endif
}

auto SceneHierarchyPanel::set_scene(this SceneHierarchyPanel& self, EditorScene* scene) -> void {
  ZoneScoped;

  self.current_scene = scene;
  self.scene_ = scene != nullptr ? scene->get_scene().get() : nullptr;

  self.selected_entity_.reset();
  self.renaming_entity_ = {};
  self.dragged_entity_ = {};
  self.dragged_entity_target_ = {};
  self.deleted_entity_ = {};
}

auto SceneHierarchyPanel::get_scene(this const SceneHierarchyPanel& self) -> EditorScene* {
  ZoneScoped;

  return self.current_scene;
}
} // namespace ox
