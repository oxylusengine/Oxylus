#include "EditorLayer.hpp"

#include <filesystem>
#include <imgui_internal.h>
// #include <imspinner.h>
#include <ranges>

#include "Asset/AssetManager.hpp"
#include "Core/FileSystem.hpp"
#include "Core/Input.hpp"
#include "Core/Project.hpp"
#include "EditorTheme.hpp"
#include "Panels/AssetInspectorPanel.hpp"
#include "Panels/ContentPanel.hpp"
#include "Panels/EditorSettingsPanel.hpp"
#include "Panels/InspectorPanel.hpp"
#include "Panels/ProjectPanel.hpp"
#include "Panels/RendererSettingsPanel.hpp"
#include "Panels/SceneHierarchyPanel.hpp"
#include "Panels/StatisticsPanel.hpp"
#include "Render/Window.hpp"
#include "UI/ImGuiLayer.hpp"
#include "UI/OxUI.hpp"
#include "Utils/CVars.hpp"
#include "Utils/EditorConfig.hpp"
#include "Utils/EmbeddedBanner.hpp"
#include "Utils/ImGuiScoped.hpp"
#include "Utils/Log.hpp"

namespace ox {
EditorLayer* EditorLayer::instance = nullptr;

static ViewportPanel* fullscreen_viewport_panel = nullptr;

EditorLayer::EditorLayer() : Layer("Editor Layer") { instance = this; }

void EditorLayer::on_attach() {
  OX_SCOPED_ZONE;

  editor_theme.init();

  Project::create_new();

  editor_config.load_config();

  engine_banner = create_shared<Texture>();
  engine_banner->create({},
                        {.preset = Preset::eRTT2DUnmipped,
                         .format = vuk::Format::eR8G8B8A8Unorm,
                         .mime = {},
                         .data = EngineBanner,
                         .extent = {EngineBannerWidth, EngineBannerHeight}});

  add_panel<SceneHierarchyPanel>();
  add_panel<ContentPanel>();
  add_panel<InspectorPanel>();
  add_panel<AssetInspectorPanel>();
  add_panel<EditorSettingsPanel>();
  add_panel<RendererSettingsPanel>();
  add_panel<ProjectPanel>();
  add_panel<StatisticsPanel>();

  const auto& viewport = viewport_panels.emplace_back(create_unique<ViewportPanel>());
  viewport->set_context(editor_scene, *get_panel<SceneHierarchyPanel>());

  editor_scene = create_shared<Scene>();
  load_default_scene(editor_scene);
  set_editor_context(editor_scene);

  if (auto project_arg = App::get()->get_command_line_args().get_index("project=")) {
    if (auto next_arg = App::get()->get_command_line_args().get(project_arg.value() + 1)) {
      get_panel<ProjectPanel>()->load_project_for_editor(next_arg->arg_str);
    } else {
      OX_LOG_ERROR("Project argument missing a path!");
    }
  }
}

void EditorLayer::on_detach() { editor_config.save_config(); }

void EditorLayer::on_update(const Timestep& delta_time) {
  Project::get_active()->check_module();

  for (const auto& panel : viewport_panels) {
    if (panel->fullscreen_viewport) {
      fullscreen_viewport_panel = panel.get();
      break;
    }
    fullscreen_viewport_panel = nullptr;
  }

  for (const auto& panel : editor_panels | std::views::values) {
    if (!panel->visible)
      continue;
    panel->on_update();
  }
  for (const auto& panel : viewport_panels) {
    if (!panel->visible)
      continue;
    panel->on_update();
  }

  switch (scene_state) {
    case SceneState::Edit: {
      editor_scene->on_editor_update(delta_time, viewport_panels[0]->editor_camera);
      break;
    }
    case SceneState::Play: {
      active_scene->on_runtime_update(delta_time);
      break;
    }
    case SceneState::Simulate: {
      active_scene->on_editor_update(delta_time, viewport_panels[0]->editor_camera);
      break;
    }
  }
}

void EditorLayer::on_render(const vuk::Extent3D extent,
                            const vuk::Format format) {
  if (const auto scene = get_active_scene(); active_scene)
    scene->on_render(extent, format);

  if (EditorCVar::cvar_show_style_editor.get())
    ImGui::ShowStyleEditor();
  if (EditorCVar::cvar_show_imgui_demo.get())
    ImGui::ShowDemoWindow();

  editor_shortcuts();

  constexpr ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_PassthruCentralNode;

  constexpr ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBackground |
                                            ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                                            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                            ImGuiWindowFlags_NoSavedSettings;

  ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->WorkPos);
  ImGui::SetNextWindowSize(viewport->WorkSize);
  ImGui::SetNextWindowViewport(viewport->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  if (ImGui::Begin("DockSpace", nullptr, window_flags)) {
    ImGui::PopStyleVar(3);

    // Submit the DockSpace
    const ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_DockingEnable) {
      dockspace_id = ImGui::GetID("MainDockspace");
      ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);
    }

