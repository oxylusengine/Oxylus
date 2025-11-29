#include "MainViewportPanel.hpp"

#include <icons/IconsMaterialDesignIcons.h>
#include <vuk/ImageAttachment.hpp>

#include "Core/App.hpp"
#include "Editor.hpp"
#include "UI/PayloadData.hpp"
#include "UI/UI.hpp"

namespace ox {
MainViewportPanel::MainViewportPanel() : EditorPanel("Scenes", ICON_MDI_VIDEO_3D, true, false) {}

auto MainViewportPanel::init(this MainViewportPanel& self) -> void {
  auto& event_system = App::get_event_system();
  std::ignore = event_system.subscribe<Editor::ViewportSceneLoadEvent>(
    [&self](const Editor::ViewportSceneLoadEvent& e) { self.dock_should_update = true; }
  );
  std::ignore = event_system.subscribe<Editor::ScenePlayEvent>([&self](const Editor::ScenePlayEvent& e) {
    auto& editor = App::mod<Editor>();
    auto scene = editor.scene_manager.get_scene(e.scene_id);
    auto scene_copy = scene->play();
    self.add_new_play_scene(scene_copy);
  });
  std::ignore = event_system.subscribe<Editor::SceneStopEvent>([&self](const Editor::SceneStopEvent& e) {
    auto should_stop_and_remove = [e, &self](const std::unique_ptr<ViewportPanel>& panel) {
      auto* editor_scene = panel->get_scene();

      if (editor_scene && editor_scene->get_id() == e.scene_id && editor_scene->is_playing()) {
        editor_scene->stop();
        self.dock_should_update = true;
        return true;
      }
      return false;
    };

    App::defer_to_next_frame([&self, should_stop_and_remove] {
      std::erase_if(self.viewport_panels, should_stop_and_remove);
      std::erase_if(self.pending_viewports, should_stop_and_remove);
    });
  });
}

auto MainViewportPanel::reset(this MainViewportPanel& self) -> void {
  ZoneScoped;

  self.viewport_panels.clear();
  self.pending_viewports.clear();
}

auto MainViewportPanel::get_focused_viewport(this const MainViewportPanel& self) -> ViewportPanel* {
  for (auto& viewport : self.viewport_panels) {
    if (viewport->is_viewport_focused) {
      return viewport.get();
    }
  }

  return nullptr;
}

auto MainViewportPanel::get_visible_viwports(this const MainViewportPanel& self) -> std::vector<ViewportPanel*> {
  auto v = std::vector<ViewportPanel*>{};

  for (auto& viewport : self.viewport_panels) {
    if (viewport->visible) {
      v.emplace_back(viewport.get());
    }
  }

  return v;
}

auto MainViewportPanel::add_new_scene(this MainViewportPanel& self, const std::shared_ptr<EditorScene>& scene) -> void {
  auto* viewport = self.add_viewport();
  viewport->set_context(scene);

  self.dock_should_update = true;
}

auto MainViewportPanel::add_new_play_scene(this MainViewportPanel& self, const std::shared_ptr<EditorScene>& scene)
  -> void {
  auto* viewport = self.add_viewport();
  viewport->set_context(scene);
  viewport->set_icon(ICON_MDI_CONTROLLER);
  viewport->set_name(fmt::format("Game:{}", scene->get_scene()->scene_name));

  self.dock_should_update = true;
}

auto MainViewportPanel::add_viewport(this MainViewportPanel& self) -> ViewportPanel* {
  // We create the viewport in the pending queue,
  // this prevents on_render() from seeing it until on_update() has flushed the queue
  auto& viewport = self.pending_viewports.emplace_back(std::make_unique<ViewportPanel>());

  self.dock_should_update = true;

  return viewport.get();
}

void MainViewportPanel::on_render(vuk::ImageAttachment swapchain_attachment) {
  if (on_begin(ImGuiWindowFlags_MenuBar)) {
    if (ImGui::BeginMenuBar()) {
      if (ImGui::MenuItem(ICON_MDI_PLUS_THICK)) {
        App::mod<Editor>().new_scene();
      }
      UI::tooltip_hover("New scene");
      if (ImGui::MenuItem(ICON_MDI_FOLDER_OPEN)) {
        App::mod<Editor>().open_scene_file_dialog();
      }
      UI::tooltip_hover("Open scene");
      ImGui::EndMenuBar();
    }

    auto dockspace_id = ImGui::GetID("ViewportDockspace");

    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);

    drag_drop();

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

void MainViewportPanel::update(this MainViewportPanel& self, const Timestep& timestep, SceneHierarchyPanel* sh) {
  ZoneScoped;

  // Move pending viewports to the main list
  if (!self.pending_viewports.empty()) {
    self.viewport_panels.insert(
      self.viewport_panels.end(),
      std::make_move_iterator(self.pending_viewports.begin()),
      std::make_move_iterator(self.pending_viewports.end())
    );
    self.pending_viewports.clear();
  }

  for (const auto& panel : self.viewport_panels) {
    auto* editor_scene = panel->get_scene();

    if (panel->is_viewport_focused) {
      auto sh_scene = sh->get_scene();
      // Did scene change?
      if (sh_scene && editor_scene && sh_scene->get_id() != editor_scene->get_id())
        sh->set_scene(editor_scene);

      if (!sh_scene || !editor_scene)
        sh->set_scene(editor_scene);
    }

    if (editor_scene) {
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

  std::erase_if(self.viewport_panels, [](const std::unique_ptr<ViewportPanel>& ptr) {
    return ptr == nullptr || !ptr->visible;
  });
}

auto MainViewportPanel::update_dockspace(this MainViewportPanel& self) -> void { self.dock_should_update = true; }

auto MainViewportPanel::set_dockspace(this const MainViewportPanel& self) -> void {
  auto dock_id = ImGui::GetID("ViewportDockspace");

  for (auto& panel : self.viewport_panels) {
    ImGui::DockBuilderDockWindow(panel->get_id(), dock_id);
  }

  ImGui::DockBuilderFinish(dock_id);
}

void MainViewportPanel::drag_drop(this MainViewportPanel& self) {
  auto& editor = App::mod<Editor>();

  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* imgui_payload = ImGui::AcceptDragDropPayload(PayloadData::DRAG_DROP_SOURCE)) {
      const auto* payload = PayloadData::from_payload(imgui_payload);
      const auto path = payload->get_path();
      if (path.extension() == ".oxscene") {
        auto scene_id = editor.scene_manager.load_scene(path);
        if (scene_id.has_value()) {
          self.add_new_scene(App::mod<Editor>().scene_manager.get_scene(scene_id.value()));
        }
      }
    }

    ImGui::EndDragDropTarget();
  }
}

} // namespace ox
