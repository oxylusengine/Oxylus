#include "EditorConfig.hpp"

#include <Render/RendererConfig.hpp>
#include <fmt/format.h>
#include <fstream>
#include <toml++/toml.hpp>
#include <tracy/Tracy.hpp>

#include "Core/FileSystem.hpp"
#include "Core/Project.hpp"

namespace ox {
EditorConfig* EditorConfig::instance = nullptr;

constexpr const char* EDITOR_CONFIG_FILE_NAME = "editor_config.toml";

std::expected<void, std::string> EditorConfig::init() {
  ZoneScoped;
  const auto& content = fs::read_file(EDITOR_CONFIG_FILE_NAME);
  if (content.empty())
    return std::unexpected(fmt::format("Couldn't read {}", EDITOR_CONFIG_FILE_NAME));

  toml::table toml = toml::parse(content);
  const auto config = toml["editor_config"];
  for (auto& project : *config["recent_projects"].as_array()) {
    recent_projects.emplace_back(*project.as_string());
  }

  if (auto v = config["grid"].as_boolean())
    EditorCVar::cvar_draw_grid.set(v->get());
  if (auto v = config["grid_distance"].as_floating_point())
    EditorCVar::cvar_draw_grid_distance.set((float)v->get());
  if (auto v = config["camera_speed"].as_floating_point())
    EditorCVar::cvar_camera_speed.set((float)v->get());
  if (auto v = config["camera_sens"].as_floating_point())
    EditorCVar::cvar_camera_sens.set((float)v->get());
  if (auto v = config["camera_smooth"].as_boolean())
    EditorCVar::cvar_camera_smooth.set(v->get());
  if (auto v = config["file_thumbnails"].as_boolean())
    EditorCVar::cvar_file_thumbnails.set(v->get());
  if (auto v = config["file_thumbnail_size"].as_floating_point())
    EditorCVar::cvar_file_thumbnail_size.set(v->get());
  if (auto v = config["show_meta_files"].as_boolean())
    EditorCVar::cvar_show_meta_files.set(v->get());

  return {};
}

std::expected<void, std::string> EditorConfig::deinit() {
  toml::array recent_projects_array;

  for (auto& project : recent_projects)
    recent_projects_array.emplace_back(project);

  const auto root = toml::table{
    {"editor_config",
     toml::table{
       {"recent_projects", recent_projects_array},
       {"grid", (bool)EditorCVar::cvar_draw_grid.get()},
       {"grid_distance", EditorCVar::cvar_draw_grid_distance.get()},
       {"camera_speed", EditorCVar::cvar_camera_speed.get()},
       {"camera_sens", EditorCVar::cvar_camera_sens.get()},
       {"camera_smooth", (bool)EditorCVar::cvar_camera_smooth.get()},
       {"file_thumbnails", (bool)EditorCVar::cvar_file_thumbnails.get()},
       {"file_thumbnail_size", EditorCVar::cvar_file_thumbnail_size.get()},
       {"show_meta_files", EditorCVar::cvar_show_meta_files.get()},
     }}
  };

  std::stringstream ss;
  ss << "# Oxylus Editor config file \n";
  ss << root;
  std::ofstream filestream(EDITOR_CONFIG_FILE_NAME);
  filestream << ss.str();

  return {};
}

void EditorConfig::add_recent_project(const Project* path) {
  for (auto& project : recent_projects) {
    if (fs::get_file_name(project) == fs::get_file_name(path->get_project_file_path())) {
      return;
    }
  }
  recent_projects.emplace_back(path->get_project_file_path());
}

auto EditorConfig::remove_recent_project(const std::string& path) -> void {
  std::erase_if(recent_projects, [path](const std::string& e) { return e == path; });
}
} // namespace ox
