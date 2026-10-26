#include "ProjectPanel.hpp"

#include <algorithm>
#include <icons/IconsMaterialDesignIcons.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>
#include <system_error>
#include <tracy/Tracy.hpp>
#include <utility>

#include "Core/App.hpp"
#include "Core/Base.hpp"
#include "Core/Version.hpp"
#include "Editor.hpp"
#include "Panels/ContentPanel.hpp"
#include "Panels/LoadingPanel.hpp"
#include "Project/Project.hpp"
#include "Render/Window.hpp"
#include "UI/UI.hpp"
#include "Utils/Log.hpp"

namespace ox {
static auto validate_new_project_form(
  const std::string_view location, const std::string_view name, const std::string_view asset_directory
) -> std::string_view {
  ZoneScoped;

  if (name.empty() || name.find_first_not_of(" \t\r\n") == std::string_view::npos) {
    return "Enter a project name";
  }

  if (
    name == "." || name == ".." || name.find_first_of("<>:\"/\\|?*") != std::string_view::npos || name.ends_with('.') ||
    name.ends_with(' ')
  ) {
    return "The project name contains characters that cannot be used in a folder name";
  }

  if (location.empty()) {
    return "Choose where the project should be created";
  }

  const auto location_path = std::filesystem::path(location);
  std::error_code error;
  if (!std::filesystem::exists(location_path, error) || !std::filesystem::is_directory(location_path, error)) {
    return "The selected location is not an existing folder";
  }
  if (error) {
    return "The selected location could not be accessed";
  }

  if (asset_directory.empty()) {
    return "Enter an asset folder";
  }

  const auto asset_path = std::filesystem::path(asset_directory);
  if (
    asset_path == "." || asset_path.is_absolute() || asset_path.has_root_path() ||
    std::ranges::any_of(asset_path, [](const auto& component) -> bool { return component == ".."; })
  ) {
    return "The asset folder must stay inside the project";
  }

  const auto project_path = location_path / name;
  if (std::filesystem::exists(project_path, error)) {
    return "A folder with this project name already exists at the selected location";
  }
  if (error) {
    return "The project location could not be checked";
  }

  return {};
}

static auto draw_project_message(const std::string_view message) -> void {
  if (message.empty()) {
    return;
  }

  ImGui::PushStyleColor(ImGuiCol_Text, Gruvbox::bright_red.Value);
  ImGui::TextWrapped(ICON_MDI_ALERT_CIRCLE "  %.*s", static_cast<i32>(message.size()), message.data());
  ImGui::PopStyleColor();
}

static auto draw_project_heading(const std::string_view title, const std::string_view description) -> void {
  ZoneScoped;

  const auto& theme = App::mod<Editor>().editor_theme;
  ImGui::PushFont(theme.bold_font, 24.0f);
  ImGui::TextUnformatted(title.data(), title.data() + title.size());
  ImGui::PopFont();
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::PushTextWrapPos();
  ImGui::TextUnformatted(description.data(), description.data() + description.size());
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
  ImGui::Spacing();
}

static auto draw_project_row_text(
  ImDrawList* draw_list, const ImVec2 position, const f32 max_x, const std::string_view text
) -> void {
  ImGui::RenderTextEllipsis(
    draw_list,
    position,
    {max_x, position.y + ImGui::GetTextLineHeight()},
    max_x,
    text.data(),
    text.data() + text.size(),
    nullptr
  );
}

ProjectPanel::ProjectPanel() : EditorPanelState("Projects", ICON_MDI_FOLDER_OPEN, true) {
  window_default_size = {1080.0f, 680.0f};
  window_center_at_appear = true;
  window_center_cond = ImGuiCond_Appearing;

  std::error_code error;
  new_project_location = std::filesystem::current_path(error).string();
}

auto ProjectPanel::load_project_for_editor(this ProjectPanel& self, const std::filesystem::path& filepath) -> void {
  ZoneScoped;

  auto& editor = App::mod<Editor>();

  std::error_code error;
  if (!std::filesystem::is_regular_file(filepath, error)) {
    OX_LOG_WARN("Couldn't find project. Removing from recent projects: {}", filepath);
    editor.editor_cvar.remove_recent_project(filepath);
    self.panel_error = "That project file no longer exists and was removed from the recent list";
    return;
  }

  // let the loading panel paint before the directory walk starts
  editor.editor_panel_registry.get<LoadingPanel>().begin(filepath);
  self.panel_error.clear();
  self.creating_project = false;
  self.close_requested = true;
}

auto ProjectPanel::show_error(this ProjectPanel& self, std::string message) -> void {
  self.panel_error = std::move(message);
  self.creating_project = false;
  self.select_projects_tab = true;
  self.close_requested = false;
  self.visible = true;
}

auto ProjectPanel::new_project(
  this ProjectPanel& self,
  const std::filesystem::path& project_dir,
  const std::string_view project_name,
  const std::filesystem::path& project_asset_dir
) -> bool {
  ZoneScoped;

  auto& editor = App::mod<Editor>();
  const auto& active_project = editor.active_project;

  try {
    if (!active_project->new_project(project_dir, project_name, project_asset_dir)) {
      return false;
    }
  } catch (const std::filesystem::filesystem_error& exception) {
    OX_LOG_ERROR("Couldn't create project at {}: {}", project_dir, exception.what());
    return false;
  }

  editor.reset();
  editor.new_scene();
  editor.reset_current_docking_layout();
  editor.editor_cvar.add_recent_project(active_project.get());
  editor.editor_panel_registry.get<ContentPanel>().init();

  self.panel_error.clear();
  self.creating_project = false;
  self.close_requested = true;
  return true;
}

auto ProjectPanel::browse_project(this ProjectPanel& self) -> void {
  ZoneScoped;

  FileDialogFilter dialog_filters[] = {{.name = "Oxylus Project", .pattern = "oxproj"}};
  std::error_code error;
  auto default_path = std::filesystem::path(self.new_project_location);
  if (!std::filesystem::is_directory(default_path, error)) {
    default_path = std::filesystem::current_path(error);
  }

  App::get_window().show_dialog({
    .kind = DialogKind::OpenFile,
    .user_data = &self,
    .callback = [](void* user_data, const c8* const* files, i32) -> void {
      auto* panel = static_cast<ProjectPanel*>(user_data);
      if (!panel || !files || !*files) {
        return;
      }

      auto path = std::filesystem::path(std::string(*files));
      App::defer_to_next_frame([panel, p = std::move(path)]() -> void { panel->load_project_for_editor(p); });
    },
    .title = "Open an Oxylus project",
    .default_path = default_path,
    .filters = dialog_filters,
    .multi_select = false,
  });
}

auto ProjectPanel::continue_to_editor(this ProjectPanel& self) -> void {
  ZoneScoped;

  auto& editor = App::mod<Editor>();
  if (editor.main_viewport_panel.get_visible_viwports().empty()) {
    editor.new_scene();
  }
  self.creating_project = false;
  self.close_requested = true;
}

auto ProjectPanel::draw_sidebar(this ProjectPanel& self) -> void {
  ZoneScoped;

  const auto& editor = App::mod<Editor>();
  const auto& theme = editor.editor_theme;
  theme.draw_banner(ImGui::GetContentRegionAvail().x * 0.8f);
  UI::spacing(2);
  const auto& build_info = engine_build_info();
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextUnformatted("Version");
  ImGui::PopStyleColor();
  ImGui::TextUnformatted(build_info.full_version.data());
  UI::spacing(3);
  ImGui::Separator();
  UI::spacing(2);

  ImGui::PushStyleColor(ImGuiCol_Button, theme.header_selected_color);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
  if (ImGui::Button(ICON_MDI_PLUS "  New Project", {-1.0f, UI::scale(38.0f)})) {
    self.creating_project = true;
    self.select_projects_tab = true;
    self.panel_error.clear();
  }
  ImGui::PopStyleColor(2);
  if (ImGui::Button(ICON_MDI_FOLDER_OPEN "  Open Project...", {-1.0f, UI::scale(38.0f)})) {
    self.browse_project();
  }

  UI::spacing(3);
  ImGui::TextDisabled("GETTING STARTED");
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("Start with a blank project, or open an existing .oxproj file from your computer.");
  ImGui::PopStyleColor();

  if (!editor.active_project->get_project_file_path().empty()) {
    UI::spacing(3);
    ImGui::TextDisabled("CURRENT PROJECT");
    ImGui::TextWrapped("%s", editor.active_project->get_config().name.c_str());
  }

  const auto footer_height = UI::scale(38.0f);
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.0f, ImGui::GetContentRegionAvail().y - footer_height));
  if (ImGui::Button(ICON_MDI_ARROW_RIGHT "  Continue to editor", {-1.0f, footer_height})) {
    self.continue_to_editor();
  }
}

