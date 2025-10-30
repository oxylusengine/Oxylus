#pragma once

#include <expected>
#include <string>
#include <filesystem>
#include <vector>

#include "Utils/CVars.hpp"

namespace ox {
class Project;

namespace EditorCVar {
inline AutoCVar_Int cvar_draw_grid("rr.draw_grid", "draw editor scene grid", 1);
inline AutoCVar_Float cvar_draw_grid_distance("rr.grid_distance", "max grid distance", 1000.f);

inline AutoCVar_Float cvar_camera_speed("editor.camera_speed", "editor camera speed", 5.0f);
inline AutoCVar_Float cvar_camera_sens("editor.camera_sens", "editor camera sens", 0.1f);
inline AutoCVar_Int cvar_camera_smooth("editor.camera_smooth", "editor camera smoothing", 1);
inline AutoCVar_Int cvar_camera_zoom("editor.camera_zoom", "editor camera zoom for ortho projection", 1);

inline AutoCVar_Int cvar_file_thumbnails("editor.file_thumbnails", "show file thumbnails in content panel", 0);
inline AutoCVar_Float cvar_file_thumbnail_size(
  "editor.file_thumbnail_size", "file thumbnail size in content panel", 120.0f
);
inline AutoCVar_Int cvar_show_meta_files("editor.show_meta_files", "show oxasset files in conten panel", 1);

inline AutoCVar_Int cvar_show_style_editor("ui.imgui_style_editor", "show imgui style editor", 0);

inline AutoCVar_Int cvar_show_imgui_demo("ui.imgui_demo", "show imgui demo window", 0);
} // namespace EditorCVar

class EditorConfig {
public:
  constexpr static auto MODULE_NAME = "EditorConfig";

  auto init() -> std::expected<void, std::string>;
  auto deinit() -> std::expected<void, std::string>;

  auto add_recent_project(const Project* path) -> void;
  auto remove_recent_project(const std::filesystem::path& path) -> void;
  auto get_recent_projects() const -> const std::vector<std::filesystem::path>& { return recent_projects; }

private:
  std::vector<std::filesystem::path> recent_projects{};
  static EditorConfig* instance;
};
} // namespace ox
