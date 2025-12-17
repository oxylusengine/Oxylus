#include "EditorConfig.hpp"

#include <Render/RendererConfig.hpp>
#include <fmt/format.h>
#include <fstream>
#include <toml++/toml.hpp>
#include <tracy/Tracy.hpp>

#include "Core/Project.hpp"
#include "OS/File.hpp"

namespace ox {
constexpr const char* EDITOR_CONFIG_FILE_NAME = "editor_config.toml";

std::expected<void, std::string> EditorConfig::init(this EditorConfig& self) {
  ZoneScoped;
  const auto& content = File::to_string(EDITOR_CONFIG_FILE_NAME);
  if (content.empty()) {
    self.write_file();
    return {};
  }

  toml::table toml = toml::parse(content);
  const auto config = toml["editor_config"];
  for (auto& project : *config["recent_projects"].as_array()) {
    self.recent_projects.emplace_back(std::filesystem::path(**project.as_string()));
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
    EditorCVar::cvar_file_thumbnail_size.set(static_cast<f32>(v->get()));
  if (auto v = config["show_meta_files"].as_boolean())
    EditorCVar::cvar_show_meta_files.set(v->get());

  return {};
}

std::expected<void, std::string> EditorConfig::deinit(this const EditorConfig& self) {
  ZoneScoped;

  self.write_file();

  return {};
}

void EditorConfig::write_file(this const EditorConfig& self) {
  ZoneScoped;

  toml::array recent_projects_array;

  for (auto& project : self.recent_projects)
    recent_projects_array.emplace_back(project.string());

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
  auto file = File(EDITOR_CONFIG_FILE_NAME, FileAccess::Write);
  file.write(ss.str());
  file.close();
}

void EditorConfig::add_recent_project(this EditorConfig& self, const Project* project) {
  for (auto& recent_project_path : self.recent_projects) {
    if (recent_project_path.filename() == project->get_project_file_path().filename()) {
      return;
    }
  }

  self.recent_projects.emplace_back(project->get_project_file_path());
}

auto EditorConfig::remove_recent_project(this EditorConfig& self, const std::filesystem::path& path) -> void {
  std::erase_if(self.recent_projects, [path](const std::filesystem::path& e) { return e == path; });
}
} // namespace ox
