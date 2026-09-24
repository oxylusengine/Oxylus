#pragma once

#include <imgui_internal.h>

#include "AssetManagerPanel.hpp"
#include "EditorPanelState.hpp"
#include "Scene/Scene.hpp"
#include "Utils/SceneManager.hpp"

namespace ox {
class SceneHierarchyPanel : public EditorPanelState {
public:
  // Selecting here is what drives the editor context, so going through this rather than assigning
  // the entity keeps the inspector and the gizmos in step.
  struct SelectedEntity {
    flecs::entity entity = flecs::entity::null();

    auto set(this SelectedEntity& self, flecs::entity e) -> void;
    auto get(this SelectedEntity& self) -> flecs::entity;
    auto reset(this SelectedEntity& self) -> void;
  };

  SelectedEntity selected_entity_ = {};
  const UUID* selected_script_ = nullptr;

  bool table_hovered_ = false;
  bool table_hovered_scripts = false;
  bool window_hovered_ = false;

  flecs::entity renaming_entity_ = {};
  flecs::entity dragged_entity_ = {};
  flecs::entity dragged_entity_target_ = {};
  flecs::entity deleted_entity_ = {};

  SceneHierarchyPanel();

  auto on_update(this SceneHierarchyPanel& self) -> void;
  auto on_render(this SceneHierarchyPanel& self, vuk::ImageAttachment swapchain_attachment) -> void;

  auto set_scene(this SceneHierarchyPanel& self, EditorScene* scene) -> void;
  auto get_scene(this const SceneHierarchyPanel& self) -> EditorScene*;

private:
  EditorScene* current_scene = nullptr;
  Scene* scene_ = nullptr;

  ImGuiTextFilter scripts_filter_;
  ImGuiTextFilter entities_filter_;

  AssetBrowser script_picker_ = {};
  bool script_picker_open_ = false;

  auto draw_entity_node(
    this SceneHierarchyPanel& self,
    flecs::entity entity,
    u32 depth = 0,
    bool force_expand_tree = false,
    bool is_part_of_prefab = false
  ) -> ImRect;

  auto draw_entities_context_menu(this SceneHierarchyPanel& self) -> void;
  auto draw_scripts_context_menu(this SceneHierarchyPanel& self) -> void;
  auto draw_script_picker(this SceneHierarchyPanel& self) -> void;
};
} // namespace ox
