#pragma once

#include "Core/Project.hpp"
#include "Panels/EditorPanelRegistry.hpp"
#include "Panels/MainViewportPanel.hpp"
#include "Panels/SceneHierarchyPanel.hpp"
#include "UI/RuntimeConsole.hpp"
#include "Utils/Command.hpp"
#include "Utils/EditorContext.hpp"
#include "Utils/EditorTheme.hpp"
#include "Utils/Notification.hpp"
#include "Utils/SceneManager.hpp"
#include "Utils/ThumbnailManager.hpp"

namespace ox {
class Editor {
public:
  constexpr static auto MODULE_NAME = "Editor";

  struct ViewportSceneLoadEvent {};

  struct ScenePlayEvent {
    SceneID scene_id;

    ScenePlayEvent(SceneID s) : scene_id(s) {}
  };

  struct SceneStopEvent {
    SceneID scene_id;

    SceneStopEvent(SceneID s) : scene_id(s) {}
  };

  enum class EditorLayout { Classic = 0, BigViewport };

  // Panels
  MainViewportPanel main_viewport_panel = {};
  EditorPanelRegistry editor_panel_registry = {};

  SceneManager scene_manager = {};

  std::unique_ptr<Project> active_project = nullptr;

  EditorTheme editor_theme;

  ThumbnailManager thumbnail_manager;

  // Layout
  ImGuiID dockspace_id;
  EditorLayout current_layout = EditorLayout::Classic;

  std::unique_ptr<UndoRedoSystem> undo_redo_system = nullptr;

  NotificationSystem notification_system = {};

  auto init(this Editor& self) -> std::expected<void, std::string>;
  auto deinit(this Editor& self) -> std::expected<void, std::string>;

  auto update(this Editor& self, const Timestep& timestep) -> void;
  auto render(this Editor& self, const vuk::ImageAttachment& swapchain_attachment) -> void;

  // Removes all viewports then adds one and resets the SceneManager
  auto reset(this Editor& self) -> void;

  auto new_scene(this Editor& self) -> void;

  // Loads the scene from the path and appends the scene to the first viewport panel
  auto open_scene(const std::filesystem::path& path) -> bool;

  auto open_scene_file_dialog() -> void;
  auto save_scene() -> void;
  auto save_scene_as() -> void;

  auto get_context() -> EditorContext& { return editor_context; }

  auto get_selected_scene() -> Scene* {
    auto* sh_scene = editor_panel_registry.get<SceneHierarchyPanel>().get_scene();
    if (sh_scene) {
      return sh_scene->get_scene().get();
    }

    return nullptr;
  }

  auto set_docking_layout(this Editor& self, EditorLayout layout) -> void;
  auto reset_current_docking_layout() -> void;

private:
  RuntimeConsole runtime_console = {};

  // Context
  EditorContext editor_context = {};

  auto save_project(const std::string& path) -> void;

  auto draw_menubar(this Editor& self) -> void;

  auto undo() const -> void;
  auto redo() const -> void;
};
} // namespace ox
