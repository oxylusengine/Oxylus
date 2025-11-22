#include "Editor.hpp"

#include <ImGuizmo.h>
#include <ResourceCompiler.hpp>
#include <filesystem>
#include <flecs.h>
#include <imgui_internal.h>
#include <vuk/vsl/Core.hpp>

#include "Core/App.hpp"
#include "Core/Input.hpp"
#include "Core/JobManager.hpp"
#include "Panels/AssetManagerPanel.hpp"
#include "Panels/ContentPanel.hpp"
#include "Panels/EditorSettingsPanel.hpp"
#include "Panels/InspectorPanel.hpp"
#include "Panels/ProjectPanel.hpp"
#include "Panels/SceneHierarchyPanel.hpp"
#include "Panels/TextEditorPanel.hpp"
#include "Render/Window.hpp"
#include "UI/ImGuiRenderer.hpp"
#include "UI/UI.hpp"
#include "Utils/CVars.hpp"
#include "Utils/Command.hpp"
#include "Utils/EmbeddedBanner.hpp"
#include "Utils/ImGuiScoped.hpp"
#include "Utils/Log.hpp"

namespace ox {
auto Editor::init() -> std::expected<void, std::string> {
  ZoneScoped;

  auto& job_man = App::get_job_manager();
  job_man.get_tracker().start_tracking();

  undo_redo_system = std::make_unique<UndoRedoSystem>();

  editor_theme.init();

  active_project = std::make_unique<Project>();

  engine_banner = std::make_shared<Texture>();
  engine_banner->create(
    {},
    {.preset = Preset::eRTT2DUnmipped,
     .format = vuk::Format::eR8G8B8A8Srgb,
     .mime = {},
     .loaded_data = editor_banner,
     .extent = vuk::Extent3D{.width = editor_bannerWidth, .height = editor_bannerHeight, .depth = 1u}}
  );

  auto scene_hierarchy_panel = add_panel<SceneHierarchyPanel>();
  add_panel<ContentPanel>();
  add_panel<InspectorPanel>();
  add_panel<EditorSettingsPanel>();
  add_panel<ProjectPanel>();
  add_panel<AssetManagerPanel>();
  auto text_editor_panel = add_panel<TextEditorPanel>();

  scene_hierarchy_panel->viewer.opened_script_callback = [text_editor_panel](const UUID& uuid) {
    auto& asset_man = App::mod<AssetManager>();
    auto asset = asset_man.get_asset(uuid);
    if (asset) {
      text_editor_panel->visible = true;
      text_editor_panel->text_editor.open_file(asset->path);
    }
  };

  const auto& viewport = viewport_panels.emplace_back(std::make_unique<ViewportPanel>());
  viewport->set_context(editor_scene.get(), *get_panel<SceneHierarchyPanel>());

  editor_scene = std::make_shared<Scene>();
  load_default_scene(editor_scene);
  set_editor_context(editor_scene);

  if (auto project_arg = App::get()->get_command_line_args().get_index("project=")) {
    if (auto next_arg = App::get()->get_command_line_args().get(project_arg.value() + 1)) {
      get_panel<ProjectPanel>()->load_project_for_editor(next_arg.value());
    } else {
      OX_LOG_ERROR("Project argument missing a path!");
    }
  }

  return {};
}

auto Editor::deinit() -> std::expected<void, std::string> {
  auto& job_man = App::get_job_manager();
  job_man.get_tracker().stop_tracking();

  return {};
}

auto Editor::update(const Timestep& timestep) -> void {
  ZoneScoped;

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
      editor_scene->disable_phases({flecs::PreUpdate, flecs::OnUpdate});
      editor_scene->runtime_update(timestep);
      break;
    }
    case SceneState::Play: {
      active_scene->enable_all_phases();
      active_scene->runtime_update(timestep);
      break;
    }
  }

  auto& vk_context = App::get_vkcontext();
  auto& imgui_renderer = App::mod<ImGuiRenderer>();
  auto& window = App::get_window();

  auto swapchain_attachment = vk_context.new_frame();
  swapchain_attachment = vuk::clear_image(std::move(swapchain_attachment), vuk::Black<f32>);

  vuk::Format format = swapchain_attachment->format;
  vuk::Extent3D extent = swapchain_attachment->extent;

  imgui_renderer.begin_frame(timestep.get_seconds(), {window.get_logical_width(), window.get_logical_height()});

  auto sc_info = vuk::ImageAttachment{
    .image_type = swapchain_attachment->image_type,
    .extent = swapchain_attachment->extent,
    .format = swapchain_attachment->format,
    .sample_count = swapchain_attachment->sample_count,
    .base_level = swapchain_attachment->base_level,
    .level_count = swapchain_attachment->level_count,
    .base_layer = swapchain_attachment->base_layer,
    .layer_count = swapchain_attachment->layer_count,
  };

  render(sc_info);

  swapchain_attachment = imgui_renderer.end_frame(vk_context, std::move(swapchain_attachment));

  vk_context.end_frame(swapchain_attachment);
}

