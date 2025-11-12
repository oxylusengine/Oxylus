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

  auto add_new_scene(const std::shared_ptr<EditorScene>& scene) -> void;
  auto add_new_play_scene(const std::shared_ptr<EditorScene>& scene) -> void;
  auto add_viewport() -> ViewportPanel*;

  void on_render(vuk::ImageAttachment swapchain_attachment) override;
  void update(const Timestep & timestep, SceneHierarchyPanel * sh) const;

  auto update_dockspace() -> void;
  auto set_dockspace() -> void;

private:
  std::vector<std::unique_ptr<ViewportPanel>> viewport_panels = {};
  bool dock_should_update = false;
};
} // namespace ox
