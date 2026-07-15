#pragma once

#include <filesystem>

#include "Utils/CVars.hpp"

namespace ox {
class Project;
class EditorCVar : public CVarInterface {
public:
  static constexpr const char* EDITOR_CONFIG_FILE_NAME = "editor_config.toml";

  EditorCVar();
  ~EditorCVar();

  auto init(this EditorCVar& self) -> void;

  auto save(this EditorCVar& self) -> void;
  auto load(this EditorCVar& self) -> void;

  auto add_recent_project(this EditorCVar& self, const Project* project) -> void;
  auto remove_recent_project(this EditorCVar& self, const std::filesystem::path& path) -> void;
  auto get_recent_projects(this EditorCVar& self) -> const std::vector<std::filesystem::path>& {
    return self.recent_projects;
  }

  AutoCVar_Int cvar_draw_grid;
  AutoCVar_Float cvar_draw_grid_distance;

  AutoCVar_Float cvar_camera_speed;
  AutoCVar_Float cvar_camera_sens;
  AutoCVar_Int cvar_camera_smooth;
  AutoCVar_Int cvar_camera_zoom;

  AutoCVar_Int cvar_file_thumbnails;
  AutoCVar_Float cvar_file_thumbnail_size;
  AutoCVar_Int cvar_show_meta_files;
  AutoCVar_Int cvar_show_style_editor;
  AutoCVar_Int cvar_show_imgui_demo;

private:
  std::vector<std::filesystem::path> recent_projects{};
};
} // namespace ox