auto Editor::render(vuk::ImageAttachment swapchain_attachment) -> void {
  if (active_scene)
    active_scene->on_render(swapchain_attachment.extent, swapchain_attachment.format);

  auto& job_man = App::get_job_manager();

  auto status = job_man.get_tracker().get_status();
  for (const auto& [name, is_working] : status) {
    if (name == "Completion callback")
      continue;

    Notification notification(name, !is_working);
    notification_system.add(std::move(notification));
  }

  job_man.get_tracker().cleanup_old();
  notification_system.draw();

  if (EditorCVar::cvar_show_style_editor.get())
    ImGui::ShowStyleEditor();
  if (EditorCVar::cvar_show_imgui_demo.get())
    ImGui::ShowDemoWindow();

  editor_shortcuts();

  constexpr ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_PassthruCentralNode;

  constexpr ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking |
                                            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNavFocus |
                                            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                                            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings;

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

    ViewportPanel* fullscreen_viewport_panel = nullptr;
    for (const auto& panel : viewport_panels) {
      if (panel->fullscreen_viewport) {
        fullscreen_viewport_panel = panel.get();
        break;
      }
      fullscreen_viewport_panel = nullptr;
    }

    if (fullscreen_viewport_panel != nullptr) {
      fullscreen_viewport_panel->on_render(swapchain_attachment);
    } else {
      for (const auto& panel : viewport_panels)
        panel->on_render(swapchain_attachment);

      for (const auto& panel : editor_panels | std::views::values) {
        if (panel->visible)
          panel->on_render(swapchain_attachment);
      }
    }

    runtime_console.on_imgui_render();

    const float frame_height = ImGui::GetFrameHeight();

    ImVec2 frame_padding = ImGui::GetStyle().FramePadding;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {frame_padding.x, 4.0f});
    draw_menubar(viewport, frame_height);
    ImGui::PopStyleVar();

    static bool dock_layout_initalized = false;
    if (!dock_layout_initalized) {
      set_docking_layout(current_layout);
      dock_layout_initalized = true;
    }

    ImGui::End();
  }
}

void Editor::draw_menubar(ImGuiViewport* viewport, f32 frame_height) {
  constexpr ImGuiWindowFlags menu_flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                          ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoNavFocus;

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
          App::get()->should_stop();
        }
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("Edit")) {
        ImGui::BeginDisabled(undo_redo_system->get_undo_count() < 1);
        if (ImGui::MenuItem("Undo", "Ctrl + Z")) {
          undo();
        }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(undo_redo_system->get_redo_count() < 1);
        if (ImGui::MenuItem("Redo", "Ctrl + Y")) {
          redo();
        }
        ImGui::EndDisabled();
        if (ImGui::MenuItem("Settings")) {
          get_panel<EditorSettingsPanel>()->visible = true;
        }
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("Window")) {
        if (ImGui::MenuItem("Add viewport", nullptr)) {
          viewport_panels.emplace_back(std::make_unique<ViewportPanel>())
            ->set_context(editor_scene.get(), *get_panel<SceneHierarchyPanel>());
        }
        ImGui::MenuItem("Inspector", nullptr, &get_panel<InspectorPanel>()->visible);
        ImGui::MenuItem("Scene hierarchy", nullptr, &get_panel<SceneHierarchyPanel>()->visible);
        ImGui::MenuItem("Console window", nullptr, &runtime_console.visible);
        ImGui::MenuItem("Performance Overlay", nullptr, &viewport_panels[0]->performance_overlay_visible);
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
          get_panel<AssetManagerPanel>()->visible = true;
        }
        UI::tooltip_hover("WIP");
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About")) {
        }
        UI::tooltip_hover("WIP");
        ImGui::EndMenu();
      }
      ImGui::SameLine();

      {
        // Project name text
        const std::string& project_name = active_project->get_config().name;
        ImGui::SetCursorPos(
          ImVec2(ImGui::GetMainViewport()->Size.x - 10 - ImGui::CalcTextSize(project_name.c_str()).x, 0)
        );
        ImGuiScoped::StyleColor b_color1(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 0.7f));
        ImGuiScoped::StyleColor b_color2(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.2f, 0.2f, 0.7f));
        ImGui::Button(project_name.c_str());
      }

      ImGui::EndMenuBar();
    }
    ImGui::End();
  }
}

