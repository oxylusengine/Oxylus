#include "ProjectPanel.hpp"

#include <algorithm>
#include <icons/IconsMaterialDesignIcons.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>
#include <system_error>

#include "Core/App.hpp"
#include "Core/Project.hpp"
#include "Core/VFS.hpp"
#include "Editor.hpp"
#include "Panels/ContentPanel.hpp"
#include "UI/UI.hpp"
#include "Utils/EmbeddedBanner.hpp"

namespace ox {
auto validate_new_project_form(
  const std::string_view location, const std::string_view name, const std::string_view asset_directory
) -> std::string {
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
    std::ranges::any_of(asset_path, [](const auto& component) { return component == ".."; })
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

ProjectPanel::ProjectPanel() : EditorPanelState("Projects", ICON_MDI_ACCOUNT_BADGE, true) {
  engine_banner = Texture::create({
    .format = vuk::Format::eR8G8B8A8Srgb,
    .extent = vuk::Extent3D{.width = editor_bannerWidth, .height = editor_bannerHeight, .depth = 1u},
    .usage = vuk::ImageUsageFlagBits::eSampled,
  });
  engine_banner.upload(std::span(editor_banner), vuk::eFragmentSampled);

  std::error_code error;
  new_project_location = std::filesystem::current_path(error).string();
}

auto ProjectPanel::load_project_for_editor(this ProjectPanel& self, const std::filesystem::path& filepath) -> void {
  auto& editor = App::mod<Editor>();
  const auto& active_project = editor.active_project;

  std::error_code error;
  if (!std::filesystem::is_regular_file(filepath, error)) {
    OX_LOG_WARN("Couldn't find project. Removing from recent projects: {}", filepath);
    editor.editor_cvar.remove_recent_project(filepath);
    self.panel_error = "That project file no longer exists and was removed from the recent list";
    return;
  }

  if (active_project->load(filepath)) {
    auto& vfs = App::get_vfs();
    const auto start_scene = vfs.resolve_physical_dir(VFS::PROJECT_DIR, active_project->get_config().start_scene);
    editor.reset();
    if (!editor.open_scene(start_scene)) {
      editor.new_scene();
    }
    editor.reset_current_docking_layout();
    editor.editor_cvar.add_recent_project(active_project.get());
    editor.editor_panel_registry.get<ContentPanel>().init();
    self.panel_error.clear();
    self.close_requested = true;
    return;
  }

  self.panel_error = "The selected file is not a valid Oxylus project";
  OX_LOG_WARN("Couldn't load project: {}", filepath);
}

auto ProjectPanel::new_project(
  this ProjectPanel& self,
  const std::filesystem::path& project_dir,
  const std::string_view project_name,
  const std::filesystem::path& project_asset_dir
) -> bool {
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
  self.close_requested = true;
  return true;
}

auto ProjectPanel::on_render(this ProjectPanel& self, vuk::ImageAttachment) -> void {
  if (self.visible && !ImGui::IsPopupOpen("ProjectSelector"))
    ImGui::OpenPopup("ProjectSelector");

  constexpr auto flags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                         ImGuiWindowFlags_NoBackground;

  const auto banner_size = self.engine_banner.get_extent();

  UI::center_next_window();
  ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.0f, 0.0f, 0.0f, 0.7f));
  ImGui::SetNextWindowSize(UI::scale(ImVec2(static_cast<f32>(banner_size.width), 400.0f)));
  if (ImGui::BeginPopupModal("ProjectSelector", nullptr, flags)) {
    const auto& window = App::get_window();

    if (!self.creating_project) {
      constexpr f32 banner_left_padding = 50.0f;
      constexpr f32 banner_right_padding = 79.0f;
      const auto banner_width = ImGui::GetContentRegionAvail().x;
      const auto banner_height = banner_width * static_cast<f32>(banner_size.height) /
                                 static_cast<f32>(banner_size.width);
      const auto banner_center_offset = (banner_right_padding - banner_left_padding) * 0.5f * banner_width /
                                        static_cast<f32>(banner_size.width);
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + banner_center_offset);
      UI::image(self.engine_banner.view(), {banner_width, banner_height});
      UI::spacing(2);
    }

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
    const auto contents_visible = ImGui::BeginChild(
      "##Contents",
      {},
      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding
    );
    ImGui::PopStyleColor();
    if (contents_visible) {
      UI::push_frame_style();
      if (self.creating_project) {
        if (ImGui::Button(ICON_MDI_ARROW_LEFT "  Back")) {
          self.creating_project = false;
          self.panel_error.clear();
        }

        ImGui::SameLine();
        ImGui::SeparatorText("Create a project");
        UI::spacing(2);

        bool form_changed = false;

        ImGui::TextUnformatted("Project name");
        ImGui::SetNextItemWidth(-1.0f);
        form_changed |= ImGui::InputTextWithHint("##ProjectName", "MyGame", &self.new_project_name);

        UI::spacing(1);
        ImGui::TextUnformatted("Location");
        const auto browse_button_width = ImGui::CalcTextSize(ICON_MDI_FOLDER_OPEN "  Browse...").x +
                                         ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetNextItemWidth(
          ImGui::GetContentRegionAvail().x - browse_button_width - ImGui::GetStyle().ItemSpacing.x
        );
        form_changed |= ImGui::InputText("##ProjectLocation", &self.new_project_location);
        ImGui::SameLine();
        if (ImGui::Button(ICON_MDI_FOLDER_OPEN "  Browse...", {browse_button_width, 0.0f})) {
          auto default_path = std::filesystem::path(self.new_project_location);
          std::error_code error;
          if (!std::filesystem::is_directory(default_path, error)) {
            default_path = std::filesystem::current_path(error);
          }

          window.show_dialog({
            .kind = DialogKind::OpenFolder,
            .user_data = &self,
            .callback =
              [](void* user_data, const c8* const* files, i32) {
                auto* panel = static_cast<ProjectPanel*>(user_data);
                if (!panel || !files || !*files) {
                  return;
                }

                auto selected_path = std::filesystem::path(std::string(*files)).make_preferred().string();
                App::defer_to_next_frame([panel, selected_path = std::move(selected_path)]() -> void {
                  panel->new_project_location = selected_path;
                  panel->panel_error.clear();
                });
              },
            .title = "Choose a project location",
            .default_path = default_path,
            .multi_select = false,
          });
        }

        UI::spacing(1);
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

        UI::spacing(2);
        ImGui::Separator();
        UI::spacing(1);
        ImGui::TextDisabled("Project folder");
        ImGui::TextWrapped("%s", project_path.string().c_str());

        const auto& displayed_error = self.panel_error.empty() ? validation_error : self.panel_error;
        if (!displayed_error.empty()) {
          UI::spacing(1);
          ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%s  %s", ICON_MDI_ALERT_CIRCLE, displayed_error.c_str());
        }

        UI::spacing(2);
        ImGui::BeginDisabled(!validation_error.empty());
        if (ImGui::Button("Create Project", {-1.0f, UI::scale(34.0f)})) {
          const auto asset_path = std::filesystem::path(self.new_project_asset_dir).lexically_normal();
          if (!self.new_project(project_path, self.new_project_name, asset_path)) {
            self.panel_error = "The project could not be created. Check the log for details";
          }
        }
        ImGui::SetItemDefaultFocus();
        ImGui::EndDisabled();
      } else {
        ImGui::SeparatorText("Recent Projects");
        UI::spacing(1);

        const auto projects = App::mod<Editor>().editor_cvar.get_recent_projects();
        const auto recent_projects_visible = ImGui::BeginChild(
          "##RecentProjects",
          {0.0f, UI::scale(108.0f)},
          ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding
        );
        if (recent_projects_visible) {
          if (projects.empty()) {
            UI::spacing(3);
            ImGui::TextDisabled("No recent projects yet");
            ImGui::TextWrapped("Create a new project or open an existing .oxproj file to get started.");
          }

          for (const auto& project : projects) {
            ImGui::PushID(project.string().c_str());

            std::error_code error;
            const auto project_exists = std::filesystem::is_regular_file(project, error);
            const auto row_height = UI::scale(42.0f);
            const auto menu_button_width = UI::scale(28.0f);
            const auto project_button_width = ImGui::GetContentRegionAvail().x - menu_button_width -
                                              ImGui::GetStyle().ItemSpacing.x;

            ImGui::BeginDisabled(!project_exists);
            if (ImGui::Button("##OpenProject", {project_button_width, row_height})) {
              self.load_project_for_editor(project);
            }
            ImGui::EndDisabled();

            const auto row_min = ImGui::GetItemRectMin();
            const auto row_max = ImGui::GetItemRectMax();
            const auto text_vertical_padding = UI::scale(2.0f);
            const auto name_y = row_min.y + text_vertical_padding;
            const auto path_y = row_max.y - ImGui::GetTextLineHeight() - text_vertical_padding;
            const auto name_color = ImGui::GetColorU32(project_exists ? ImGuiCol_Text : ImGuiCol_TextDisabled);
            auto path_color = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
            path_color.w *= 0.75f;

            const auto project_name = project.stem().string();
            auto* draw_list = ImGui::GetWindowDrawList();
            const auto icon_width = ImGui::CalcTextSize(ICON_MDI_FOLDER).x;
            const auto name_width = ImGui::CalcTextSize(project_name.c_str()).x;
            const auto name_spacing = UI::scale(8.0f);
            const auto name_group_width = icon_width + name_spacing + name_width;
            const auto icon_x = row_min.x + ((row_max.x - row_min.x) - name_group_width) * 0.5f;
            draw_list->AddText({icon_x, name_y}, name_color, ICON_MDI_FOLDER);
            const auto name_x = icon_x + icon_width + name_spacing;
            draw_list->AddText({name_x, name_y}, name_color, project_name.c_str());

            const auto project_path = project_exists ? project.parent_path().string() : "Project file is missing";
            const auto path_margin = UI::scale(9.0f);
            const auto path_max_x = row_max.x - path_margin;
            const auto path_available_width = (row_max.x - row_min.x) - path_margin * 2.0f;
            const auto path_width = ImGui::CalcTextSize(project_path.c_str()).x;
            const auto path_x = path_width <= path_available_width
                                  ? row_min.x + ((row_max.x - row_min.x) - path_width) * 0.5f
                                  : row_min.x + path_margin;
            ImGui::PushStyleColor(ImGuiCol_Text, path_color);
            ImGui::RenderTextEllipsis(
              draw_list,
              {path_x, path_y},
              {path_max_x, path_y + ImGui::GetTextLineHeight()},
              path_max_x,
              project_path.data(),
              project_path.data() + project_path.size(),
              nullptr
            );
            ImGui::PopStyleColor();
            UI::tooltip_hover(project.string().c_str());

            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            if (ImGui::Button(ICON_MDI_DOTS_VERTICAL, {menu_button_width, row_height})) {
              ImGui::OpenPopup("##RecentProjectActions");
            }
            ImGui::PopStyleColor(2);
            UI::tooltip_hover("Project actions");

            if (ImGui::BeginPopup("##RecentProjectActions")) {
              if (ImGui::MenuItem(ICON_MDI_CLOSE_CIRCLE_OUTLINE "  Remove from recent projects")) {
                App::mod<Editor>().editor_cvar.remove_recent_project(project);
                self.panel_error.clear();
              }
              ImGui::EndPopup();
            }

            ImGui::PopID();
          }
        }
        ImGui::EndChild();

        UI::spacing(1);
        ImGui::Separator();
        UI::spacing(1);
        const auto action_width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (ImGui::Button(ICON_MDI_PLUS "  New Project", {action_width, UI::scale(34.0f)})) {
          self.creating_project = true;
          self.panel_error.clear();
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_MDI_FOLDER_OPEN "  Open Project", {action_width, UI::scale(34.0f)})) {
          FileDialogFilter dialog_filters[] = {{.name = "Oxylus Project", .pattern = "oxproj"}};
          window.show_dialog({
            .kind = DialogKind::OpenFile,
            .user_data = &self,
            .callback =
              [](void* user_data, const c8* const* files, i32) {
                auto* panel = static_cast<ProjectPanel*>(user_data);
                if (!panel || !files || !*files) {
                  return;
                }

                auto path = std::filesystem::path(std::string(*files));
                App::defer_to_next_frame([panel, path = std::move(path)]() -> void {
                  panel->load_project_for_editor(path);
                });
              },
            .title = "Open an Oxylus project",
            .default_path = std::filesystem::current_path(),
            .filters = dialog_filters,
            .multi_select = false,
          });
        }

        if (!self.panel_error.empty()) {
          UI::spacing(1);
          ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%s  %s", ICON_MDI_ALERT_CIRCLE, self.panel_error.c_str());
        }

        UI::spacing(3);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        if (ImGui::Button("Continue without a project", {-1.0f, 0.0f})) {
          auto& editor = App::mod<Editor>();
          editor.reset();
          editor.new_scene();
          self.close_requested = true;
        }
        ImGui::PopStyleColor();
      }

      UI::pop_frame_style();
    }
    ImGui::EndChild();

    if (self.close_requested) {
      self.close_requested = false;
      self.creating_project = false;
      self.visible = false;
      ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
  }

  ImGui::PopStyleColor();
}
} // namespace ox
