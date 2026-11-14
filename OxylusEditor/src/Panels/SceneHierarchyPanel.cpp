#include "SceneHierarchyPanel.hpp"

#include <glm/trigonometric.hpp>
#include <icons/IconsMaterialDesignIcons.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include "Actions/EntityActions.hpp"
#include "Asset/AssetImporter.hpp"
#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Core/VFS.hpp"
#include "Editor.hpp"
#include "Memory/Stack.hpp"
#include "Render/DebugRenderer.hpp"
#ifdef OX_DOTNET
  #include "Scripting/DotNetScripting.hpp"
#endif
#include "UI/PayloadData.hpp"
#include "UI/UI.hpp"
#include "Utils/ImGuiScoped.hpp"

namespace ox {
static auto has_mesh_in_hierarchy(flecs::entity entity) -> bool {
  if (entity.has<MeshComponent>()) {
    return true;
  }

  auto found = false;
  entity.children([&found](flecs::entity child) { found = found || has_mesh_in_hierarchy(child); });
  return found;
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

  if (editor_context.type == EditorContext::Type::Entity) {
    if (editor_context.entity.has_value())
      self.selected_entity_.set(editor_context.entity.value());
  } else {
    self.selected_entity_.entity = flecs::entity::null();
  }

  if (self.selected_entity_.get() != flecs::entity::null()) {
    if (auto* cam = self.selected_entity_.get().try_get<CameraComponent>(); cam && self.scene_) {
      const auto proj = cam->get_projection_matrix() * cam->get_view_matrix();
      self.scene_->debug_renderer.draw_frustum(proj, glm::vec4(0, 1, 0, 1), cam->near_clip, cam->far_clip);
    }
    if (auto* light = self.selected_entity_.get().try_get<LightComponent>()) {
      const glm::vec3 world_pos = Scene::get_world_position(self.selected_entity_.get());
      if (light->type == LightComponent::Point && self.scene_) {
        self.scene_->debug_renderer.draw_sphere(light->radius, world_pos, glm::vec4(0, 1.f, 0.f, 1.f));
      } else if (light->type == LightComponent::Spot) {
      }
    }

    if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) && ImGui::IsKeyPressed(ImGuiKey_D) && self.scene_) {
      auto tx = begin_edit(editor, *self.scene_, "duplicate entity");
      if (const auto clone = duplicate_entity(tx, self.selected_entity_.get())) {
        self.selected_entity_.set(clone.value());
      } else {
        OX_LOG_ERROR("{}", clone.error());
      }
      tx.commit();
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

  if (self.deleted_entity_ && self.scene_) {
    auto tx = begin_edit(editor, *self.scene_, "delete entity");
    if (const auto result = delete_entity(tx, self.deleted_entity_); !result) {
      OX_LOG_ERROR("{}", result.error());
    }
    tx.commit();

    self.deleted_entity_ = flecs::entity::null();
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
              if (auto asset = import_asset(App::mod<AssetManager>(), path)) {
                self.queue_create_from_asset(asset, flecs::entity::null());
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
        auto tx = begin_edit(App::mod<Editor>(), *self.scene_, "reparent entity");
        if (const auto result = reparent_entity(tx, self.dragged_entity_, self.dragged_entity_target_); !result) {
          OX_LOG_ERROR("{}", result.error());
        }
        tx.commit();

        self.dragged_entity_ = flecs::entity::null();
        self.dragged_entity_target_ = flecs::entity::null();
      }

      self.run_deferred_edits();

      ImGui::TreePop();
    }

    self.window_hovered_ = ImGui::IsWindowHovered();
  }

