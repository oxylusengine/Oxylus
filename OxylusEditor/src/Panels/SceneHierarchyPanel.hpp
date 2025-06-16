#pragma once

#include <imgui_internal.h>

#include "EditorPanel.hpp"
#include "Scene/Scene.hpp"

namespace ox {
class SceneHierarchyPanel : public EditorPanel {
public:
  SceneHierarchyPanel();

  auto on_update() -> void override;
  auto on_render(vuk::Extent3D extent, vuk::Format format) -> void override;

  auto draw_entity_node(flecs::entity entity,
                        uint32_t depth = 0,
                        bool force_expand_tree = false,
                        bool is_part_of_prefab = false) -> ImRect;
  auto set_scene(const std::shared_ptr<Scene>& scene) -> void;

  auto get_scene() const -> const std::shared_ptr<Scene>& { return _scene; }

private:
  class SelectedEntity {
  public:
    auto set(this SelectedEntity& self, flecs::entity e) -> void;
    auto get(this SelectedEntity& self) -> flecs::entity { return self.entity; };
    auto reset(this SelectedEntity& self) -> void;

  private:
    flecs::entity entity = flecs::entity::null();
  };

  SelectedEntity _selected_entity = {};

  flecs::entity _renaming_entity = {};
  flecs::entity _dragged_entity = {};
  flecs::entity _dragged_entity_target = {};
  flecs::entity _deleted_entity = {};

  std::shared_ptr<Scene> _scene = nullptr;
  ImGuiTextFilter _filter;
  bool _table_hovered = false;
  bool _window_hovered = false;

  void draw_context_menu();
};
} // namespace ox
