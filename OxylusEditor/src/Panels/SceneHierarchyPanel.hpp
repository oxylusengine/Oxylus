#pragma once

#include <imgui_internal.h>

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

  bool table_hovered_ = false;
  bool table_hovered_scripts = false;
  bool window_hovered_ = false;

  flecs::entity renaming_entity_ = {};
  flecs::entity dragged_entity_ = {};
  flecs::entity dragged_entity_target_ = {};
  flecs::entity deleted_entity_ = {};

  SceneHierarchyPanel();

  auto on_update(this SceneHierarchyPanel& self) -> void;
  auto on_render(this SceneHierarchyPanel& self, const SwapchainInfo& swapchain_info) -> void;

  auto set_scene(this SceneHierarchyPanel& self, EditorScene* scene) -> void;
  auto get_scene(this const SceneHierarchyPanel& self) -> EditorScene*;
  auto find_component(this SceneHierarchyPanel& self, flecs::entity component) -> void;

private:
  enum class EntityView : u8 { Hierarchy, SearchResult };

  EditorScene* current_scene = nullptr;
  Scene* scene_ = nullptr;

  ImGuiTextFilter scripts_filter_;
  ImGuiTextFilter entities_filter_;
  flecs::entity search_root_ = {};
  usize selected_suggestion_ = 0;
  bool focus_search_ = false;

  auto draw_entity_search(this SceneHierarchyPanel& self) -> void;

  auto draw_entity_node(
    this SceneHierarchyPanel& self,
    flecs::entity entity,
    EntityView view = EntityView::Hierarchy,
    u32 depth = 0,
    bool force_expand_tree = false,
    bool is_part_of_prefab = false
  ) -> ImRect;

  auto draw_entities_context_menu(this SceneHierarchyPanel& self) -> void;
  auto draw_scripts_context_menu(this SceneHierarchyPanel& self) -> void;
};
} // namespace ox