    const float frame_height = ImGui::GetFrameHeight();

    constexpr ImGuiWindowFlags menu_flags =
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoNavFocus;

    ImVec2 frame_padding = ImGui::GetStyle().FramePadding;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {frame_padding.x, 4.0f});

    if (ImGui::BeginViewportSideBar("##PrimaryMenuBar", viewport, ImGuiDir_Up, frame_height, menu_flags)) {
      if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
          if (ImGui::MenuItem("New Scene", "Ctrl + N")) {
            new_scene();
          }
          if (ImGui::MenuItem("Open Scene", "Ctrl + O")) {
            open_scene_file_dialog();
          }
          if (ImGui::MenuItem("Save Scene", "Ctrl + S")) {
            save_scene();
          }
          if (ImGui::MenuItem("Save Scene As...", "Ctrl + Shift + S")) {
            save_scene_as();
          }
          ImGui::Separator();
          if (ImGui::MenuItem("Launcher...")) {
            get_panel<ProjectPanel>()->visible = true;
          }
          ImGui::Separator();
          if (ImGui::MenuItem("Exit")) {
            App::get()->close();
          }
          ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
          if (ImGui::MenuItem("Settings")) {
            get_panel<EditorSettingsPanel>()->visible = true;
          }
          if (ImGui::MenuItem("Reload project module")) {
            Project::get_active()->load_module();
          }
          ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Window")) {
          if (ImGui::MenuItem("Add viewport", nullptr)) {
            viewport_panels.emplace_back(create_unique<ViewportPanel>())->set_context(editor_scene, *get_panel<SceneHierarchyPanel>());
          }
          ImGui::MenuItem("Inspector", nullptr, &get_panel<InspectorPanel>()->visible);
          ImGui::MenuItem("Scene hierarchy", nullptr, &get_panel<SceneHierarchyPanel>()->visible);
          ImGui::MenuItem("Console window", nullptr, &runtime_console.visible);
          ImGui::MenuItem("Performance Overlay", nullptr, &viewport_panels[0]->performance_overlay_visible);
          ImGui::MenuItem("Statistics", nullptr, &get_panel<StatisticsPanel>()->visible);
          if (ImGui::BeginMenu("Layout")) {
            if (ImGui::MenuItem("Classic")) {
              set_docking_layout(EditorLayout::Classic);
            }
            if (ImGui::MenuItem("Big Viewport")) {
              set_docking_layout(EditorLayout::BigViewport);
            }
            ImGui::EndMenu();
          }
          ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Assets")) {
          if (ImGui::MenuItem("Asset Manager")) {
          }
          ui::tooltip_hover("WIP");
          ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
          if (ImGui::MenuItem("About")) {
          }
          ui::tooltip_hover("WIP");
          ImGui::EndMenu();
        }
        ImGui::SameLine();

        {
          // Project name text
          ImGui::SetCursorPos(
              ImVec2(ImGui::GetMainViewport()->Size.x - 10 - ImGui::CalcTextSize(Project::get_active()->get_config().name.c_str()).x, 0));
          ImGuiScoped::StyleColor b_color1(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 0.7f));
          ImGuiScoped::StyleColor b_color2(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.2f, 0.2f, 0.7f));
          ImGui::Button(Project::get_active()->get_config().name.c_str());
        }

        ImGui::EndMenuBar();
      }
      ImGui::End();
    }
    ImGui::PopStyleVar();

    for (const auto& panel : viewport_panels)
      panel->on_render(extent, format);

    for (const auto& panel : editor_panels | std::views::values) {
      if (panel->visible)
        panel->on_render(extent, format);
    }

    runtime_console.on_imgui_render();

    static bool dock_layout_initalized = false;
    if (!dock_layout_initalized) {
      set_docking_layout(current_layout);
      dock_layout_initalized = true;
    }

    ImGui::End();
  }
}

