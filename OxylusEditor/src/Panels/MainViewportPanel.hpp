#pragma once

#include "Panels/EditorPanel.hpp"
#include "ViewportPanel.hpp"

namespace ox {
class MainViewportPanel : public EditorPanel {
public:
  MainViewportPanel();

  auto init(this MainViewportPanel& self) -> void;
  auto reset(this MainViewportPanel& self) -> void;

  auto get_focused_viewport(this const MainViewportPanel& self) -> ViewportPanel*;

  auto add_new_scene(this MainViewportPanel& self, const std::shared_ptr<EditorScene>& scene) -> void;
  auto add_new_play_scene(this MainViewportPanel& self, const std::shared_ptr<EditorScene>& scene) -> void;
  auto add_viewport(this MainViewportPanel& self) -> ViewportPanel*;

  void on_render(vuk::ImageAttachment swapchain_attachment) override;
  void update(const Timestep & timestep, SceneHierarchyPanel * sh) const;

  auto update_dockspace(this MainViewportPanel& self) -> void;
  auto set_dockspace(this const MainViewportPanel& self) -> void;

private:
  std::vector<std::unique_ptr<ViewportPanel>> viewport_panels = {};
  bool dock_should_update = false;

  void drag_drop(this MainViewportPanel& self);
};
} // namespace ox