auto ProjectPanel::draw_recent_projects(this ProjectPanel& self) -> void {
  ZoneScoped;

  auto& editor = App::mod<Editor>();
  const auto& projects = editor.editor_cvar.get_recent_projects();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Recent projects");
  ImGui::SameLine();
  ImGui::TextDisabled("(%zu)", projects.size());
  ImGui::SetNextItemWidth(-1.0f);
  if (
    ImGui::InputTextWithHint(
      "##ProjectSearch",
      ICON_MDI_MAGNIFY "  Search by name or path...",
      self.project_filter.InputBuf,
      IM_ARRAYSIZE(self.project_filter.InputBuf)
    )
  ) {
    self.project_filter.Build();
  }

  std::filesystem::path project_to_open;
  std::filesystem::path project_to_remove;
  const auto list_visible = ImGui::BeginChild(
    "##RecentProjects",
    {},
    ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding
  );
  if (list_visible) {
    bool found_project = false;
    for (const auto& project : projects) {
      const auto project_file = project.string();
      if (!self.project_filter.PassFilter(project_file.c_str())) {
        continue;
      }
      found_project = true;
      ImGui::PushID(project_file.c_str());

      std::error_code error;
      const auto project_exists = std::filesystem::is_regular_file(project, error);
      const auto row_height = std::max(UI::scale(64.0f), ImGui::GetTextLineHeight() * 2.0f + UI::scale(24.0f));
      const auto menu_button_width = UI::scale(32.0f);
      const auto project_button_width = std::max(
        1.0f,
        ImGui::GetContentRegionAvail().x - menu_button_width - ImGui::GetStyle().ItemSpacing.x
      );

      ImGui::BeginDisabled(!project_exists);
      if (ImGui::Button("##OpenProject", {project_button_width, row_height})) {
        project_to_open = project;
      }
      ImGui::EndDisabled();
      const auto row_min = ImGui::GetItemRectMin();
      const auto row_max = ImGui::GetItemRectMax();
      UI::tooltip_hover(project_file.c_str());
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        ImGui::OpenPopup("##RecentProjectActions");
      }

      auto* draw_list = ImGui::GetWindowDrawList();
      const auto icon_color = ImGui::GetColorU32(project_exists ? ImGuiCol_CheckMark : ImGuiCol_TextDisabled);
      const auto icon_x = row_min.x + UI::scale(14.0f);
      const auto icon_width = ImGui::CalcTextSize(ICON_MDI_FOLDER_OUTLINE).x;
      draw_list->AddText(
        {icon_x, row_min.y + (row_height - ImGui::GetTextLineHeight()) * 0.5f},
        icon_color,
        ICON_MDI_FOLDER_OUTLINE
      );

      const auto text_x = icon_x + icon_width + UI::scale(12.0f);
      const auto text_max_x = row_max.x - UI::scale(12.0f);
      const auto name_y = row_min.y + UI::scale(10.0f);
      const auto project_name = project.stem().string();
      ImGui::PushFont(editor.editor_theme.bold_font, editor.editor_theme.regular_font_size);
      ImGui::PushStyleColor(
        ImGuiCol_Text,
        ImGui::GetStyleColorVec4(project_exists ? ImGuiCol_Text : ImGuiCol_TextDisabled)
      );
      draw_project_row_text(draw_list, {text_x, name_y}, text_max_x, project_name);
      ImGui::PopStyleColor();
      ImGui::PopFont();

      const auto project_directory = project.parent_path().string();
      const std::string_view displayed_path = project_exists ? std::string_view(project_directory)
                                                             : "Project file is missing";
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
      draw_project_row_text(
        draw_list,
        {text_x, row_max.y - ImGui::GetTextLineHeight() - UI::scale(10.0f)},
        text_max_x,
        displayed_path
      );
      ImGui::PopStyleColor();

      ImGui::SameLine();
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
      if (ImGui::Button(ICON_MDI_DOTS_VERTICAL, {menu_button_width, row_height})) {
        ImGui::OpenPopup("##RecentProjectActions");
      }
      ImGui::PopStyleColor(2);
      UI::tooltip_hover("Project actions");

      if (ImGui::BeginPopup("##RecentProjectActions")) {
        if (ImGui::MenuItem(ICON_MDI_FOLDER_OPEN "  Open project", nullptr, false, project_exists)) {
          project_to_open = project;
        }
        if (ImGui::MenuItem(ICON_MDI_CONTENT_COPY "  Copy project path")) {
          ImGui::SetClipboardText(project_file.c_str());
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_MDI_CLOSE_CIRCLE_OUTLINE "  Remove from recent projects")) {
          project_to_remove = project;
        }
        ImGui::EndPopup();
      }
      ImGui::PopID();
    }

    if (!found_project) {
      UI::spacing(4);
      draw_project_heading(
        projects.empty() ? "Your next project starts here" : "No matching projects",
        projects.empty()
          ? "Create a new project, or open an existing .oxproj file. Your recent projects will appear here."
          : "Try a different name or folder path."
      );
      if (self.project_filter.IsActive() && ImGui::Button("Clear search")) {
        self.project_filter.Clear();
      }
    }
  }
  ImGui::EndChild();

  // mutate the recent list after its rows and popups have finished using it
  if (!project_to_remove.empty()) {
    editor.editor_cvar.remove_recent_project(project_to_remove);
    self.panel_error.clear();
  }
  if (!project_to_open.empty()) {
    self.load_project_for_editor(project_to_open);
  }
}

