#pragma once

#include "Core/Project.hpp"
#include "Panels/SceneHierarchyPanel.hpp"
#include "Panels/ViewportPanel.hpp"
#include "UI/RuntimeConsole.hpp"
#include "Utils/Command.hpp"
#include "Utils/EditorContext.hpp"
#include "Utils/EditorTheme.hpp"
#include "Utils/Notification.hpp"
#include "Utils/SceneManager.hpp"

namespace ox {
class Editor {
public:
  constexpr static auto MODULE_NAME = "Editor";

  struct ScenePlayEvent {
    Scene* scene;

    ScenePlayEvent(Scene* s) : scene(s) {}
  };

  struct SceneStopEvent {
    Scene* scene;

    SceneStopEvent(Scene* s) : scene(s) {}
  };

  enum class EditorLayout { Classic = 0, BigViewport };

  // Panels
  ankerl::unordered_dense::map<size_t, std::unique_ptr<EditorPanel>> editor_panels;
  std::vector<std::unique_ptr<ViewportPanel>> viewport_panels;

  template <typename T>
  auto add_panel() -> T* {
    editor_panels.emplace(typeid(T).hash_code(), std::make_unique<T>());
    return get_panel<T>();
  }

  template <typename T>
  auto get_panel() -> T* {
    const auto hash_code = typeid(T).hash_code();
    OX_ASSERT(editor_panels.contains(hash_code));
    return dynamic_cast<T*>(editor_panels[hash_code].get());
  }

  SceneManager scene_manager = {};

  std::unique_ptr<Project> active_project = nullptr;

  EditorTheme editor_theme;

  // Layout
  ImGuiID dockspace_id;
  EditorLayout current_layout = EditorLayout::Classic;

  std::unique_ptr<UndoRedoSystem> undo_redo_system = nullptr;

  NotificationSystem notification_system = {};

  auto init() -> std::expected<void, std::string>;
  auto deinit() -> std::expected<void, std::string>;

  auto update(const Timestep& timestep) -> void;
  auto render(const vuk::ImageAttachment& swapchain_attachment) -> void;

  // Removes all viewports then adds one and resets the SceneManager
  auto reset(this Editor& self) -> void;

  auto new_scene(const std::unique_ptr<ViewportPanel>& viewport) -> void;

  // Loads the scene from the path and appends the scene to the first viewport panel
  auto open_scene(const std::filesystem::path& path) -> bool;

  auto open_scene_file_dialog() -> void;
  auto save_scene() -> void;
  auto save_scene_as(std::string* last_save_path, Scene* scene) -> void;

  auto get_context() -> EditorContext& { return editor_context; }

  auto editor_shortcuts() -> void;

  auto get_selected_scene() -> Scene* { return get_panel<SceneHierarchyPanel>()->get_scene(); }

  auto set_docking_layout(EditorLayout layout) -> void;
  auto reset_current_docking_layout() -> void;

private:
  RuntimeConsole runtime_console = {};

  // Context
  EditorContext editor_context = {};

  auto save_project(const std::string& path) -> void;

  auto draw_menubar(ImGuiViewport* viewport, f32 frame_height) -> void;

  auto undo() const -> void;
  auto redo() const -> void;
};
} // namespace ox
