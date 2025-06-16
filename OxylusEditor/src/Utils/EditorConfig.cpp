#include "EditorConfig.hpp"

#include <Render/RendererConfig.hpp>
#include <fstream>

#include "Core/FileSystem.hpp"
#include "Core/Project.hpp"
#include "EditorLayer.hpp"
#include "Utils/Log.hpp"
#include "Utils/Toml.hpp"

namespace ox {
EditorConfig* EditorConfig::instance = nullptr;

constexpr const char* EDITOR_CONFIG_FILE_NAME = "editor_config.toml";

EditorConfig::EditorConfig() {
  if (!instance)
    instance = this;
}

void EditorConfig::load_config() {
  ZoneScoped;
  const auto& content = fs::read_file(EDITOR_CONFIG_FILE_NAME);
  if (content.empty())
    return;

  toml::table toml = toml::parse(content);
  const auto config = toml["editor_config"];
  for (auto& project : *config["recent_projects"].as_array()) {
    recent_projects.emplace_back(*project.as_string());
  }
  RendererCVar::cvar_draw_grid.set(config["grid"].as_boolean()->get());
  RendererCVar::cvar_draw_grid_distance.set((float)config["grid_distance"].as_floating_point()->get());
  EditorCVar::cvar_camera_speed.set((float)config["camera_speed"].as_floating_point()->get());
  EditorCVar::cvar_camera_sens.set((float)config["camera_sens"].as_floating_point()->get());
  EditorCVar::cvar_camera_smooth.set(config["camera_smooth"].as_boolean()->get());
  EditorCVar::cvar_file_thumbnails.set(config["file_thumbnails"].as_boolean()->get());
  EditorCVar::cvar_file_thumbnail_size.set((float)config["file_thumbnail_size"].as_floating_point()->get());
}

void EditorConfig::save_config() const {
  toml::array recent_projects_array;

  for (auto& project : recent_projects)
    recent_projects_array.emplace_back(project);

  const auto root = toml::table{{"editor_config",
                                 toml::table{
                                     {"recent_projects", recent_projects_array},
                                     {"grid", (bool)RendererCVar::cvar_draw_grid.get()},
                                     {"grid_distance", RendererCVar::cvar_draw_grid_distance.get()},
                                     {"camera_speed", EditorCVar::cvar_camera_speed.get()},
                                     {"camera_sens", EditorCVar::cvar_camera_sens.get()},
                                     {"camera_smooth", (bool)EditorCVar::cvar_camera_smooth.get()},
                                     {"file_thumbnails", (bool)EditorCVar::cvar_file_thumbnails.get()},
                                     {"file_thumbnail_size", EditorCVar::cvar_file_thumbnail_size.get()},
                                 }}};

  std::stringstream ss;
  ss << "# Oxylus Editor config file \n";
  ss << root;
  std::ofstream filestream(EDITOR_CONFIG_FILE_NAME);
  filestream << ss.str();
}

void EditorConfig::add_recent_project(const Project* path) {
  for (auto& project : recent_projects) {
    if (fs::get_file_name(project) == fs::get_file_name(path->get_project_file_path())) {
      return;
    }
  }
  recent_projects.emplace_back(path->get_project_file_path());
}
} // namespace ox
