#pragma once

#include "Core/EventSystem.hpp"
#include "Panels/EditorPanelRegistry.hpp"
#include "Panels/MainViewportPanel.hpp"
#include "Panels/SceneHierarchyPanel.hpp"
#include "Project/Project.hpp"
#include "Utils/Command.hpp"
#include "Utils/EditorCVar.hpp"
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

  EditorCVar editor_cvar;

  // Panels
  MainViewportPanel main_viewport_panel = {};
  EditorPanelRegistry editor_panel_registry = {};

  SceneManager scene_manager = {};

  std::unique_ptr<Project> active_project = nullptr;
  // Set when a project opens, consumed once its asset scan has registered everything the scene names.
  option<std::filesystem::path> pending_start_scene = nullopt;

  EditorTheme editor_theme;

  ThumbnailManager thumbnail_manager;

  // Layout
  ImGuiID dockspace_id;
  EditorLayout current_layout = EditorLayout::Classic;

  std::unique_ptr<UndoRedoSystem> undo_redo_system = nullptr;

  NotificationSystem notification_system = {};

  HandlerId scene_play_handler = {};
  HandlerId scene_stop_handler = {};

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
  // Context
  EditorContext editor_context = {};

  auto save_project(const std::string& path) -> void;

  // Terrain brush edits have to come back off the GPU before the save job can write them, so the
  // whole save is deferred to the main thread and only the file write runs on a worker.
  static auto submit_scene_save(EditorScene* scene, std::filesystem::path path) -> void;

  // Points the terrain at an edits asset (creating one next to the scene the first time it is
  // painted) and refreshes its payload from the GPU. Returns the file the asset exports to.
  static auto sync_terrain_edits_asset(Scene& scene, const std::filesystem::path& scene_path) -> std::filesystem::path;

  auto draw_menubar(this Editor& self) -> void;
  void draw_bottom_toolbar(this Editor& self, float height);

  auto undo() const -> void;
  auto redo() const -> void;
};
} // namespace ox
