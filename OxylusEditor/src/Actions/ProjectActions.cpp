#include "Actions/ProjectActions.hpp"

#include <algorithm>
#include <fmt/format.h>
#include <system_error>
#include <tracy/Tracy.hpp>

#include "Actions/SceneActions.hpp"
#include "Core/App.hpp"
#include "Core/VFS.hpp"
#include "Editor.hpp"
#include "Panels/ContentPanel.hpp"
#include "Panels/LoadingPanel.hpp"
#include "Panels/ProjectPanel.hpp"
#include "Project/Project.hpp"

namespace ox {
auto finish_cook(CookStatus& status, bool is_ok, const std::filesystem::path& output) -> void {
  status.output = output;
  if (!is_ok) {
    status.error = fmt::format("cooking into '{}' failed, log_read has the errors", output.string());
  }

  status.is_ok.store(is_ok, std::memory_order_relaxed);
  status.is_done.store(true, std::memory_order_release);
}

auto validate_new_project_form(
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

auto open_project(Editor& editor, const std::filesystem::path& project_file) -> std::expected<void, std::string> {
  ZoneScoped;

  std::error_code error;
  if (!std::filesystem::is_regular_file(project_file, error) || project_file.extension() != ".oxproj") {
    return std::unexpected(fmt::format("'{}' is not an existing .oxproj file", project_file.string()));
  }

  if (editor.editor_panel_registry.get<LoadingPanel>().is_loading()) {
    return std::unexpected("a project is still opening, wait for it with editor_wait");
  }

  if (editor.main_viewport_panel.is_any_scene_playing()) {
    return std::unexpected("stop the playing scene before opening a project");
  }

  editor.editor_panel_registry.get<ProjectPanel>().load_project_for_editor(project_file);
  return {};
}

auto new_project(
  Editor& editor, const std::filesystem::path& location, std::string_view name, const std::filesystem::path& asset_dir
) -> std::expected<void, std::string> {
  ZoneScoped;

  const auto problem = validate_new_project_form(location.string(), name, asset_dir.string());
  if (!problem.empty()) {
    return std::unexpected(std::string(problem));
  }

  if (editor.main_viewport_panel.is_any_scene_playing()) {
    return std::unexpected("stop the playing scene before creating a project");
  }

  const auto& active_project = editor.active_project;
  const auto project_dir = location / name;
  try {
    if (!active_project->new_project(project_dir, name, asset_dir)) {
      return std::unexpected("the project could not be created, log_read has the reason");
    }
  } catch (const std::filesystem::filesystem_error& exception) {
    OX_LOG_ERROR("Couldn't create project at {}: {}", project_dir, exception.what());
    return std::unexpected(fmt::format("could not create the project: {}", exception.what()));
  }

  editor.reset();
  editor.new_scene();
  editor.reset_current_docking_layout();
  editor.editor_cvar.add_recent_project(active_project.get());
  editor.editor_panel_registry.get<ContentPanel>().init();
#ifdef OX_DOTNET
  if (auto created = CSharpScripts::create_project(*active_project); !created) {
    OX_LOG_ERROR("Couldn't create the project's C# scripts: {}", created.error());
  }

  editor.csharp_scripts.open(*active_project);
#endif

  return {};
}

auto set_project(Editor& editor, option<std::string_view> name, option<std::string_view> start_scene)
  -> std::expected<void, std::string> {
  ZoneScoped;

  auto& project = *editor.active_project;
  if (project.get_project_file_path().empty()) {
    return std::unexpected("no project is open");
  }

  if (name.has_value() && name->find_first_not_of(" \t\r\n") == std::string_view::npos) {
    return std::unexpected("the project name is empty");
  }

  auto& config = project.get_config();
  const auto previous = config;
  if (name.has_value()) {
    config.name = std::string(name.value());
  }

  if (start_scene.has_value()) {
    config.start_scene = std::string(start_scene.value());
  }

  if (!project.save(project.get_project_file_path())) {
    config = previous;
    return std::unexpected("the project file could not be written, log_read has the reason");
  }

  return {};
}

auto cook_project(Editor& editor, const std::filesystem::path& folder)
  -> std::expected<std::shared_ptr<CookStatus>, std::string> {
  ZoneScoped;

  auto status = std::make_shared<CookStatus>();
  status->output = folder;
  const auto started = editor.cook_assets_into(folder, status);
  if (!started) {
    return std::unexpected(started.error());
  }

  return status;
}
} // namespace ox
