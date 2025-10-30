#pragma once

#include "Core/Project.hpp"
#include "EditorContext.hpp"
#include "EditorTheme.hpp"
#include "Notification.hpp"
#include "Panels/ContentPanel.hpp"
#include "Panels/SceneHierarchyPanel.hpp"
#include "Panels/ViewportPanel.hpp"
#include "UI/RuntimeConsole.hpp"
#include "Utils/Command.hpp"
#include "Utils/EditorConfig.hpp"

namespace ox {
class Editor {
public:
  constexpr static auto MODULE_NAME = "Editor";

  enum class SceneState { Edit = 0, Play = 1 };

  enum class EditorLayout { Classic = 0, BigViewport };

  SceneState scene_state = SceneState::Edit;

  // Panels
  ankerl::unordered_dense::map<size_t, std::unique_ptr<EditorPanel>> editor_panels;
  std::vector<std::unique_ptr<ViewportPanel>> viewport_panels;

  template <typename T>
  T* add_panel() {
    editor_panels.emplace(typeid(T).hash_code(), std::make_unique<T>());
    return get_panel<T>();
  }

  template <typename T>
  T* get_panel() {
    const auto hash_code = typeid(T).hash_code();
    OX_ASSERT(editor_panels.contains(hash_code));
    return dynamic_cast<T*>(editor_panels[hash_code].get());
  }

  std::unique_ptr<Project> active_project = nullptr;

  EditorTheme editor_theme;

  // Logo
  std::shared_ptr<Texture> engine_banner = nullptr;

  // Layout
  ImGuiID dockspace_id;
  EditorLayout current_layout = EditorLayout::Classic;

  std::unique_ptr<UndoRedoSystem> undo_redo_system = nullptr;

  NotificationSystem notification_system = {};

  auto init() -> std::expected<void, std::string>;
  auto deinit() -> std::expected<void, std::string>;

  auto update(const Timestep& timestep) -> void;
  auto render(vuk::Extent3D extent, vuk::Format format) -> void;

  void new_scene();
  void open_scene_file_dialog();
  void save_scene();
  void save_scene_as();
  void on_scene_play();
  void on_scene_stop();

  EditorContext& get_context() { return editor_context; }

  void editor_shortcuts();
  Scene* get_active_scene();
  void set_editor_context(const std::shared_ptr<Scene>& scene);
  bool open_scene(const std::filesystem::path& path);
  static void load_default_scene(const std::shared_ptr<Scene>& scene);

  Scene* get_selected_scene() { return get_panel<SceneHierarchyPanel>()->get_scene(); }

  void set_scene_state(SceneState state);
  void set_docking_layout(EditorLayout layout);

private:
  // Scene
  std::filesystem::path last_save_scene_path{};

  RuntimeConsole runtime_console = {};

  // Config
  EditorConfig editor_config;

  // Context
  EditorContext editor_context = {};

  std::shared_ptr<Scene> editor_scene;
  std::shared_ptr<Scene> active_scene;

  void save_project(const std::string& path);

  void undo();
  void redo();
};
} // namespace ox