  ImGui::End();
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
  }

  // Expand recursively
  if (ImGui::IsItemToggledOpen() && (ImGui::IsKeyDown(ImGuiKey_LeftAlt) || ImGui::IsKeyDown(ImGuiKey_RightAlt)))
    force_expand_tree = opened;

  bool entity_deleted = false;

  if (ImGui::BeginPopupContextItem()) {
    if (ImGui::MenuItem("Rename", "F2"))
      self.renaming_entity_ = entity;
    if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
      self.queue_edit("duplicate entity", [entity](SceneHierarchyPanel& panel, EditTransaction& tx) {
        if (const auto clone = duplicate_entity(tx, entity)) {
          panel.selected_entity_.set(clone.value());
        } else {
          OX_LOG_ERROR("{}", clone.error());
        }
      });
    }
    if (ImGui::MenuItem("Delete", "Del"))
      entity_deleted = true;

    if (has_mesh_in_hierarchy(entity) && ImGui::MenuItem("Add Mesh Colliders")) {
      self.queue_edit("add mesh colliders", [entity](SceneHierarchyPanel&, EditTransaction& tx) {
        if (const auto result = add_mesh_colliders(tx, entity); !result) {
          OX_LOG_ERROR("{}", result.error());
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
          if (auto asset = import_asset(App::mod<AssetManager>(), path)) {
            self.queue_create_from_asset(asset, entity);
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
    if (!self.is_rename_started_) {
      self.is_rename_started_ = true;
      self.rename_original_ = entity.name().c_str();
      self.rename_text_ = self.rename_original_;
      ImGui::SetKeyboardFocusHere();
    }

    if (ImGui::InputText("##Tag", &self.rename_text_)) {
      const auto is_applicable = !self.rename_text_.empty() && !self.rename_text_.contains('/') &&
                                 is_entity_name_free(*self.scene_, entity, self.rename_text_);
      if (is_applicable) {
        entity.set_name(self.rename_text_.c_str());
      }
    }

    if (ImGui::IsItemDeactivated()) {
      self.finish_rename(entity);
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
      self.queue_edit(
        "toggle entity",
        [entity, is_enabled = entity.enabled()](SceneHierarchyPanel&, EditTransaction& tx) {
          if (const auto result = set_entity_enabled(tx, entity, !is_enabled); !result) {
            OX_LOG_ERROR("{}", result.error());
          }
        }
      );
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

  const auto parent = has_context ? self.selected_entity_.get() : flecs::entity::null();
  const auto create = [&self, parent](EntityPreset preset) {
    self.queue_edit("create entity", [preset, parent](SceneHierarchyPanel& panel, EditTransaction& tx) {
      if (const auto created = create_entity(tx, "", parent, preset)) {
        panel.selected_entity_.set(created.value());
      } else {
        OX_LOG_ERROR("{}", created.error());
      }
    });
  };

  ImGuiScoped::StyleVar styleVar1(ImGuiStyleVar_ItemInnerSpacing, UI::scale(ImVec2(0.0f, 5.0f)));
  ImGuiScoped::StyleVar styleVar2(ImGuiStyleVar_ItemSpacing, UI::scale(ImVec2(1.0f, 5.0f)));
  if (ImGui::BeginMenu("Create")) {
    if (ImGui::MenuItem("New Entity")) {
      create(EntityPreset::Empty);
    }

    if (ImGui::MenuItem("Sprite")) {
      create(EntityPreset::Sprite);
    }

    if (ImGui::MenuItem("Camera")) {
      create(EntityPreset::Camera);
    }

    if (ImGui::MenuItem("Terrain")) {
      create(EntityPreset::Terrain);
    }

    if (ImGui::MenuItem("Probe Volume")) {
      create(EntityPreset::ProbeVolume);
    }

    if (ImGui::BeginMenu("Light")) {
      if (ImGui::MenuItem("Light")) {
        create(EntityPreset::Light);
      }
      if (ImGui::MenuItem("Sun")) {
        create(EntityPreset::Sun);
      }
      ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Audio")) {
      if (ImGui::MenuItem("Audio Source")) {
        create(EntityPreset::AudioSource);
        ImGui::CloseCurrentPopup();
      }
      if (ImGui::MenuItem("Audio Listener")) {
        create(EntityPreset::AudioListener);
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Effects")) {
      if (ImGui::MenuItem("Particle System")) {
        create(EntityPreset::ParticleSystem);
      }
      ImGui::EndMenu();
    }

    ImGui::EndMenu();
  }
}

auto SceneHierarchyPanel::queue_edit(
  this SceneHierarchyPanel& self, std::string label, std::function<void(SceneHierarchyPanel&, EditTransaction&)> edit
) -> void {
  self.deferred_edits_.push_back({.label = std::move(label), .edit = std::move(edit)});
}

auto SceneHierarchyPanel::run_deferred_edits(this SceneHierarchyPanel& self) -> void {
  ZoneScoped;

  auto edits = std::move(self.deferred_edits_);
  self.deferred_edits_.clear();
  if (!self.scene_) {
    return;
  }

  for (auto& deferred : edits) {
    auto tx = begin_edit(App::mod<Editor>(), *self.scene_, std::move(deferred.label));
    deferred.edit(self, tx);
    tx.commit();
  }
}

auto SceneHierarchyPanel::queue_create_from_asset(
  this SceneHierarchyPanel& self, const UUID& asset, flecs::entity parent
) -> void {
  self.queue_edit("create entity", [asset, parent](SceneHierarchyPanel& panel, EditTransaction& tx) {
    if (const auto created = create_entity_from_asset(tx, asset, parent)) {
      panel.selected_entity_.set(created.value());
    } else {
      OX_LOG_ERROR("{}", created.error());
    }
  });
}

auto SceneHierarchyPanel::finish_rename(this SceneHierarchyPanel& self, flecs::entity entity) -> void {
  self.is_rename_started_ = false;
  self.renaming_entity_ = flecs::entity::null();

  if (self.rename_text_ == self.rename_original_) {
    return;
  }

  // the typing was applied live, so it goes back first and the action records the one change
  entity.set_name(self.rename_original_.c_str());

  auto tx = begin_edit(App::mod<Editor>(), *self.scene_, "rename entity");
  if (const auto result = rename_entity(tx, entity, self.rename_text_); !result) {
    OX_LOG_ERROR("{}", result.error());
  }
  tx.commit();
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
  self.is_rename_started_ = false;
  self.deferred_edits_.clear();
}

auto SceneHierarchyPanel::get_scene(this const SceneHierarchyPanel& self) -> EditorScene* {
  ZoneScoped;

  return self.current_scene;
}
} // namespace ox
