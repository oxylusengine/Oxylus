#include "ProjectPanel.hpp"

#include <icons/IconsMaterialDesignIcons.h>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include "Core/FileSystem.hpp"
#include "Core/Project.hpp"
#include "Core/VFS.hpp"
#include "EditorLayer.hpp"
#include "EditorUI.hpp"
#include "Utils/EditorConfig.hpp"
#include "Utils/StringUtils.hpp"

namespace ox {
ProjectPanel::ProjectPanel() : EditorPanel("Projects", ICON_MDI_ACCOUNT_BADGE, true) {}

void ProjectPanel::on_update() {}

void ProjectPanel::load_project_for_editor(const std::string& filepath) {
  const auto& active_project = EditorLayer::get()->active_project;
  if (active_project->load(filepath)) {
    auto* vfs = App::get_system<VFS>(EngineSystems::VFS);
    const auto start_scene = vfs->resolve_physical_dir(VFS::PROJECT_DIR, active_project->get_config().start_scene);
    EditorLayer::get()->open_scene(start_scene);
    EditorConfig::get()->add_recent_project(active_project.get());
    EditorLayer::get()->get_panel<ContentPanel>()->invalidate();
    visible = false;
  }
}

void ProjectPanel::new_project(const std::string& project_dir,
                               const std::string& project_name,
                               const std::string& project_asset_dir) {
  const auto& active_project = EditorLayer::get()->active_project;
  if (active_project->new_project(project_dir, project_name, project_asset_dir))
    EditorConfig::get()->add_recent_project(active_project.get());
}

void ProjectPanel::on_render(vuk::Extent3D extent, vuk::Format format) {
  if (visible && !ImGui::IsPopupOpen("ProjectSelector"))
    ImGui::OpenPopup("ProjectSelector");
  constexpr auto flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking;
  static bool draw_new_project_panel = false;

  UI::center_next_window();
  if (ImGui::BeginPopupModal("ProjectSelector", nullptr, flags)) {
    const auto banner_size = EditorLayer::get()->engine_banner->get_extent();
    const float x = static_cast<float>(banner_size.width);
    const float y = static_cast<float>(ImGui::GetFrameHeight());

    const auto& window = App::get()->get_window();

    UI::image(*EditorLayer::get()->engine_banner, {x, static_cast<float>(banner_size.height)});
    UI::spacing(2);
    ImGui::SeparatorText("Projects");
    UI::spacing(2);

    if (ImGui::BeginChild("##Contents", {}, ImGuiChildFlags_AutoResizeY)) {
      UI::push_frame_style();
      if (draw_new_project_panel) {
        UI::begin_properties();

        UI::input_text("Name", &new_project_name);

        UI::begin_property_grid("Directory", nullptr, false);

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.8f);
        ImGui::InputText("##Directory", &new_project_dir, flags);
        ImGui::SameLine();
        if (ImGui::Button(StringUtils::from_char8_t(ICON_MDI_FOLDER), {ImGui::GetContentRegionAvail().x, 0})) {
          FileDialogFilter dialog_filters[] = {{.name = "Project dir", .pattern = "oxproj"}};
          window.show_dialog({
              .kind = DialogKind::OpenFolder,
              .user_data = this,
              .callback =
                  [](void* user_data, const c8* const* files, i32) {
                    auto* panel = static_cast<ProjectPanel*>(user_data);
                    if (!files || !*files) {
                      return;
                    }

                    const auto first_path_cstr = *files;
                    const auto first_path_len = std::strlen(first_path_cstr);
                    panel->new_project_dir = std::string(first_path_cstr, first_path_len);
                    panel->new_project_dir = fs::append_paths(panel->new_project_dir, panel->new_project_name);
                  },
              .title = "Project dir...",
              .default_path = fs::current_path(),
              .filters = dialog_filters,
              .multi_select = false,
          });
        }

        UI::end_property_grid();

        UI::input_text("Asset Directory", &new_project_asset_dir);
        UI::end_properties();

        ImGui::Separator();

        ImGui::SetNextItemWidth(-1);
        if (ImGui::Button("Create", ImVec2(120, 0))) {
          new_project(new_project_dir, new_project_name, new_project_asset_dir);
          visible = false;
          ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
          draw_new_project_panel = false;
        }
      } else {
        const auto projects = EditorConfig::get()->get_recent_projects();
        for (auto& project : projects) {
          auto project_name = fs::get_file_name(project);
          if (ImGui::Button(project_name.c_str(), {x, y})) {
            load_project_for_editor(project);
          }
        }

        ImGui::Separator();
        if (ImGui::Button(StringUtils::from_char8_t(ICON_MDI_FILE_DOCUMENT " New Project"), {x, y})) {
          draw_new_project_panel = true;
        }
        ImGui::SetNextItemWidth(x);
        if (ImGui::Button(StringUtils::from_char8_t(ICON_MDI_UPLOAD " Load Project"), {x, y})) {
          FileDialogFilter dialog_filters[] = {{.name = "Oxylus Project", .pattern = "oxproj"}};
          window.show_dialog({
              .kind = DialogKind::OpenFile,
              .user_data = this,
              .callback =
                  [](void* user_data, const c8* const* files, i32) {
                    auto* usr_data = static_cast<ProjectPanel*>(user_data);
                    if (!files || !*files) {
                      return;
                    }

                    const auto first_path_cstr = *files;
                    const auto first_path_len = std::strlen(first_path_cstr);
                    const auto path = std::string(first_path_cstr, first_path_len);
                    if (!path.empty()) {
                      usr_data->load_project_for_editor(path);
                    }
                  },
              .title = "Open project...",
              .default_path = fs::current_path(),
              .filters = dialog_filters,
              .multi_select = false,
          });
        }
        UI::align_right(ImVec2(120, 0).x);
        if (ImGui::Button("Skip", ImVec2(120, 0))) {
          visible = false;
          ImGui::CloseCurrentPopup();
        }
      }

      UI::pop_frame_style();
      ImGui::EndChild();
    }

    ImGui::EndPopup();
  }
}
} // namespace ox
