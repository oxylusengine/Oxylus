#include "ProjectPanel.hpp"

#include <icons/IconsMaterialDesignIcons.h>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include "Core/App.hpp"
#include "Core/Project.hpp"
#include "Core/VFS.hpp"
#include "Editor.hpp"
#include "Panels/ContentPanel.hpp"
#include "UI/UI.hpp"
#include "Utils/EditorConfig.hpp"
#include "Utils/EmbeddedBanner.hpp"

namespace ox {
ProjectPanel::ProjectPanel() : EditorPanel("Projects", ICON_MDI_ACCOUNT_BADGE, true) {
  engine_banner = std::make_shared<Texture>();
  engine_banner->create(
    {},
    {.preset = Preset::eRTT2DUnmipped,
     .format = vuk::Format::eR8G8B8A8Srgb,
     .mime = {},
     .loaded_data = editor_banner,
     .extent = vuk::Extent3D{.width = editor_bannerWidth, .height = editor_bannerHeight, .depth = 1u}}
  );
}

void ProjectPanel::on_update() {}

void ProjectPanel::load_project_for_editor(const std::filesystem::path& filepath) {
  auto& editor = App::mod<Editor>();
  const auto& active_project = editor.active_project;

  if (!std::filesystem::exists(filepath)) {
    OX_LOG_WARN("Couldn't find project. Removing from recent projects: {}", filepath);
    App::mod<EditorConfig>().remove_recent_project(filepath);
    return;
  }

  if (active_project->load(filepath)) {
    auto& vfs = App::get_vfs();
    const auto start_scene = vfs.resolve_physical_dir(VFS::PROJECT_DIR, active_project->get_config().start_scene);
    if (!editor.open_scene(start_scene)) {
      editor.new_scene();
    }
    App::mod<EditorConfig>().add_recent_project(active_project.get());
    editor.get_panel<ContentPanel>()->init();
    visible = false;
  }
}

void ProjectPanel::new_project(
  const std::filesystem::path& project_dir,
  const std::string& project_name,
  const std::filesystem::path& project_asset_dir
) {
  const auto& active_project = App::mod<Editor>().active_project;
  if (active_project->new_project(project_dir, project_name, project_asset_dir))
    App::mod<EditorConfig>().add_recent_project(active_project.get());
}

void ProjectPanel::on_render(vuk::ImageAttachment swapchain_attachment) {
  if (visible && !ImGui::IsPopupOpen("ProjectSelector"))
    ImGui::OpenPopup("ProjectSelector");

  constexpr auto flags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                         ImGuiWindowFlags_NoBackground;
  static bool draw_new_project_panel = false;

  const auto banner_size = engine_banner->get_extent();

  UI::center_next_window();
  ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.0, 0.0, 0.0, 0.7));
  ImGui::SetNextWindowSize(ImVec2(banner_size.width, 400.f));
  if (ImGui::BeginPopupModal("ProjectSelector", nullptr, flags)) {
    const float x = static_cast<float>(banner_size.width);
    const float y = static_cast<float>(ImGui::GetFrameHeight()) * 1.3f;

    const auto& window = App::get_window();

    UI::image(*engine_banner, {x, static_cast<float>(banner_size.height)});
    UI::spacing(2);
    ImGui::SeparatorText("Recent Projects");
    UI::spacing(2);

    if (ImGui::BeginChild("##Contents", {}, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding)) {
      UI::push_frame_style();
      if (draw_new_project_panel) {
        UI::begin_properties();

        UI::input_text("Name", &new_project_name);

        UI::begin_property_grid("Directory", nullptr, false);

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.8f);
        auto new_project_dir_str = std::string();
        ImGui::InputText("##Directory", &new_project_dir_str, flags);
        new_project_dir = std::filesystem::path(new_project_dir_str).make_preferred();
        ImGui::SameLine();
        if (ImGui::Button(ICON_MDI_FOLDER, {ImGui::GetContentRegionAvail().x, 0})) {
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
                panel->new_project_dir = panel->new_project_dir / panel->new_project_name;
              },
            .title = "Project dir...",
            .default_path = std::filesystem::current_path(),
            .filters = dialog_filters,
            .multi_select = false,
          });
        }

        UI::end_property_grid();

        auto new_project_asset_dir_str = std::string();
        UI::input_text("Asset Directory", &new_project_asset_dir_str);
        new_project_asset_dir = std::filesystem::path(new_project_asset_dir_str).make_preferred();
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
        const auto projects = App::mod<EditorConfig>().get_recent_projects();
        for (auto& project : projects) {
          auto project_name = project.stem().string();
          auto cursor_pos_y = ImGui::GetCursorPosY();
          if (ImGui::Button(project_name.c_str(), {-1.f, y})) {
            load_project_for_editor(project);
          }
          UI::tooltip_hover(project.string().c_str());

          ImGui::SameLine();

          // Arrow icons for buttons
          ImGui::SetCursorPosX(x - 30.f);
          const auto font_size = y * 0.6f;
          ImGui::SetCursorPosY(cursor_pos_y + 4.f); // 4 is just a random number i picked that looked centered enough...
          ImGui::PushFont(nullptr, font_size);
          ImGui::TextUnformatted(ICON_MDI_PLAY_OUTLINE);
          ImGui::PopFont();
        }

        UI::spacing(2);
        ImGui::Separator();
        UI::spacing(2);

        if (ImGui::Button(ICON_MDI_PLUS " New Project", {-1.f, y})) {
          draw_new_project_panel = true;
        }
        if (ImGui::Button(ICON_MDI_FOLDER " Load Project", {-1.f, y})) {
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
            .default_path = std::filesystem::current_path(),
            .filters = dialog_filters,
            .multi_select = false,
          });
        }

        UI::spacing(8);

        constexpr f32 cnt_button_size = 200.f;
        ImGui::SetCursorPosX((x / 2.f) - (cnt_button_size / 2.f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        if (ImGui::Button("Continue without project", ImVec2(cnt_button_size, 0))) {
          visible = false;
          ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();
      }

      UI::pop_frame_style();
      ImGui::EndChild();
    }

    ImGui::EndPopup();
  }

  ImGui::PopStyleColor(); // ImGuiCol_ModalWindowDimBg
}
} // namespace ox