auto ProjectPanel::draw_new_project(this ProjectPanel& self) -> void {
  ZoneScoped;

  const auto contents_visible = ImGui::BeginChild("##CreateProject", {}, ImGuiChildFlags_AlwaysUseWindowPadding);
  OX_DEFER() { ImGui::EndChild(); };
  if (!contents_visible) {
    return;
  }

  if (ImGui::Button(ICON_MDI_ARROW_LEFT "  Back")) {
    self.creating_project = false;
    self.panel_error.clear();
    return;
  }
  ImGui::SameLine();
  ImGui::SeparatorText("Create a project");
  ImGui::Spacing();

  bool form_changed = false;
  ImGui::TextUnformatted("Project name");
  ImGui::SetNextItemWidth(-1.0f);
  if (ImGui::IsWindowAppearing()) {
    ImGui::SetKeyboardFocusHere();
  }
  form_changed |= ImGui::InputTextWithHint("##ProjectName", "MyGame", &self.new_project_name);

  ImGui::TextUnformatted("Location");
  const auto browse_button_width = ImGui::CalcTextSize(ICON_MDI_FOLDER_OPEN "  Browse...").x +
                                   ImGui::GetStyle().FramePadding.x * 2.0f;
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - browse_button_width - ImGui::GetStyle().ItemSpacing.x);
  form_changed |= ImGui::InputText("##ProjectLocation", &self.new_project_location);
  ImGui::SameLine();
  if (ImGui::Button(ICON_MDI_FOLDER_OPEN "  Browse...", {browse_button_width, 0.0f})) {
    auto default_path = std::filesystem::path(self.new_project_location);
    std::error_code error;
    if (!std::filesystem::is_directory(default_path, error)) {
      default_path = std::filesystem::current_path(error);
    }

    App::get_window().show_dialog({
      .kind = DialogKind::OpenFolder,
      .user_data = &self,
      .callback = [](void* user_data, const c8* const* files, i32) -> void {
        auto* panel = static_cast<ProjectPanel*>(user_data);
        if (!panel || !files || !*files) {
          return;
        }

        auto selected_path = std::filesystem::path(std::string(*files)).make_preferred().string();
        App::defer_to_next_frame([panel, p = std::move(selected_path)]() -> void {
          panel->new_project_location = p;
          panel->panel_error.clear();
        });
      },
      .title = "Choose a project location",
      .default_path = default_path,
      .multi_select = false,
    });
  }

  ImGui::TextUnformatted("Asset folder");
  ImGui::SetNextItemWidth(-1.0f);
  form_changed |= ImGui::InputTextWithHint("##AssetDirectory", "Assets", &self.new_project_asset_dir);
  UI::tooltip_hover("A path relative to the project folder");
  if (form_changed) {
    self.panel_error.clear();
  }

  const auto validation_error = validate_new_project_form(
    self.new_project_location,
    self.new_project_name,
    self.new_project_asset_dir
  );
  const auto project_path = std::filesystem::path(self.new_project_location) / self.new_project_name;
  ImGui::Spacing();
  const auto preview_visible = ImGui::BeginChild(
    "##ProjectDestination",
    {},
    ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders
  );
  if (preview_visible) {
    ImGui::TextDisabled("Project folder");
    ImGui::TextWrapped(ICON_MDI_FOLDER_OUTLINE "  %s", project_path.string().c_str());
    ImGui::TextDisabled("The project file and asset folder will be created here.");
  }
  ImGui::EndChild();

  draw_project_message(self.panel_error.empty() ? validation_error : std::string_view(self.panel_error));
  ImGui::Spacing();
  const auto create_button_width = UI::scale(180.0f);
  UI::align_right(create_button_width);
  ImGui::BeginDisabled(!validation_error.empty());
  if (ImGui::Button(ICON_MDI_PLUS "  Create Project", {create_button_width, UI::scale(38.0f)})) {
    const auto asset_path = std::filesystem::path(self.new_project_asset_dir).lexically_normal();
    if (!self.new_project(project_path, self.new_project_name, asset_path)) {
      self.panel_error = "The project could not be created. Check the log for details";
    }
  }
  ImGui::EndDisabled();
}

