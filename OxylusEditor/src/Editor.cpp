#include "Editor.hpp"

#include <ImGuizmo.h>
#include <filesystem>
#include <icons/IconsMaterialDesignIcons.h>
#include <imgui_internal.h>
#include <implot.h>
#include <vuk/vsl/Core.hpp>

#include "Core/App.hpp"
#include "Core/Enum.hpp"
#include "Core/Input.hpp"
#include "Core/JobManager.hpp"
#include "Panels/ActivityLogPanel.hpp"
#include "Panels/AnimationEditorPanel.hpp"
#include "Panels/AssetManagerPanel.hpp"
#include "Panels/ContentPanel.hpp"
#include "Panels/EditorSettingsPanel.hpp"
#include "Panels/InspectorPanel.hpp"
#include "Panels/ParticleEditorPanel.hpp"
#include "Panels/ProjectPanel.hpp"
#include "Panels/SceneHierarchyPanel.hpp"
#include "Panels/TextEditorPanel.hpp"
#include "Render/Window.hpp"
#include "UI/ImGuiRenderer.hpp"
#include "UI/UI.hpp"
#include "Utils/Command.hpp"

namespace ox {
auto Editor::init(this Editor& self) -> std::expected<void, std::string> {
  ZoneScoped;

  ImPlot::CreateContext();

  auto& job_man = App::get_job_manager();
  job_man.get_tracker().start_tracking();

  self.undo_redo_system = std::make_unique<UndoRedoSystem>();

  self.editor_theme.init();

  auto& input = App::mod<Input>();
  input.set_context("editor");
  std::ignore = input.bind_action(
    ActionBinding{
      .action_id = "new_scene",
      .primary_inputs = {InputCode(ScanCode::N, ModCode::AnyControl)},
      .context = "editor",
      .on_pressed_callback = [&self](const ActionContext&) { self.new_scene(); }
    }
  );
  std::ignore = input.bind_action(
    ActionBinding{
      .action_id = "undo",
      .primary_inputs = {InputCode(ScanCode::Z, ModCode::AnyControl)},
      .context = "editor",
      .on_pressed_callback = [&self](const ActionContext&) { self.undo(); }
    }
  );
  std::ignore = input.bind_action(
    ActionBinding{
      .action_id = "redo",
      .primary_inputs = {InputCode(ScanCode::Y, ModCode::AnyControl)},
      .context = "editor",
      .on_pressed_callback = [&self](const ActionContext&) { self.redo(); }
    }
  );
  std::ignore = input.bind_action(
    ActionBinding{
      .action_id = "save_scene",
      .primary_inputs = {InputCode(ScanCode::S, ModCode::AnyControl)},
      .context = "editor",
      .on_pressed_callback = [&self](const ActionContext&) { self.save_scene(); }
    }
  );
  std::ignore = input.bind_action(
    ActionBinding{
      .action_id = "open_scene_file_dialog",
      .primary_inputs = {InputCode(ScanCode::O, ModCode::AnyControl)},
      .context = "editor",
      .on_pressed_callback = [&self](const ActionContext&) { self.open_scene_file_dialog(); }
    }
  );
  std::ignore = input.bind_action(
    ActionBinding{
      .action_id = "save_scene_as",
      .primary_inputs = {InputCode(ScanCode::S, ModCode::AnyControl | ModCode::AnyShift)},
      .context = "editor",
      .on_pressed_callback = [&self](const ActionContext&) { self.save_scene_as(); }
    }
  );
  std::ignore = input.bind_action(
    ActionBinding{
      .action_id = "fullscreen_viewport",
      .primary_inputs = {InputCode(ScanCode::F11)},
      .context = "editor",
      .on_pressed_callback = [&self](const ActionContext&) { self.main_viewport_panel.toggle_fullscreen(); }
    }
  );

  self.active_project = std::make_unique<Project>();

  auto scene_hierarchy_panel = self.editor_panel_registry.add<SceneHierarchyPanel>();
  self.editor_panel_registry.add<ContentPanel>();
  self.editor_panel_registry.add<InspectorPanel>();
  self.editor_panel_registry.add<EditorSettingsPanel>();
  self.editor_panel_registry.add<ProjectPanel>();
  self.editor_panel_registry.add<AssetManagerPanel>();
  self.editor_panel_registry.add<ParticleEditorPanel>();
  self.editor_panel_registry.add<AnimationEditorPanel>();
  auto activity_log_panel = self.editor_panel_registry.add<ActivityLogPanel>();
  activity_log_panel->set_system(&self.notification_system);
  auto text_editor_panel = self.editor_panel_registry.add<TextEditorPanel>();

  scene_hierarchy_panel->viewer.opened_script_callback = [text_editor_panel](const UUID& uuid) {
    auto& asset_man = App::mod<AssetManager>();
    auto asset = asset_man.get_asset(uuid);
    if (asset) {
      text_editor_panel->visible = true;
      text_editor_panel->text_editor.open_file(asset->path);
    }
  };

  self.main_viewport_panel.init();

  auto& event_system = App::get_event_system();
  self.scene_play_handler = event_system
                              .subscribe<ScenePlayEvent>([&self](const ScenePlayEvent& e) {
                                self.editor_context.reset();
                                auto& sh = self.editor_panel_registry.get<SceneHierarchyPanel>();
                                sh.set_scene(nullptr);
                              })
                              .value_or(0);
  self.scene_stop_handler = event_system
                              .subscribe<SceneStopEvent>([&self](const SceneStopEvent& e) {
                                self.scene_manager.remove_scene(e.scene_id);
                                self.editor_context.reset();
                                auto& sh = self.editor_panel_registry.get<SceneHierarchyPanel>();
                                sh.set_scene(nullptr);
                              })
                              .value_or(0);

  Log::add_callback(
    "editor_notifications",
    [](void* user_data, const loguru::Message& message) {
      auto* e = reinterpret_cast<Editor*>(user_data);
      auto type = Notification::Type::Info;
      if (message.verbosity == loguru::NamedVerbosity::Verbosity_ERROR) {
        type = Notification::Type::Error;
      } else if (message.verbosity == loguru::NamedVerbosity::Verbosity_WARNING) {
        type = Notification::Type::Warn;
      } else if (message.verbosity == loguru::NamedVerbosity::Verbosity_INFO || message.verbosity > 0) {
        type = Notification::Type::Info;
      }
      auto notification = Notification(message.message, true, type);
      e->notification_system.add(std::move(notification));
    },
    &self,
    loguru::Verbosity_INFO
  );

  self.thumbnail_manager.init();

  return {};
}

auto Editor::deinit(this Editor& self) -> std::expected<void, std::string> {
  ZoneScoped;

  auto& job_man = App::get_job_manager();
  job_man.get_tracker().stop_tracking();

  auto& event_system = App::get_event_system();
  std::ignore = event_system.unsubscribe<ScenePlayEvent>(self.scene_play_handler);
  std::ignore = event_system.unsubscribe<SceneStopEvent>(self.scene_stop_handler);

  self.thumbnail_manager.deinit();

  self.main_viewport_panel.deinit();
  self.main_viewport_panel.reset();

  self.editor_panel_registry.get<SceneHierarchyPanel>().set_scene(nullptr);
  self.editor_context.reset();
  self.scene_manager.reset();

  Log::remove_callback("editor_notifications");

  ImPlot::DestroyContext();

  return {};
}

auto Editor::update(this Editor& self, const Timestep& timestep) -> void {
  ZoneScoped;

  self.thumbnail_manager.update();

  self.editor_panel_registry.update_all();

  auto& shp = self.editor_panel_registry.get<SceneHierarchyPanel>();
  self.main_viewport_panel.update(timestep, &shp);

  auto& render_context = App::get_rendercontext();
  auto& imgui_renderer = App::mod<ImGuiRenderer>();
  auto& window = App::get_window();

  auto swapchain_attachment = render_context.new_frame();
  swapchain_attachment = vuk::clear_image(std::move(swapchain_attachment), vuk::Black<f32>);

  imgui_renderer.keyboard_input_enabled = !self.main_viewport_panel.is_any_scene_playing();

  imgui_renderer.begin_frame(timestep.get_seconds(), window.get_logical_size(), window.get_real_size());
  ImGuizmo::SetImGuiContext(ImGui::GetCurrentContext());
  ImGuizmo::BeginFrame();

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

  swapchain_attachment = imgui_renderer.end_frame(render_context, std::move(swapchain_attachment));

  render_context.end_frame(swapchain_attachment);
}

auto Editor::render(this Editor& self, const vuk::ImageAttachment& swapchain_attachment) -> void {
  ZoneScoped;

  auto& job_man = App::get_job_manager();

  auto status = job_man.get_tracker().get_status();
  for (const auto& [name, is_working] : status) {
    if (name == "Completion callback")
      continue;

    Notification notification(name, !is_working, Notification::Type::Loading);
    self.notification_system.add(std::move(notification));
  }

  job_man.get_tracker().cleanup_old();
  self.notification_system.draw();

  if (self.editor_cvar.cvar_show_style_editor.get())
    ImGui::ShowStyleEditor();
  if (self.editor_cvar.cvar_show_imgui_demo.get())
    ImGui::ShowDemoWindow();

  constexpr ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_PassthruCentralNode;

  constexpr ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking |
                                            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNavFocus |
                                            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                                            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings;

  constexpr float bottom_bar_height = 30.0f;

  ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->WorkPos);
  ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, viewport->WorkSize.y - bottom_bar_height));
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

    if (!self.main_viewport_panel.is_fullscreen()) {
      self.editor_panel_registry.render_all(swapchain_attachment);
    }

    self.draw_menubar();

    static bool dock_layout_initalized = false;
    if (!dock_layout_initalized) {
      self.set_docking_layout(self.current_layout);
      dock_layout_initalized = true;
    }
  }

  ImGui::End();

  self.draw_bottom_toolbar(bottom_bar_height);
}