void EditorLayer::editor_shortcuts() {
  if (Input::get_key_held(KeyCode::LeftControl)) {
    if (Input::get_key_pressed(KeyCode::N)) {
      new_scene();
    }
    if (Input::get_key_pressed(KeyCode::S)) {
      save_scene();
    }
    if (Input::get_key_pressed(KeyCode::O)) {
      open_scene_file_dialog();
    }
    if (Input::get_key_held(KeyCode::LeftShift) && Input::get_key_pressed(KeyCode::S)) {
      save_scene_as();
    }
  }

  if (Input::get_key_pressed(KeyCode::F)) {
    const auto entity = get_selected_entity();
    if (entity != flecs::entity::null()) {
      const auto tc = entity.get<TransformComponent>();
      auto& camera = viewport_panels[0]->editor_camera;
      auto final_pos = tc->position + camera.forward;
      final_pos += -5.0f * camera.forward * glm::vec3(1.0f);
      camera.position = final_pos;
    }
  }
}

void EditorLayer::new_scene() {
  const Shared<Scene> new_scene = create_shared<Scene>();
  editor_scene = new_scene;
  set_editor_context(new_scene);
  last_save_scene_path.clear();
}

void EditorLayer::open_scene_file_dialog() {
  const auto& window = App::get()->get_window();
  FileDialogFilter dialog_filters[] = {{.name = "Oxylus scene file(.oxscene)", .pattern = "oxscene"}};
  window.show_dialog({
      .kind = DialogKind::OpenFile,
      .user_data = this,
      .callback =
          [](void* user_data, const c8* const* files, i32) {
    auto* layer = static_cast<EditorLayer*>(user_data);
    if (!files || !*files) {
      return;
    }

    const auto first_path_cstr = *files;
    const auto first_path_len = std::strlen(first_path_cstr);
    const auto path = std::string(first_path_cstr, first_path_len);
    if (!path.empty())
      layer->open_scene(path);
  },
      .title = "Oxylus scene file...",
      .default_path = fs::current_path(),
      .filters = dialog_filters,
      .multi_select = false,
  });
}

bool EditorLayer::open_scene(const std::filesystem::path& path) {
  if (!::fs::exists(path)) {
    OX_LOG_WARN("Could not find scene: {0}", path.filename().string());
    return false;
  }
  if (path.extension().string() != ".oxscene") {
    if (!::fs::is_directory(path))
      OX_LOG_WARN("Could not load {0} - not a scene file", path.filename().string());
    return false;
  }
  const auto new_scene = create_shared<Scene>();
  if (new_scene->load_from_file(path.string())) {
    editor_scene = new_scene;
    set_editor_context(new_scene);
  }
  last_save_scene_path = path.string();
  return true;
}

void EditorLayer::load_default_scene(const std::shared_ptr<Scene>& scene) {
  OX_SCOPED_ZONE;
  const auto sun = scene->create_entity("Sun");
  sun.get_mut<TransformComponent>()->rotation.x = glm::radians(25.f);
  sun.set<LightComponent>({.type = LightComponent::LightType::Directional, .intensity = 10.f});
}

void EditorLayer::clear_selected_entity() { get_panel<SceneHierarchyPanel>()->clear_selected_entity(); }

void EditorLayer::save_scene() {
  if (!last_save_scene_path.empty()) {
    editor_scene->save_to_file(last_save_scene_path);
  } else {
    save_scene_as();
  }
}

void EditorLayer::save_scene_as() {
  const auto& window = App::get()->get_window();
  FileDialogFilter dialog_filters[] = {{.name = "Oxylus Scene(.oxscene)", .pattern = "oxscene"}};
  window.show_dialog({
      .kind = DialogKind::SaveFile,
      .user_data = this,
      .callback =
          [](void* user_data, const c8* const* files, i32) {
    const auto layer = static_cast<EditorLayer*>(user_data);
    if (!files || !*files) {
      return;
    }

    const auto first_path_cstr = *files;
    const auto first_path_len = std::strlen(first_path_cstr);
    const auto path = std::string(first_path_cstr, first_path_len);

    if (!path.empty()) {
      layer->editor_scene->save_to_file(path);
      layer->last_save_scene_path = path;
    }
  },
      .title = "New Scene...",
      .default_path = "NewScene.oxscene",
      .filters = dialog_filters,
      .multi_select = false,
  });
}

