#pragma once

#include "Core/Layer.hpp"
#include "Core/Project.hpp"
#include "EditorContext.hpp"
#include "EditorTheme.hpp"
#include "Panels/ContentPanel.hpp"
#include "Panels/SceneHierarchyPanel.hpp"
#include "Panels/ViewportPanel.hpp"
#include "Render/Window.hpp"
#include "UI/RuntimeConsole.hpp"
#include "Utils/Archive.hpp"
#include "Utils/EditorConfig.hpp"

namespace ox {
enum class HistoryOp : uint32_t {
  Translator,    // translator interaction
  Selection,     // selection changed
  Add,           // entity added
  Delete,        // entity removed
  ComponentData, // generic component data changed
  None
};

class EditorLayer : public Layer {
public:
  enum class SceneState { Edit = 0, Play = 1, Simulate = 2 };

  enum class EditorLayout { Classic = 0, BigViewport };

  SceneState scene_state = SceneState::Edit;

  // Panels
  ankerl::unordered_dense::map<size_t, std::unique_ptr<EditorPanel>> editor_panels;
  std::vector<std::unique_ptr<ViewportPanel>> viewport_panels;

  template <typename T>
  void add_panel() {
    editor_panels.emplace(typeid(T).hash_code(), std::make_unique<T>());
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

  EditorLayer();
  ~EditorLayer() override = default;
  void on_attach() override;
  void on_detach() override;

  void on_update(const Timestep& delta_time) override;
  void on_render(vuk::Extent3D extent, vuk::Format format) override;

  void new_scene();
  void open_scene_file_dialog();
  void save_scene();
  void save_scene_as();
  void on_scene_play();
  void on_scene_stop();
  void on_scene_simulate();

  static EditorLayer* get() { return instance; }

  EditorContext& get_context() { return editor_context; }

  void editor_shortcuts();
  std::shared_ptr<Scene> get_active_scene();
  void set_editor_context(const std::shared_ptr<Scene>& scene);
  bool open_scene(const std::filesystem::path& path);
  static void load_default_scene(const std::shared_ptr<Scene>& scene);

  std::shared_ptr<Scene> get_selected_scene() { return get_panel<SceneHierarchyPanel>()->get_scene(); }

  void set_scene_state(SceneState state);
  void set_docking_layout(EditorLayout layout);

  Archive& advance_history();

private:
  // Project
  void save_project(const std::string& path);

  // Scene
  std::string last_save_scene_path{};

  RuntimeConsole runtime_console = {};

  // Config
  EditorConfig editor_config;

  // Context
  EditorContext editor_context = {};
  std::vector<Archive> history;
  int historyPos = -1;

  std::shared_ptr<Scene> editor_scene;
  std::shared_ptr<Scene> active_scene;
  static EditorLayer* instance;
};
} // namespace ox
