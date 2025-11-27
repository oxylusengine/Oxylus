#include "Editor.hpp"

#include <ImGuizmo.h>
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
#include "Utils/EditorConfig.hpp"

namespace ox {
auto Editor::init(this Editor& self) -> std::expected<void, std::string> {
  ZoneScoped;

  auto& job_man = App::get_job_manager();
  job_man.get_tracker().start_tracking();

  self.undo_redo_system = std::make_unique<UndoRedoSystem>();

  self.editor_theme.init();

  self.active_project = std::make_unique<Project>();

  auto scene_hierarchy_panel = self.add_panel<SceneHierarchyPanel>();
  self.add_panel<ContentPanel>();
  self.add_panel<InspectorPanel>();
  self.add_panel<EditorSettingsPanel>();
  self.add_panel<ProjectPanel>();
  self.add_panel<AssetManagerPanel>();
  auto text_editor_panel = self.add_panel<TextEditorPanel>();

  scene_hierarchy_panel->viewer.opened_script_callback = [text_editor_panel](const UUID& uuid) {
    auto& asset_man = App::mod<AssetManager>();
    auto* asset = asset_man.get_asset(uuid);
    if (asset) {
      text_editor_panel->visible = true;
      text_editor_panel->text_editor.open_file(asset->path);
    }
  };

  self.main_viewport_panel.init();

  auto& event_system = App::get_event_system();
  std::ignore = event_system.subscribe<ScenePlayEvent>([&self](const ScenePlayEvent& e) {
    self.editor_context.reset();
    auto* sh = self.get_panel<SceneHierarchyPanel>();
    sh->set_scene(nullptr);
  });
  std::ignore = event_system.subscribe<SceneStopEvent>([&self](const SceneStopEvent& e) {
    self.editor_context.reset();
    auto* sh = self.get_panel<SceneHierarchyPanel>();
    sh->set_scene(nullptr);
  });

  Log::add_callback(
    "editor_notifications",
    [](void* user_data, const loguru::Message& message) {
      auto* e = reinterpret_cast<Editor*>(user_data);
      auto notification = Notification(message.message, true);
      e->notification_system.add(std::move(notification));
    },
    &self,
    loguru::Verbosity_INFO
  );

  return {};
}

auto Editor::deinit(this Editor& self) -> std::expected<void, std::string> {
  auto& job_man = App::get_job_manager();
  job_man.get_tracker().stop_tracking();

  Log::remove_callback("editor_notifications");

  return {};
}

auto Editor::update(this Editor& self, const Timestep& timestep) -> void {
  ZoneScoped;

  for (const auto& panel : self.editor_panels | std::views::values) {
    if (!panel->visible)
      continue;
    panel->on_update();
  }

  self.main_viewport_panel.update(timestep, self.get_panel<SceneHierarchyPanel>());

  auto& vk_context = App::get_vkcontext();
  auto& imgui_renderer = App::mod<ImGuiRenderer>();
  auto& window = App::get_window();

  auto swapchain_attachment = vk_context.new_frame();
  swapchain_attachment = vuk::clear_image(std::move(swapchain_attachment), vuk::Black<f32>);

  imgui_renderer.begin_frame(timestep.get_seconds(), {window.get_logical_width(), window.get_logical_height()});

  const auto sc_info = vuk::ImageAttachment{
    .image_type = swapchain_attachment->image_type,
    .extent = swapchain_attachment->extent,
    .format = swapchain_attachment->format,
    .sample_count = swapchain_attachment->sample_count,
    .base_level = swapchain_attachment->base_level,
    .level_count = swapchain_attachment->level_count,
    .base_layer = swapchain_attachment->base_layer,
    .layer_count = swapchain_attachment->layer_count,
  };

  self.render(sc_info);

  swapchain_attachment = imgui_renderer.end_frame(vk_context, std::move(swapchain_attachment));

  vk_context.end_frame(swapchain_attachment);
}

auto Editor::render(this Editor& self, const vuk::ImageAttachment& swapchain_attachment) -> void {
  auto& job_man = App::get_job_manager();

  auto status = job_man.get_tracker().get_status();
  for (const auto& [name, is_working] : status) {
    if (name == "Completion callback")
      continue;

    Notification notification(name, !is_working);
    self.notification_system.add(std::move(notification));
  }

  job_man.get_tracker().cleanup_old();
  self.notification_system.draw();

  if (EditorCVar::cvar_show_style_editor.get())
    ImGui::ShowStyleEditor();
  if (EditorCVar::cvar_show_imgui_demo.get())
    ImGui::ShowDemoWindow();

  self.editor_shortcuts();

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

    const ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_DockingEnable) {
      self.dockspace_id = ImGui::GetID("MainDockspace");
      ImGui::DockSpace(self.dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);
    }

    self.main_viewport_panel.on_render(swapchain_attachment);

    for (const auto& panel : self.editor_panels | std::views::values) {
      if (panel->visible)
        panel->on_render(swapchain_attachment);
    }

    self.runtime_console.on_imgui_render();

    self.draw_menubar();

    static bool dock_layout_initalized = false;
    if (!dock_layout_initalized) {
      self.set_docking_layout(self.current_layout);
      dock_layout_initalized = true;
    }
  }

  ImGui::End();
}

void Editor::reset(this Editor& self) {
  self.main_viewport_panel.reset();

  auto* sh = self.get_panel<SceneHierarchyPanel>();
  sh->set_scene(nullptr);

  self.scene_manager.reset();
}