auto ProjectPanel::draw_samples(this ProjectPanel& self) -> void {
  ZoneScoped;

  const auto contents_visible = ImGui::BeginChild("##SampleProjects", {}, ImGuiChildFlags_AlwaysUseWindowPadding);
  if (contents_visible) {
    ImGui::SetCursorPosY(
      ImGui::GetCursorPosY() + std::max(0.0f, (ImGui::GetContentRegionAvail().y - UI::scale(240.0f)) * 0.35f)
    );
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
    ImGui::PushFont(nullptr, 40.0f);
    ImGui::TextUnformatted(ICON_MDI_DOWNLOAD);
    ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::TextDisabled("COMING SOON");
    draw_project_heading(
      "Learn by exploring",
      "Discover and download sample projects here in a future update. "
      "For now, start with a blank project or open a local .oxproj file."
    );
    if (ImGui::Button(ICON_MDI_PLUS "  Create a blank project", {0.0f, UI::scale(38.0f)})) {
      self.creating_project = true;
      self.select_projects_tab = true;
      self.panel_error.clear();
    }
  }
  ImGui::EndChild();
}

auto ProjectPanel::on_render(this ProjectPanel& self, const SwapchainInfo&) -> void {
  ZoneScoped;

  if (!ImGui::IsPopupOpen(self.id.c_str())) {
    ImGui::OpenPopup(self.id.c_str());
  }

  const auto* viewport = ImGui::GetMainViewport();
  const auto maximum_size = ImVec2(
    std::max(1.0f, viewport->WorkSize.x - UI::scale(32.0f)),
    std::max(1.0f, viewport->WorkSize.y - UI::scale(32.0f))
  );
  ImGui::SetNextWindowSizeConstraints(
    {std::min(UI::scale(760.0f), maximum_size.x), std::min(UI::scale(520.0f), maximum_size.y)},
    maximum_size
  );
  ImGui::SetNextWindowSize(UI::scale(self.window_default_size), self.window_sizing_cond);
  ImGui::SetNextWindowPos(viewport->GetCenter(), self.window_center_cond, ImVec2(0.5f, 0.5f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, UI::scale(ImVec2(16.0f, 16.0f)));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, UI::scale(ImVec2(12.0f, 8.0f)));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, UI::scale(ImVec2(10.0f, 7.0f)));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, UI::scale(2.0f));
  ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.0f, 0.0f, 0.0f, 0.7f));
  OX_DEFER() {
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(4);
  };

  constexpr auto flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                         ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse;
  if (!ImGui::BeginPopupModal(self.id.c_str(), nullptr, flags)) {
    return;
  }
  OX_DEFER(&self) {
    // close the modal before the registry stops rendering it
    if (self.close_requested) {
      ImGui::CloseCurrentPopup();
      self.close_requested = false;
      self.visible = false;
      self.creating_project = false;
    }
    ImGui::EndPopup();
  };
  if (self.close_requested) {
    return;
  }

  UI::push_frame_style();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, App::mod<Editor>().editor_theme.window_bg_alternative_color);
  const auto sidebar_visible = ImGui::BeginChild(
    "##ProjectSidebar",
    {UI::scale(232.0f), 0.0f},
    ImGuiChildFlags_AlwaysUseWindowPadding
  );
  ImGui::PopStyleColor();
  if (sidebar_visible) {
    self.draw_sidebar();
  }
  ImGui::EndChild();

  ImGui::SameLine();
  const auto workspace_visible = ImGui::BeginChild("##ProjectWorkspace", {}, ImGuiChildFlags_AlwaysUseWindowPadding);
  if (workspace_visible) {
    if (!self.creating_project) {
      draw_project_message(self.panel_error);
    }

    if (ImGui::BeginTabBar("##ProjectTabs")) {
      const auto projects_tab_flags = self.select_projects_tab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
      self.select_projects_tab = false;
      if (ImGui::BeginTabItem(ICON_MDI_FOLDER_OUTLINE "  Projects", nullptr, projects_tab_flags)) {
        if (self.creating_project) {
          self.draw_new_project();
        } else {
          self.draw_recent_projects();
        }
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem(ICON_MDI_DOWNLOAD "  Samples")) {
        self.draw_samples();
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
  }
  ImGui::EndChild();
  UI::pop_frame_style();
}
} // namespace ox
