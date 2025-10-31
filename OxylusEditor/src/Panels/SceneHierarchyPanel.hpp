#pragma once

#include <imgui_internal.h>

#include "EditorPanel.hpp"
#include "Scene/Scene.hpp"
#include "UI/SceneHierarchyViewer.hpp"

namespace ox {
class SceneHierarchyPanel : public EditorPanel {
public:
  SceneHierarchyViewer viewer = {};

  SceneHierarchyPanel();

  auto on_update() -> void override;
  auto on_render(vuk::ImageAttachment swapchain_attachment) -> void override;

  auto set_scene(Scene* scene) -> void { viewer.set_scene(scene); }
  auto get_scene() -> Scene* { return viewer.get_scene(); }
};
} // namespace ox