void EditorLayer::on_scene_play() {
  reset_context();
  set_scene_state(SceneState::Play);
  active_scene = Scene::copy(editor_scene);
  set_editor_context(active_scene);
  active_scene->on_runtime_start();
}

void EditorLayer::on_scene_stop() {
  reset_context();
  set_scene_state(SceneState::Edit);
  active_scene->on_runtime_stop();
  active_scene = nullptr;
  set_editor_context(editor_scene);
}

void EditorLayer::on_scene_simulate() {
  reset_context();
  set_scene_state(SceneState::Simulate);
  active_scene = Scene::copy(editor_scene);
  set_editor_context(active_scene);
}

Shared<Scene> EditorLayer::get_active_scene() { return active_scene; }

void EditorLayer::set_editor_context(const Shared<Scene>& scene) {
  auto* shpanel = get_panel<SceneHierarchyPanel>();
  shpanel->set_scene(scene);
  for (const auto& panel : viewport_panels) {
    panel->set_context(scene, *shpanel);
  }
}

void EditorLayer::set_scene_state(const SceneState state) { scene_state = state; }

void EditorLayer::set_docking_layout(EditorLayout layout) {
  current_layout = layout;
  ImGui::DockBuilderRemoveNode(dockspace_id);
  ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_PassthruCentralNode);

  const ImVec2 size = ImGui::GetMainViewport()->WorkSize;
  ImGui::DockBuilderSetNodeSize(dockspace_id, size);

  if (layout == EditorLayout::BigViewport) {
    const ImGuiID right_dock = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Right, 0.8f, nullptr, &dockspace_id);
    ImGuiID left_dock = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.2f, nullptr, &dockspace_id);
    const ImGuiID left_split_dock = ImGui::DockBuilderSplitNode(left_dock, ImGuiDir_Down, 0.4f, nullptr, &left_dock);

    ImGui::DockBuilderDockWindow(viewport_panels[0]->get_id(), right_dock);
    ImGui::DockBuilderDockWindow(get_panel<SceneHierarchyPanel>()->get_id(), left_dock);
    ImGui::DockBuilderDockWindow(get_panel<RendererSettingsPanel>()->get_id(), left_split_dock);
    ImGui::DockBuilderDockWindow(get_panel<ContentPanel>()->get_id(), left_split_dock);
    ImGui::DockBuilderDockWindow(get_panel<InspectorPanel>()->get_id(), left_dock);
  } else if (layout == EditorLayout::Classic) {
    const ImGuiID right_dock = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Right, 0.2f, nullptr, &dockspace_id);
    ImGuiID left_dock = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.2f, nullptr, &dockspace_id);
    ImGuiID left_split_vertical_dock = ImGui::DockBuilderSplitNode(left_dock, ImGuiDir_Right, 0.8f, nullptr, &left_dock);
    const ImGuiID bottom_dock = ImGui::DockBuilderSplitNode(left_split_vertical_dock, ImGuiDir_Down, 0.3f, nullptr, &left_split_vertical_dock);
    const ImGuiID left_split_dock = ImGui::DockBuilderSplitNode(left_dock, ImGuiDir_Down, 0.4f, nullptr, &left_dock);

    ImGui::DockBuilderDockWindow(get_panel<SceneHierarchyPanel>()->get_id(), left_dock);
    ImGui::DockBuilderDockWindow(get_panel<RendererSettingsPanel>()->get_id(), left_split_dock);
    ImGui::DockBuilderDockWindow(get_panel<ContentPanel>()->get_id(), bottom_dock);
    ImGui::DockBuilderDockWindow(get_panel<InspectorPanel>()->get_id(), right_dock);
    ImGui::DockBuilderDockWindow(viewport_panels[0]->get_id(), left_split_vertical_dock);
  }

  ImGui::DockBuilderFinish(dockspace_id);
}

Archive& EditorLayer::advance_history() {
  historyPos++;

  while (static_cast<int>(history.size()) > historyPos) {
    history.pop_back();
  }

  history.emplace_back();
  history.back().set_read_mode_and_reset_pos(false);

  return history.back();
}

void EditorLayer::new_project() { Project::create_new(); }

void EditorLayer::save_project(const std::string& path) { Project::save_active(path); }
} // namespace ox
