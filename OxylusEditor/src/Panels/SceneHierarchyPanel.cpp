#include "SceneHierarchyPanel.hpp"

#include <glm/trigonometric.hpp>
#include <icons/IconsMaterialDesignIcons.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include "Asset/AssetImporter.hpp"
#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Editor.hpp"
#include "Memory/Stack.hpp"
#include "Panels/TextEditorPanel.hpp"
#include "Render/DebugRenderer.hpp"
#include "UI/PayloadData.hpp"
#include "UI/UI.hpp"
#include "Utils/ImGuiScoped.hpp"

namespace ox {
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

// Opening a script is a hand-off to the text editor panel, which owns the buffer and the tab.
static auto open_script_in_editor(const UUID& uuid) -> void {
  ZoneScoped;

  auto asset = App::mod<AssetManager>().get_asset(uuid);
  if (!asset) {
    return;
  }

  auto& text_editor_panel = App::mod<Editor>().editor_panel_registry.get<TextEditorPanel>();
  text_editor_panel.visible = true;
  text_editor_panel.text_editor.open_file(asset->path);
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

  if (self.selected_entity_.get() != flecs::entity::null()) {
    if (auto* cam = self.selected_entity_.get().try_get<CameraComponent>()) {
      const auto proj = cam->get_projection_matrix() * cam->get_view_matrix();
      auto& debug_renderer = App::mod<DebugRenderer>();
      debug_renderer.draw_frustum(proj, glm::vec4(0, 1, 0, 1), cam->near_clip, cam->far_clip);
    }
    if (auto* light = self.selected_entity_.get().try_get<LightComponent>()) {
      const glm::vec3 world_pos = Scene::get_world_position(self.selected_entity_.get());
      if (light->type == LightComponent::Point) {
        auto& debug_renderer = App::mod<DebugRenderer>();
        debug_renderer.draw_sphere(light->radius, world_pos, glm::vec4(0, 1.f, 0.f, 1.f));
      } else if (light->type == LightComponent::Spot) {
      }
    }

    if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) && ImGui::IsKeyPressed(ImGuiKey_D)) {
      auto clone_entity = [](flecs::entity entity) -> flecs::entity {
        std::string clone_name = entity.name().c_str();
        while (entity.world().lookup(clone_name.data())) {
          clone_name = fmt::format("{}_clone", clone_name);
        }
        auto cloned_entity = entity.clone(true);
        return cloned_entity.set_name(clone_name.data());
      };

      self.selected_entity_.set(clone_entity(self.selected_entity_.get()));
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
    self.selected_script_ && ImGui::IsKeyPressed(ImGuiKey_Delete) && (self.table_hovered_ || self.table_hovered_scripts)
  ) {
    self.scene_->remove_lua_system(*self.selected_script_);
  }

  if (self.deleted_entity_) {
    auto command_id = fmt::format("delete entity {}", self.deleted_entity_.name().c_str());
    undo_redo_system->execute_command<EntityDeleteCommand>(self.scene_, self.deleted_entity_, "", command_id);
    self.selected_entity_.reset();
  }
}