void Editor::reset(this Editor& self) {
  ZoneScoped;

  self.main_viewport_panel.reset();

  auto& sh = self.editor_panel_registry.get<SceneHierarchyPanel>();
  sh.set_scene(nullptr);

  self.scene_manager.reset();
}

void Editor::new_scene(this Editor& self) {
  ZoneScoped;

  App::get_rendercontext().wait();

  auto new_scene_id = self.scene_manager.new_scene();
  self.scene_manager.load_default_scene(new_scene_id);
  auto scene = self.scene_manager.get_scene(new_scene_id);

  self.main_viewport_panel.add_new_scene(scene);
}

bool Editor::open_scene(const std::filesystem::path& path) {
  ZoneScoped;

  auto loaded_scene = scene_manager.load_scene(path);

  if (loaded_scene.has_value()) {
    auto scene = scene_manager.get_scene(loaded_scene.value());
    main_viewport_panel.add_new_scene(scene);
  }

  return loaded_scene.has_value();
}

void Editor::open_scene_file_dialog() {
  ZoneScoped;

  const auto& window = App::get_window();
  FileDialogFilter dialog_filters[] = {{.name = "Oxylus scene file(.oxscene)", .pattern = "oxscene"}};
  window.show_dialog({
    .kind = DialogKind::OpenFile,
    .user_data = this,
    .callback =
      [](void* user_data, const c8* const* files, i32) {
        auto* e = static_cast<Editor*>(user_data);
        if (!e || !files || !*files) {
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
  ZoneScoped;

  auto* focused_viewport = main_viewport_panel.get_focused_viewport();
  if (!focused_viewport)
    return;

  auto* scene = focused_viewport->get_scene();

  if (!scene || scene->is_playing()) {
    return;
  }

  if (!scene->get_path().empty()) {
    submit_scene_save(scene, scene->get_path());
  } else {
    save_scene_as();
  }
}

auto Editor::sync_terrain_edits_asset(Scene& scene, const std::filesystem::path& scene_path) -> std::filesystem::path {
  ZoneScoped;

  if (scene.terrain == nullptr || !scene.terrain_entity || !scene.terrain_entity.has<TerrainComponent>()) {
    return {};
  }

  auto& c = scene.terrain_entity.get_mut<TerrainComponent>();
  auto& asset_man = App::mod<AssetManager>();

  if (!c.terrain_edits) {
    if (!scene.terrain->edits_dirty) {
      return {};
    }

    auto edits_path = scene_path;
    edits_path.replace_extension(".oxterrain");

    c.terrain_edits = asset_man.create_asset(AssetType::Terrain, edits_path);
    // Takes the scene's one ref and loads the (still empty) payload the readback fills in.
    scene.set_terrain_edits_ref(c.terrain_edits);
  }

  scene.sync_terrain_edits();

  auto asset = asset_man.get_asset(c.terrain_edits);

  return asset ? asset->path : std::filesystem::path{};
}

auto Editor::submit_scene_save(EditorScene* scene, std::filesystem::path path) -> void {
  App::defer_to_next_frame([scene, scene_path = std::move(path)] {
    // The readback records GPU work, so it has to finish here on the main thread; the job below
    // only writes files.
    auto edits_path = sync_terrain_edits_asset(*scene->get_scene(), scene_path);
    auto edits_uuid = UUID{};
    if (!edits_path.empty()) {
      edits_uuid = scene->get_scene()->terrain_edits_ref;
    }

    auto& job_man = App::get_job_manager();
    job_man.push_job_name("Saving scene");
    job_man.submit(Job::create([scene, scene_path, edits_path, edits_uuid] {
      scene->get_scene()->save_to_file(scene_path);
      if (edits_uuid) {
        App::mod<AssetManager>().export_asset(edits_uuid, edits_path);
      }
      scene->set_path(scene_path);
    }));
    job_man.pop_job_name();
  });
}

void Editor::save_scene_as() {
  ZoneScoped;

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
        if (!files || !*files) {
          if (udata) {
            delete udata;
          }
          return;
        }

        const auto first_path_cstr = *files;
        const auto first_path_len = std::strlen(first_path_cstr);
        const auto path = std::string(first_path_cstr, first_path_len);

        if (!path.empty()) {
          submit_scene_save(udata->scene, path);
        }

        delete udata;
      },
    .title = "New Scene...",
    .default_path = "NewScene.oxscene",
    .filters = dialog_filters,
    .multi_select = false,
  });
}

void Editor::set_docking_layout(this Editor& self, EditorLayout layout) {
  ZoneScoped;

  self.current_layout = layout;
  ImGui::DockBuilderRemoveNode(self.dockspace_id);
  ImGui::DockBuilderAddNode(self.dockspace_id, ImGuiDockNodeFlags_PassthruCentralNode);

  const ImVec2 size = ImGui::GetMainViewport()->WorkSize;
  ImGui::DockBuilderSetNodeSize(self.dockspace_id, size);

  if (layout == EditorLayout::BigViewport) {
    const ImGuiID
      right_dock = ImGui::DockBuilderSplitNode(self.dockspace_id, ImGuiDir_Right, 0.8f, nullptr, &self.dockspace_id);
    ImGuiID
      left_dock = ImGui::DockBuilderSplitNode(self.dockspace_id, ImGuiDir_Left, 0.2f, nullptr, &self.dockspace_id);
    const ImGuiID left_split_dock = ImGui::DockBuilderSplitNode(left_dock, ImGuiDir_Down, 0.4f, nullptr, &left_dock);

    ImGui::DockBuilderDockWindow(self.main_viewport_panel.get_id(), right_dock);
    ImGui::DockBuilderDockWindow(self.editor_panel_registry.get<SceneHierarchyPanel>().get_id(), left_dock);
    ImGui::DockBuilderDockWindow(self.editor_panel_registry.get<ContentPanel>().get_id(), left_split_dock);
    ImGui::DockBuilderDockWindow(self.editor_panel_registry.get<InspectorPanel>().get_id(), left_dock);
  } else if (layout == EditorLayout::Classic) {
    const ImGuiID
      right_dock = ImGui::DockBuilderSplitNode(self.dockspace_id, ImGuiDir_Right, 0.2f, nullptr, &self.dockspace_id);
    ImGuiID
      left_dock = ImGui::DockBuilderSplitNode(self.dockspace_id, ImGuiDir_Left, 0.85f, nullptr, &self.dockspace_id);
    const ImGuiID left_bottom_dock = ImGui::DockBuilderSplitNode(left_dock, ImGuiDir_Down, 0.3f, nullptr, &left_dock);
    const ImGuiID
      left_vertical_split_dock = ImGui::DockBuilderSplitNode(left_dock, ImGuiDir_Left, 0.2f, nullptr, &left_dock);

    ImGui::DockBuilderDockWindow(self.editor_panel_registry.get<InspectorPanel>().get_id(), right_dock);
    ImGui::DockBuilderDockWindow(self.main_viewport_panel.get_id(), left_dock);
    ImGui::DockBuilderDockWindow(self.editor_panel_registry.get<ContentPanel>().get_id(), left_bottom_dock);
    ImGui::DockBuilderDockWindow(
      self.editor_panel_registry.get<SceneHierarchyPanel>().get_id(),
      left_vertical_split_dock
    );
  }

  ImGui::DockBuilderFinish(self.dockspace_id);
}

void Editor::reset_current_docking_layout() {
  ZoneScoped;

  set_docking_layout(current_layout);

  main_viewport_panel.update_dockspace();
}

void Editor::draw_menubar(this Editor& self) {
  ZoneScoped;

  if (ImGui::BeginMenuBar()) {
    if (ImGui::BeginMenu("File")) {
      ImGui::Separator();
      if (ImGui::MenuItem("Launcher...")) {
        self.editor_panel_registry.get<ProjectPanel>().visible = true;
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Exit")) {
        App::get()->should_stop();
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
      ImGui::BeginDisabled(self.undo_redo_system->get_undo_count() < 1);
      if (ImGui::MenuItem("Undo", "Ctrl + Z")) {
        self.undo();
      }
      ImGui::EndDisabled();
      ImGui::BeginDisabled(self.undo_redo_system->get_redo_count() < 1);
      if (ImGui::MenuItem("Redo", "Ctrl + Y")) {
        self.redo();
      }
      ImGui::EndDisabled();
      if (ImGui::MenuItem("Settings")) {
        self.editor_panel_registry.get<EditorSettingsPanel>().visible = true;
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Window")) {
      ImGui::MenuItem("Inspector", nullptr, &self.editor_panel_registry.get<InspectorPanel>().visible);
      ImGui::MenuItem("Scene hierarchy", nullptr, &self.editor_panel_registry.get<SceneHierarchyPanel>().visible);
      ImGui::MenuItem("Text Editor", nullptr, &self.editor_panel_registry.get<TextEditorPanel>().visible);
      ImGui::MenuItem("Particle Editor", nullptr, &self.editor_panel_registry.get<ParticleEditorPanel>().visible);
      if (ImGui::BeginMenu("Layout")) {
        if (ImGui::MenuItem("Classic")) {
          self.set_docking_layout(EditorLayout::Classic);
        }
        if (ImGui::MenuItem("Big Viewport")) {
          self.set_docking_layout(EditorLayout::BigViewport);
        }
        ImGui::EndMenu();
      }
      ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Assets")) {
      if (ImGui::MenuItem("Asset Manager")) {
        self.editor_panel_registry.get<AssetManagerPanel>().visible = true;
      }
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
      const std::string& project_name = self.active_project->get_config().name;
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

void Editor::draw_bottom_toolbar(this Editor& self, float height) {
  ZoneScoped;

  ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - height));
  ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, height));
  ImGui::SetNextWindowViewport(viewport->ID);

  constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 4.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.11f, 0.11f, 0.11f, 1.0f));

  if (ImGui::Begin("##BottomToolbar", nullptr, flags)) {
    auto& content_panel = self.editor_panel_registry.get<ContentPanel>();
    auto content_panel_text = fmt::format("{} {}", content_panel.get_icon(), "Content Panel");
    if (content_panel.visible)
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (
      UI::toggle_button(
        content_panel_text.c_str(),
        content_panel.visible,
        {},
        1.f,
        1.f,
        ImGuiButtonFlags_None,
        ImGuiCol_Header
      )
    ) {
      content_panel.visible = !content_panel.visible;
    }
    if (content_panel.visible)
      ImGui::PopStyleColor();

    ImGui::SameLine();
    auto activity_log_text = fmt::format("{} {}", ICON_MDI_FORUM, "Activity Log");
    auto& activity_log_panel_state = self.editor_panel_registry.get<ActivityLogPanel>().visible;
    if (activity_log_panel_state)
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (
      UI::toggle_button(
        activity_log_text.c_str(),
        activity_log_panel_state,
        {},
        1.f,
        1.f,
        ImGuiButtonFlags_None,
        ImGuiCol_Header
      )
    ) {
      activity_log_panel_state = !activity_log_panel_state;
    }
    if (activity_log_panel_state)
      ImGui::PopStyleColor();

    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();

    ImGui::TextDisabled("Cmd: `~` to open console");

    const auto& io = ImGui::GetIO();
    auto last_notification = self.notification_system.get_last_notification();
    if (last_notification.has_value()) {
      auto& notif = *last_notification;
      std::string icon_text = {};
      switch (notif.type) {
        case Notification::Info   : icon_text = ICON_MDI_INFORMATION; break;
        case Notification::Warn   : icon_text = ICON_MDI_ALERT; break;
        case Notification::Error  : icon_text = ICON_MDI_EXCLAMATION; break;
        case Notification::Loading: icon_text = ICON_MDI_CHECK_BOLD; break;
      }

      auto notif_text = fmt::format("{} {}", icon_text, notif.title);
      const float text_width = ImGui::CalcTextSize(notif_text.c_str()).x;
      ImGui::SameLine(ImGui::GetWindowWidth() - text_width - 16.0f);
      ImGui::TextUnformatted(notif_text.c_str());
      if (ImGui::IsItemClicked()) {
        activity_log_panel_state = !activity_log_panel_state;
      }
    }
  }

  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(3);
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