void Editor::new_scene(this Editor& self) {
  App::get_vkcontext().wait();

  auto new_scene_id = self.scene_manager.new_scene();
  self.scene_manager.load_default_scene(new_scene_id);
  auto scene = self.scene_manager.get_scene(new_scene_id);

  self.main_viewport_panel.add_new_scene(scene);
}

bool Editor::open_scene(const std::filesystem::path& path) {
  auto loaded_scene = scene_manager.load_scene(path);

  if (loaded_scene.has_value()) {
    auto scene = scene_manager.get_scene(loaded_scene.value());
    main_viewport_panel.add_new_scene(scene);
  }

  return loaded_scene.has_value();
}

void Editor::open_scene_file_dialog() {
  const auto& window = App::get_window();
  FileDialogFilter dialog_filters[] = {{.name = "Oxylus scene file(.oxscene)", .pattern = "oxscene"}};
  window.show_dialog({
    .kind = DialogKind::OpenFile,
    .user_data = this,
    .callback =
      [](void* user_data, const c8* const* files, i32) {
        auto* e = static_cast<Editor*>(user_data);
        if (!files || !*files) {
          return;
        }

        const auto first_path_cstr = *files;
        const auto first_path_len = std::strlen(first_path_cstr);
        const auto path = std::string(first_path_cstr, first_path_len);
        if (!path.empty())
          e->open_scene(path);
      },
    .title = "Oxylus scene file...",
    .default_path = std::filesystem::current_path(),
    .filters = dialog_filters,
    .multi_select = false,
  });
}

void Editor::save_scene() {
  auto* focused_viewport = main_viewport_panel.get_focused_viewport();
  if (!focused_viewport)
    return;

  auto* scene = focused_viewport->get_scene();

  if (!scene || scene->is_playing()) {
    return;
  }

  if (!scene->get_path().empty()) {
    auto& job_man = App::get_job_manager();
    job_man.push_job_name("Saving scene");
    job_man.submit(Job::create([scene] {
      auto last_saved_path = scene->get_path();
      scene->get_scene()->save_to_file(last_saved_path);
    }));
    job_man.pop_job_name();
  } else {
    save_scene_as();
  }
}

void Editor::save_scene_as() {
  auto* focused_viewport = main_viewport_panel.get_focused_viewport();
  if (!focused_viewport)
    return;

  auto* focused_viewport_scene = focused_viewport->get_scene();
  if (!focused_viewport_scene || focused_viewport_scene->is_playing())
    return;

  const auto& window = App::get_window();
  FileDialogFilter dialog_filters[] = {{.name = "Oxylus Scene(.oxscene)", .pattern = "oxscene"}};
  struct UData {
    EditorScene* scene = {};
  };

  const auto u_data = new UData{.scene = focused_viewport_scene};

  window.show_dialog({
    .kind = DialogKind::SaveFile,
    .user_data = u_data,
    .callback =
      [](void* user_data, const c8* const* files, i32) {
        const auto udata = static_cast<UData*>(user_data);
        if (!udata && (!files || !*files)) {
          return;
        }

        const auto first_path_cstr = *files;
        const auto first_path_len = std::strlen(first_path_cstr);
        const auto path = std::string(first_path_cstr, first_path_len);

        if (!path.empty()) {
          auto& job_man = App::get_job_manager();
          job_man.push_job_name("Saving scene");
          job_man.submit(Job::create([s = udata->scene, path] {
            s->get_scene()->save_to_file(path);
            s->set_path(path);
          }));
          job_man.pop_job_name();
        }

        delete udata;
      },
    .title = "New Scene...",
    .default_path = "NewScene.oxscene",
    .filters = dialog_filters,
    .multi_select = false,
  });
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

    ImGui::DockBuilderDockWindow(main_viewport_panel.get_id(), right_dock);
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
    ImGui::DockBuilderDockWindow(main_viewport_panel.get_id(), left_dock);
    ImGui::DockBuilderDockWindow(get_panel<ContentPanel>()->get_id(), left_bottom_dock);
    ImGui::DockBuilderDockWindow(get_panel<SceneHierarchyPanel>()->get_id(), left_vertical_split_dock);
  }

  ImGui::DockBuilderFinish(dockspace_id);
}

void Editor::reset_current_docking_layout() {
  set_docking_layout(current_layout);

  main_viewport_panel.update_dockspace();
}

void Editor::draw_menubar() {
  if (ImGui::BeginMenuBar()) {
    if (ImGui::BeginMenu("File")) {
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
        main_viewport_panel.add_viewport();
      }
      ImGui::MenuItem("Inspector", nullptr, &get_panel<InspectorPanel>()->visible);
      ImGui::MenuItem("Scene hierarchy", nullptr, &get_panel<SceneHierarchyPanel>()->visible);
      ImGui::MenuItem("Console window", nullptr, &runtime_console.visible);
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
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 0.7f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.2f, 0.2f, 0.7f));
      ImGui::Button(project_name.c_str());
      ImGui::PopStyleColor(2);
    }

    ImGui::EndMenuBar();
  }
}

void Editor::undo() const {
  ZoneScoped;
  undo_redo_system->undo();
}

void Editor::redo() const {
  ZoneScoped;
  undo_redo_system->redo();
}
} // namespace ox
