#include "MainViewportPanel.hpp"

#include <icons/IconsMaterialDesignIcons.h>
#include <vuk/ImageAttachment.hpp>

#include "Core/App.hpp"
#include "Editor.hpp"

namespace ox {
MainViewportPanel::MainViewportPanel() : EditorPanel("Viewports", ICON_MDI_VIDEO_3D, true, false) {}

auto MainViewportPanel::init(this MainViewportPanel& self) -> void {
  auto& event_system = App::get_event_system();
  event_system.subscribe<Editor::ViewportSceneLoadEvent>([&self](const Editor::ViewportSceneLoadEvent& e) {
    self.dock_should_update = true;
  });
  event_system.subscribe<Editor::ScenePlayEvent>([&self](const Editor::ScenePlayEvent& e) {
    App::get()->defer_to_next_frame([&self, e] {
      auto& editor = App::mod<Editor>();
      auto scene = editor.scene_manager.get_scene(e.scene_id);
      auto scene_copy = scene->play();
      self.add_new_play_scene(scene_copy);
    });
  });
  event_system.subscribe<Editor::SceneStopEvent>([&self](const Editor::SceneStopEvent& e) {
    App::get()->defer_to_next_frame([&self, e] {
      for (auto it = self.viewport_panels.begin(); it != self.viewport_panels.end(); ++it) {
        auto* panel = it->get();
        auto* editor_scene = panel->get_scene();
        if (editor_scene && editor_scene->get_id() == e.scene_id && editor_scene->is_playing()) {
          editor_scene->stop();
          self.viewport_panels.erase(it);
          self.dock_should_update = true;
          break;
        }
      }
    });
  });
}

auto MainViewportPanel::reset(this MainViewportPanel& self) -> void {
  ZoneScoped;

  self.viewport_panels.clear();
}

auto MainViewportPanel::get_focused_viewport(this const MainViewportPanel& self) -> ViewportPanel* {
  for (auto& viewport : self.viewport_panels) {
    if (viewport->is_viewport_focused) {
      return viewport.get();
    }
  }

  return nullptr;
}

auto MainViewportPanel::add_new_scene(const std::shared_ptr<EditorScene>& scene) -> void {
  auto* viewport = add_viewport();
  viewport->set_context(scene, nullptr);

  dock_should_update = true;
}

auto MainViewportPanel::add_new_play_scene(const std::shared_ptr<EditorScene>& scene) -> void {
  auto* viewport = add_viewport();
  viewport->set_context(scene, nullptr);
  viewport->set_icon(ICON_MDI_CONTROLLER);
  viewport->set_name(fmt::format("Game:{}", scene->get_scene()->scene_name));

  dock_should_update = true;
}

auto MainViewportPanel::add_viewport() -> ViewportPanel* {
  auto& viewport = viewport_panels.emplace_back(std::make_unique<ViewportPanel>());
  dock_should_update = true;

  return viewport.get();
}

void MainViewportPanel::on_render(vuk::ImageAttachment swapchain_attachment) {
  if (on_begin()) {
    auto dockspace_id = ImGui::GetID("ViewportDockspace");
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);

    if (dock_should_update) {
      set_dockspace();
      dock_should_update = false;
    }

    auto fullscreen_view = viewport_panels |
                           std::views::filter([](const auto& panel) { return panel->fullscreen_viewport; });

    if (fullscreen_view) {
      fullscreen_view.front()->on_render(swapchain_attachment);
    } else {
      for (const auto& panel : viewport_panels) {
        auto* viewport_editor_scene = panel->get_scene();
        if (viewport_editor_scene) {
          if (viewport_editor_scene->is_playing()) {
            viewport_editor_scene->get_scene()->on_render(swapchain_attachment.extent, swapchain_attachment.format);
          }
        }

        panel->on_render(swapchain_attachment);
      }
    }
  }

  on_end();
}

void MainViewportPanel::update(const Timestep& timestep, SceneHierarchyPanel* sh) const {
  for (const auto& panel : viewport_panels) {
    auto* editor_scene = panel->get_scene();

    if (editor_scene) {
      if (panel->is_viewport_focused) {
        if (editor_scene->get_scene()) {
          sh->set_scene(editor_scene->get_scene().get());
        }
      }

      if (editor_scene->is_playing()) {
        editor_scene->get_scene()->enable_all_phases();
        editor_scene->get_scene()->runtime_update(timestep);
      } else {
        editor_scene->get_scene()->disable_phases({flecs::PreUpdate, flecs::OnUpdate});
        editor_scene->get_scene()->runtime_update(timestep);
      }
    }

    if (panel->visible) {
      panel->on_update();
    }
  }
}

auto MainViewportPanel::update_dockspace() -> void { dock_should_update = true; }

auto MainViewportPanel::set_dockspace() -> void {
  auto dock_id = ImGui::GetID("ViewportDockspace");

  for (auto& panel : viewport_panels) {
    ImGui::DockBuilderDockWindow(panel->get_id(), dock_id);
  }

  ImGui::DockBuilderFinish(dock_id);
}
} // namespace ox