void Editor::editor_shortcuts() {
  auto& input_sys = App::mod<Input>();
  if (input_sys.get_key_held(KeyCode::LeftControl)) {
    if (input_sys.get_key_pressed(KeyCode::Z)) {
      undo();
    }
    if (input_sys.get_key_pressed(KeyCode::Y)) {
      redo();
    }
    if (input_sys.get_key_pressed(KeyCode::N)) {
      new_scene();
    }
    if (input_sys.get_key_pressed(KeyCode::S)) {
      save_scene();
    }
    if (input_sys.get_key_pressed(KeyCode::O)) {
      open_scene_file_dialog();
    }
    if (input_sys.get_key_held(KeyCode::LeftShift) && input_sys.get_key_pressed(KeyCode::S)) {
      save_scene_as();
    }
  }
}

void Editor::new_scene() {
  const std::shared_ptr<Scene> new_scene = std::make_shared<Scene>(editor_scene->scene_name);
  editor_scene = new_scene;
  set_editor_context(new_scene);
  load_default_scene(new_scene);
  last_save_scene_path.clear();
}

void Editor::open_scene_file_dialog() {
  const auto& window = App::get_window();
  FileDialogFilter dialog_filters[] = {{.name = "Oxylus scene file(.oxscene)", .pattern = "oxscene"}};
  window.show_dialog({
    .kind = DialogKind::OpenFile,
    .user_data = this,
    .callback =
      [](void* user_data, const c8* const* files, i32) {
        auto* layer = static_cast<Editor*>(user_data);
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
    .default_path = std::filesystem::current_path(),
    .filters = dialog_filters,
    .multi_select = false,
  });
}

bool Editor::open_scene(const std::filesystem::path& path) {
  if (!std::filesystem::exists(path)) {
    OX_LOG_WARN("Could not find scene: {0}", path.filename());
    return false;
  }
  if (path.extension() != ".oxscene") {
    if (!std::filesystem::is_directory(path))
      OX_LOG_WARN("Could not load {0} - not a scene file", path.filename());
    return false;
  }

  auto& job_man = App::get_job_manager();
  job_man.wait();

  const auto new_scene = std::make_shared<Scene>(editor_scene->scene_name);
  if (new_scene->load_from_file(path)) {
    editor_scene = new_scene;
    set_editor_context(new_scene);
  }
  last_save_scene_path = path;
  return true;
}

void Editor::load_default_scene(const std::shared_ptr<Scene>& scene) {
  ZoneScoped;
  const auto sun = scene->create_entity("sun", true);
  sun.get_mut<TransformComponent>().rotation.x = glm::radians(90.f);
  sun.get_mut<TransformComponent>().rotation.y = glm::radians(45.f);
  sun.set<LightComponent>({.type = LightComponent::LightType::Directional, .intensity = 10.f});
  sun.add<AtmosphereComponent>();
  const auto camera = scene->create_entity("camera", true);
  camera.add<CameraComponent>();
}

void Editor::save_scene() {
  if (!last_save_scene_path.empty()) {
    auto& job_man = App::get_job_manager();
    job_man.push_job_name("Saving scene");
    job_man.submit(Job::create([s = editor_scene.get(), p = last_save_scene_path]() { s->save_to_file(p); }));
    job_man.pop_job_name();
  } else {
    save_scene_as();
  }
}

void Editor::save_scene_as() {
  const auto& window = App::get()->get_window();
  FileDialogFilter dialog_filters[] = {{.name = "Oxylus Scene(.oxscene)", .pattern = "oxscene"}};
  window.show_dialog({
    .kind = DialogKind::SaveFile,
    .user_data = this,
    .callback =
      [](void* user_data, const c8* const* files, i32) {
        const auto layer = static_cast<Editor*>(user_data);
        if (!files || !*files) {
          return;
        }

        const auto first_path_cstr = *files;
        const auto first_path_len = std::strlen(first_path_cstr);
        const auto path = std::string(first_path_cstr, first_path_len);

        if (!path.empty()) {
          auto& job_man = App::get_job_manager();
          job_man.push_job_name("Saving scene");
          job_man.submit(Job::create([s = layer->editor_scene.get(), path]() { s->save_to_file(path); }));
          job_man.pop_job_name();

          layer->last_save_scene_path = path;
        }
      },
    .title = "New Scene...",
    .default_path = "NewScene.oxscene",
    .filters = dialog_filters,
    .multi_select = false,
  });
}

void Editor::on_scene_play() {
  ZoneScoped;

  editor_context.reset();
  set_scene_state(SceneState::Play);
  active_scene = Scene::copy(editor_scene);
  set_editor_context(active_scene);
  active_scene->runtime_start();

  editor_scene->reset_renderer_instance();
}

void Editor::on_scene_stop() {
  editor_context.reset();
  set_scene_state(SceneState::Edit);
  active_scene.reset();

  set_editor_context(editor_scene);

  editor_scene->world
    .query_builder() //
    .with<TransformComponent>()
    .build()
    .each([](flecs::entity e) { e.modified<TransformComponent>(); });
}

Scene* Editor::get_active_scene() { return active_scene.get(); }

void Editor::set_editor_context(const std::shared_ptr<Scene>& scene) {
  scene->meshes_dirty = true;
  auto* shpanel = get_panel<SceneHierarchyPanel>();
  shpanel->set_scene(scene.get());
  for (const auto& panel : viewport_panels) {
    panel->set_context(scene.get(), *shpanel);
  }
}

void Editor::set_scene_state(const SceneState state) { scene_state = state; }

void Editor::set_docking_layout(EditorLayout layout) {
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
    ImGui::DockBuilderDockWindow(get_panel<ContentPanel>()->get_id(), left_split_dock);
    ImGui::DockBuilderDockWindow(get_panel<InspectorPanel>()->get_id(), left_dock);
  } else if (layout == EditorLayout::Classic) {
    const ImGuiID right_dock = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Right, 0.2f, nullptr, &dockspace_id);
    ImGuiID left_dock = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.85f, nullptr, &dockspace_id);
    const ImGuiID left_bottom_dock = ImGui::DockBuilderSplitNode(left_dock, ImGuiDir_Down, 0.3f, nullptr, &left_dock);
    const ImGuiID
      left_vertical_split_dock = ImGui::DockBuilderSplitNode(left_dock, ImGuiDir_Left, 0.2f, nullptr, &left_dock);

    ImGui::DockBuilderDockWindow(get_panel<InspectorPanel>()->get_id(), right_dock);
    ImGui::DockBuilderDockWindow(viewport_panels[0]->get_id(), left_dock);
    ImGui::DockBuilderDockWindow(get_panel<ContentPanel>()->get_id(), left_bottom_dock);
    ImGui::DockBuilderDockWindow(get_panel<SceneHierarchyPanel>()->get_id(), left_vertical_split_dock);
  }

  ImGui::DockBuilderFinish(dockspace_id);
}

void Editor::undo() {
  ZoneScoped;
  undo_redo_system->undo();
}

void Editor::redo() {
  ZoneScoped;
  undo_redo_system->redo();
}
} // namespace ox
