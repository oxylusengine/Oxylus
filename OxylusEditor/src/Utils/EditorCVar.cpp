#include "EditorCVar.hpp"

#include <Tracy.hpp>
#include <toml++/toml.hpp>

#include "Core/Project.hpp"
#include "OS/File.hpp"

namespace ox {
EditorCVar::EditorCVar() {
  ZoneScoped;

  init();
  load();
}

EditorCVar::~EditorCVar() {
  ZoneScoped;

  save();
}

auto EditorCVar::init(this EditorCVar& self) -> void {
  ZoneScoped;

  self.cvar_draw_grid.init(self.system, "rr.draw_grid", "draw editor scene grid", 1);
  self.cvar_draw_grid_distance.init(self.system, "rr.grid_distance", "max grid distance", 1000.f);

  self.cvar_camera_speed.init(self.system, "editor.camera_speed", "editor camera speed", 5.0f);
  self.cvar_camera_sens.init(self.system, "editor.camera_sens", "editor camera sens", 0.1f);
  self.cvar_camera_smooth.init(self.system, "editor.camera_smooth", "editor camera smoothing", 1);
  self.cvar_camera_zoom.init(self.system, "editor.camera_zoom", "editor camera zoom for ortho projection", 1);

  self.cvar_scale_viewport_size_with_content_scale
    .init(self.system, "editor.scale_viewport_size_with_content_scale", "scale viewport size with content scale", 1);
  self.cvar_viewport_scale_amount
    .init(self.system, "editor.viewport_scale_amount", "scale viewport size with content scale", 1);

  self.cvar_file_thumbnails.init(self.system, "editor.file_thumbnails", "show file thumbnails in content panel", 0);
  self.cvar_file_thumbnail_size
    .init(self.system, "editor.file_thumbnail_size", "file thumbnail size in content panel", 120.0f);
  self.cvar_show_meta_files.init(self.system, "editor.show_meta_files", "show oxasset files in conten panel", 0);
  self.cvar_show_style_editor.init(self.system, "ui.imgui_style_editor", "show imgui style editor", 0);
  self.cvar_show_imgui_demo.init(self.system, "ui.imgui_demo", "show imgui demo window", 0);
}

auto EditorCVar::load(this EditorCVar& self) -> void {
  ZoneScoped;

  const auto& content = File::to_string(EDITOR_CONFIG_FILE_NAME);
  if (content.empty()) {
    return;
  }

  toml::table toml = toml::parse(content);
  const auto config = toml["editor_config"];
  for (auto& project : *config["recent_projects"].as_array()) {
    self.recent_projects.emplace_back(std::filesystem::path(**project.as_string()));
  }

  if (auto v = config["grid"].as_boolean())
    self.cvar_draw_grid.set(v->get());
  if (auto v = config["grid_distance"].as_floating_point())
    self.cvar_draw_grid_distance.set((float)v->get());
  if (auto v = config["camera_speed"].as_floating_point())
    self.cvar_camera_speed.set((float)v->get());
  if (auto v = config["camera_sens"].as_floating_point())
    self.cvar_camera_sens.set((float)v->get());
  if (auto v = config["camera_smooth"].as_boolean())
    self.cvar_camera_smooth.set(v->get());
  if (auto v = config["scale_viewport_size_with_content_scale"].as_boolean())
    self.cvar_scale_viewport_size_with_content_scale.set(v->get());
  if (auto v = config["viewport_scale_amount"].as_integer())
    self.cvar_viewport_scale_amount.set(v->get());
  if (auto v = config["file_thumbnails"].as_boolean())
    self.cvar_file_thumbnails.set(v->get());
  if (auto v = config["file_thumbnail_size"].as_floating_point())
    self.cvar_file_thumbnail_size.set(static_cast<f32>(v->get()));
  if (auto v = config["show_meta_files"].as_boolean())
    self.cvar_show_meta_files.set(v->get());

  return;
}

auto EditorCVar::save(this EditorCVar& self) -> void {
  ZoneScoped;

  toml::array recent_projects_array;

  for (auto& project : self.recent_projects)
    recent_projects_array.emplace_back(project.string());

  const auto root = toml::table{
    {"editor_config",
     toml::table{
       {"recent_projects", recent_projects_array},
       {"grid", self.cvar_draw_grid.as_bool()},
       {"grid_distance", self.cvar_draw_grid_distance.get()},
       {"camera_speed", self.cvar_camera_speed.get()},
       {"camera_sens", self.cvar_camera_sens.get()},
       {"camera_smooth", self.cvar_camera_smooth.as_bool()},
       {"scale_viewport_size_with_content_scale", self.cvar_scale_viewport_size_with_content_scale.as_bool()},
       {"viewport_scale_amount", self.cvar_viewport_scale_amount.get()},
       {"file_thumbnails", self.cvar_file_thumbnails.as_bool()},
       {"file_thumbnail_size", self.cvar_file_thumbnail_size.get()},
       {"show_meta_files", self.cvar_show_meta_files.as_bool()},
     }}
  };

  std::stringstream ss;
  ss << "# Oxylus Editor config file \n";
  ss << root;
  auto file = File(EDITOR_CONFIG_FILE_NAME, FileAccess::Write);
  file.write(ss.str());
  file.close();
}

void EditorCVar::add_recent_project(this EditorCVar& self, const Project* project) {
  for (auto& recent_project_path : self.recent_projects) {
    if (recent_project_path.filename() == project->get_project_file_path().filename()) {
      return;
    }
  }

  self.recent_projects.emplace_back(project->get_project_file_path());
}

auto EditorCVar::remove_recent_project(this EditorCVar& self, const std::filesystem::path& path) -> void {
  std::erase_if(self.recent_projects, [path](const std::filesystem::path& e) { return e == path; });
}

} // namespace ox
