#pragma once

#include "EditorPanelState.hpp"
#include "UI/SceneHierarchyViewer.hpp"
#include "Utils/SceneManager.hpp"

namespace ox {
class SceneHierarchyPanel : public EditorPanelState {
public:
  SceneHierarchyViewer viewer = {};

  SceneHierarchyPanel();

  auto on_update(this SceneHierarchyPanel& self) -> void;
  auto on_render(this SceneHierarchyPanel& self, vuk::ImageAttachment swapchain_attachment) -> void;

  auto set_scene(this SceneHierarchyPanel& self, EditorScene* scene) -> void;
  auto get_scene(this const SceneHierarchyPanel& self) -> EditorScene*;

private:
  EditorScene* current_scene = nullptr;
};
} // namespace ox
