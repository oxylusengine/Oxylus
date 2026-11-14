#pragma once

#include <functional>
#include <imgui_internal.h>
#include <string>
#include <vector>

#include "Actions/EntityActions.hpp"
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

  std::string rename_text_ = {};
  std::string rename_original_ = {};
  bool is_rename_started_ = false;

  SceneHierarchyPanel();

  auto on_update(this SceneHierarchyPanel& self) -> void;
  auto on_render(this SceneHierarchyPanel& self, const SwapchainInfo& swapchain_info) -> void;

  auto set_scene(this SceneHierarchyPanel& self, EditorScene* scene) -> void;
  auto get_scene(this const SceneHierarchyPanel& self) -> EditorScene*;

private:
  EditorScene* current_scene = nullptr;
  Scene* scene_ = nullptr;

  struct DeferredEdit {
    std::string label;
    std::function<void(SceneHierarchyPanel&, EditTransaction&)> edit;
  };

  // edits raised while the tree is being walked run after it, so entities don't move under the walk
  std::vector<DeferredEdit> deferred_edits_ = {};

  ImGuiTextFilter scripts_filter_;
  ImGuiTextFilter entities_filter_;

  auto draw_entity_node(
    this SceneHierarchyPanel& self,
    flecs::entity entity,
    u32 depth = 0,
    bool force_expand_tree = false,
    bool is_part_of_prefab = false
  ) -> ImRect;

  auto queue_edit(
    this SceneHierarchyPanel& self, std::string label, std::function<void(SceneHierarchyPanel&, EditTransaction&)> edit
  ) -> void;
  auto run_deferred_edits(this SceneHierarchyPanel& self) -> void;
  auto queue_create_from_asset(this SceneHierarchyPanel& self, const UUID& asset, flecs::entity parent) -> void;
  auto finish_rename(this SceneHierarchyPanel& self, flecs::entity entity) -> void;

  auto draw_entities_context_menu(this SceneHierarchyPanel& self) -> void;
  auto draw_scripts_context_menu(this SceneHierarchyPanel& self) -> void;
};
} // namespace ox
