#pragma once

#include "EditorPanel.hpp"
#include "Scene/Scene.hpp"
#include "UI/SceneHierarchyViewer.hpp"
#include "Utils/SceneManager.hpp"

namespace ox {
class SceneHierarchyPanel : public EditorPanel {
public:
  SceneHierarchyViewer viewer = {};

  SceneHierarchyPanel();

  auto on_update() -> void override;
  auto on_render(vuk::ImageAttachment swapchain_attachment) -> void override;

  auto set_scene(EditorScene* scene) -> void;
  auto get_scene() const -> EditorScene*;

private:
  EditorScene* current_scene = nullptr;
};
} // namespace ox