auto SceneHierarchyPanel::on_render(this SceneHierarchyPanel& self, vuk::ImageAttachment swapchain_attachment) -> void {
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

        for (auto& [uuid, system] : self.scene_->get_lua_systems()) {
          auto system_name = system->get_path().filename().string();
          if (self.scripts_filter_.IsActive() && !self.scripts_filter_.PassFilter(system_name.c_str())) {
            continue;
          }

          ImGui::TableNextRow();

          ImGui::TableNextColumn();

          bool is_selected = self.selected_script_ != nullptr && *self.selected_script_ == uuid;

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

          ImGui::TreeNodeEx(uuid.str().c_str(), flags, "%s %s", ICON_MDI_SCRIPT, system_name.c_str());

          if (highlight)
            ImGui::PopStyleColor(3);

          // Select
          if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            self.selected_entity_.reset();
            self.selected_script_ = &uuid;
          }
          if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            open_script_in_editor(uuid);
          }
        }

        if (
          ImGui::BeginPopupContextWindow(
            "scene_h_scripts_context_window",
            ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems
          )
        ) {
          self.selected_entity_.reset();
          self.selected_script_ = nullptr;
          self.draw_scripts_context_menu();
          ImGui::EndPopup();
        }

        ImGui::EndTable();

        if (ImGui::BeginPopupContextItem()) {
          if (ImGui::MenuItem("Reload")) {
            if (self.selected_script_ && !self.scene_->is_running()) {
              if (auto lua_system = self.scene_->get_lua_system(*self.selected_script_)) {
                lua_system->reload();
              }
            }
          }
          if (ImGui::MenuItem("Remove", "Del")) {
            if (self.selected_script_) {
              self.scene_->remove_lua_system(*self.selected_script_);
            }
          }

          ImGui::EndPopup();
        }

        self.table_hovered_scripts = ImGui::IsItemHovered();

        if (ImGui::IsItemClicked()) {
          self.selected_entity_.reset();
          self.selected_script_ = nullptr;
        }
      }
      ImGui::TreePop();
    }

    filter_cursor_pos_x = ImGui::GetCursorPosX();

    if (ImGui::TreeNodeEx("Entities", ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_DefaultOpen)) {
      self.entities_filter_.Draw(
        "###HierarchyFilter",
        ImGui::GetContentRegionAvail().x - (ImGui::CalcTextSize(ICON_MDI_PLUS).x + UI::scale(20.0f))
      );
      ImGui::SameLine();

      if (UI::button(stack.format_char("{}###AddEntity", ICON_MDI_PLUS)))
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

      if (!self.entities_filter_.IsActive()) {
        ImGui::SameLine();
        ImGui::SetCursorPosX(filter_cursor_pos_x + ImGui::GetFontSize() * 0.5f);
        auto search_txt = fmt::format("  {} Search entities...", ICON_MDI_MAGNIFY);
        ImGui::TextUnformatted(search_txt.c_str());
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
                  self.selected_script_ = nullptr;
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
        self.scene_->world.query_builder<TransformComponent>()
          .with(flecs::Disabled)
          .optional()
          .build()
          .each([&self](const flecs::entity e, TransformComponent) {
            if (e.parent() == flecs::entity::null())
              self.draw_entity_node(e);
          });
        ImGui::PopStyleVar();

        if (
          ImGui::BeginPopupContextWindow(
            "scene_h_entities_context_window",
            ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems
          )
        ) {
          self.selected_entity_.reset();
          self.selected_script_ = nullptr;
          self.draw_entities_context_menu();
          ImGui::EndPopup();
        }

        ImGui::EndTable();

        self.table_hovered_ = ImGui::IsItemHovered();

        if (ImGui::IsItemClicked()) {
          self.selected_entity_.reset();
          self.selected_script_ = nullptr;
        }
      }

      if (ImGui::IsMouseDown(0) && self.window_hovered_) {
        self.selected_entity_.reset();
        self.selected_script_ = nullptr;
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

  self.draw_script_picker();
}

auto SceneHierarchyPanel::draw_entity_node(
  this SceneHierarchyPanel& self, flecs::entity entity, u32 depth, bool force_expand_tree, bool is_part_of_prefab
) -> ImRect {
  ZoneScoped;

  if (entity.has<Hidden>())
    return {0, 0, 0, 0};

  ImGui::TableNextRow();
  ImGui::TableNextColumn();

  const auto child_count = self.scene_->world.count(flecs::ChildOf, entity);

  if (self.entities_filter_.IsActive() && !self.entities_filter_.PassFilter(entity.name().c_str())) {
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
    self.selected_script_ = nullptr;
  }

  // Expand recursively
  if (ImGui::IsItemToggledOpen() && (ImGui::IsKeyDown(ImGuiKey_LeftAlt) || ImGui::IsKeyDown(ImGuiKey_RightAlt)))
    force_expand_tree = opened;

  bool entity_deleted = false;

  if (ImGui::BeginPopupContextItem()) {
    if (ImGui::MenuItem("Rename", "F2"))
      self.renaming_entity_ = entity;
    if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
      auto clone_entity = [&self](flecs::entity e) -> flecs::entity {
        std::string clone_name = self.scene_->safe_entity_name(fmt::format("{}_clone", e.name().c_str()));
        auto cloned_entity = e.clone(true);
        return cloned_entity.set_name(clone_name.data());
      };

      self.selected_entity_.set(clone_entity(entity));
      self.selected_script_ = nullptr;
    }
    if (ImGui::MenuItem("Delete", "Del"))
      entity_deleted = true;

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
              self.selected_script_ = nullptr;
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
    if (opened && !entity_deleted) {
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
        const ImRect child_rect = self.draw_entity_node(child, depth + 1, force_expand_tree, is_part_of_prefab);

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
    self.selected_script_ = nullptr;
  }
}

auto SceneHierarchyPanel::draw_scripts_context_menu(this SceneHierarchyPanel& self) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  if (ImGui::MenuItem(stack.format_char("{} Add script...", ICON_MDI_SCRIPT))) {
    self.script_picker_open_ = true;
    ImGui::CloseCurrentPopup();
  }
}

auto SceneHierarchyPanel::draw_script_picker(this SceneHierarchyPanel& self) -> void {
  ZoneScoped;

  if (!self.script_picker_open_ || !self.scene_) {
    return;
  }

  const auto picked = self.script_picker_.render_picker("Add Script", &self.script_picker_open_, AssetType::Script);
  if (!picked) {
    return;
  }

  auto& asset_man = App::mod<AssetManager>();
  if (!asset_man.load_asset(picked->uuid)) {
    OX_LOG_ERROR("Couldn't load script asset {}!", picked->uuid.str());
    return;
  }

  self.scene_->add_lua_system(picked->uuid);
}

auto SceneHierarchyPanel::set_scene(this SceneHierarchyPanel& self, EditorScene* scene) -> void {
  ZoneScoped;

  self.current_scene = scene;
  self.scene_ = scene != nullptr ? scene->get_scene().get() : nullptr;

  self.selected_entity_.reset();
  self.selected_script_ = nullptr;
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
