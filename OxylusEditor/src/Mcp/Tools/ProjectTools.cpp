#include <fmt/format.h>
#include <memory>
#include <string>
#include <tracy/Tracy.hpp>

#include "Actions/ProjectActions.hpp"
#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Core/VFS.hpp"
#include "Editor.hpp"
#include "Mcp/McpArgs.hpp"
#include "Mcp/McpRefs.hpp"
#include "Mcp/McpSchema.hpp"
#include "Mcp/McpServer.hpp"
#include "Mcp/McpTools.hpp"
#include "Panels/LoadingPanel.hpp"

namespace ox {
constexpr static usize OPEN_REPORTED_ERRORS = 5;

static auto last_log_id(Editor& editor) -> u64 {
  editor.notification_system.drain_pending();
  const auto& history = editor.notification_system.notification_history;
  return history.empty() ? 0 : history.back().id;
}

static auto project_get(Editor& editor, const McpArgs&) -> McpResult {
  ZoneScoped;

  auto& project = *editor.active_project;
  if (project.get_project_file_path().empty()) {
    return McpResult::ok("no project is open");
  }

  const auto& config = project.get_config();
  const auto& project_dir = project.get_project_directory();
  std::string out;
  out += fmt::format("name: {}\n", config.name);
  out += fmt::format("directory: {}\n", project_dir.string());
  out += fmt::format("file: {}\n", project.get_project_file_path().string());
  out += fmt::format("asset directory: {}\n", config.asset_directory.generic_string());
  out += fmt::format("start scene: {}\n", config.start_scene.empty() ? "none" : config.start_scene);
  out += fmt::format(
    "scripts project: {}\n",
    config.scripts_project.empty() ? "none" : config.scripts_project.generic_string()
  );
#ifdef OX_DOTNET
  out += "csharp: enabled\n";
#else
  out += "csharp: disabled in this build\n";
#endif
  out += fmt::format("asset scan: {}", asset_scan_active() ? "active" : "idle");
  return McpResult::ok(std::move(out));
}

static auto project_open(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto path_arg = args.str("path");
  if (!path_arg) {
    return McpResult::error(path_arg.error());
  }

  const auto opened = open_project(editor, std::filesystem::path(*path_arg));
  if (!opened) {
    return McpResult::error(opened.error());
  }

  const auto first_log_id = last_log_id(editor);
  return McpResult::pending([&editor, first_log_id]() -> option<McpResult> {
    auto& loading = editor.editor_panel_registry.get<LoadingPanel>();
    if (loading.is_failed()) {
      auto message = fmt::format("could not open the project: {}", loading.failure_text());
      loading.dismiss_failure();
      return McpResult::error(std::move(message));
    }

    const auto is_opened = loading.is_past_scan() && !editor.pending_start_scene.has_value() && !asset_scan_active();
    if (!is_opened) {
      return nullopt;
    }

    editor.notification_system.drain_pending();
    usize error_count = 0;
    std::string errors;
    for (const auto& entry : editor.notification_system.notification_history) {
      if (entry.id <= first_log_id || entry.type != Notification::Error) {
        continue;
      }

      error_count += 1;
      if (error_count <= OPEN_REPORTED_ERRORS) {
        errors += fmt::format("\n  {} {}", entry.id, entry.title.substr(0, entry.title.find('\n')));
      }
    }

    const auto asset_count = App::mod<AssetManager>().get_registry_snapshot().size();
    return McpResult::ok(
      fmt::format(
        "opened '{}', {} assets registered, {} errors in the log{}",
        editor.active_project->get_config().name,
        asset_count,
        error_count,
        errors
      )
    );
  });
}

static auto project_new(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto directory = args.str("directory");
  if (!directory) {
    return McpResult::error(directory.error());
  }

  const auto name = args.str("name");
  if (!name) {
    return McpResult::error(name.error());
  }

  const auto asset_directory = args.opt_str("asset_directory");
  if (!asset_directory) {
    return McpResult::error(asset_directory.error());
  }

  const auto asset_path = std::filesystem::path(asset_directory->value_or("Assets")).lexically_normal();
  const auto created = new_project(editor, std::filesystem::path(*directory), *name, asset_path);
  if (!created) {
    return McpResult::error(created.error());
  }

  return McpResult::ok(
    fmt::format(
      "created project '{}' in {}, the asset scan runs in the background, editor_wait assets waits for it",
      *name,
      editor.active_project->get_project_directory().string()
    )
  );
}

static auto project_set(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto name = args.opt_str("name");
  if (!name) {
    return McpResult::error(name.error());
  }

  const auto start_scene = args.opt_str("start_scene");
  if (!start_scene) {
    return McpResult::error(start_scene.error());
  }

  if (!name->has_value() && !start_scene->has_value()) {
    return McpResult::error("give a name or a start_scene");
  }

  option<std::string> start_scene_text = nullopt;
  if (start_scene->has_value()) {
    const auto resolved = resolve_project_path(editor, start_scene->value(), true);
    if (!resolved) {
      return McpResult::error(resolved.error());
    }

    if (resolved->extension() != ".oxscene") {
      return McpResult::error("start_scene must be an .oxscene file");
    }

    std::error_code error;
    const auto asset_root = std::filesystem::weakly_canonical(
      App::get_vfs().resolve_physical_dir(VFS::ASSETS_DIR, ""),
      error
    );
    const auto relative = resolved->lexically_relative(asset_root);
    if (relative.empty() || *relative.begin() == "..") {
      return McpResult::error("start_scene must be inside the project's asset directory");
    }

    start_scene_text = relative.generic_string();
  }

  const auto name_text = name->has_value() ? option<std::string_view>(name->value()) : nullopt;
  const auto start_text = start_scene_text.has_value() ? option<std::string_view>(std::string_view(*start_scene_text))
                                                       : nullopt;
  const auto saved = set_project(editor, name_text, start_text);
  if (!saved) {
    return McpResult::error(saved.error());
  }

  const auto& config = editor.active_project->get_config();
  return McpResult::ok(
    fmt::format(
      "project saved: name '{}', start scene {}",
      config.name,
      config.start_scene.empty() ? "none" : config.start_scene
    )
  );
}

static auto project_cook(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto folder_arg = args.str("folder");
  if (!folder_arg) {
    return McpResult::error(folder_arg.error());
  }

  auto folder = std::filesystem::path(*folder_arg);
  if (!folder.is_absolute()) {
    const auto resolved = resolve_project_path(editor, *folder_arg, false);
    if (!resolved) {
      return McpResult::error(resolved.error());
    }

    folder = resolved.value();
  }

  const auto status = cook_project(editor, folder);
  if (!status) {
    return McpResult::error(status.error());
  }

  return McpResult::pending([cook_status = status.value()]() -> option<McpResult> {
    if (!cook_status->is_done.load(std::memory_order_acquire)) {
      return nullopt;
    }

    if (!cook_status->is_ok.load(std::memory_order_relaxed)) {
      return McpResult::error(cook_status->error);
    }

    return McpResult::ok(fmt::format("cooked the project assets into {}", cook_status->output.string()));
  });
}

auto register_project_tools(McpServer& server) -> void {
  ZoneScoped;

  server.add_tool(
    McpTool{
      .name = "project_get",
      .title = "Project info",
      .description = "Name, directory, project file, asset directory, start scene, C# scripts project and whether C# "
                     "is enabled for the open project.",
      .input_schema = SchemaBuilder{}.build(),
      .is_read_only = true,
      .is_idempotent = true,
      .handler = project_get,
    }
  );

  server.add_tool(
    McpTool{
      .name = "project_open",
      .title = "Open project",
      .description = "Opens an .oxproj project, replacing the current one and closing its scenes. Waits for the "
                     "asset scan and the start scene, a cold project can take minutes. Returns the registered asset "
                     "count and the errors logged during the open. Unsaved scenes are lost.",
      .input_schema = SchemaBuilder{}.string("path", "absolute path of the .oxproj file", true).build(),
      .is_destructive = true,
      .timeout_ms = 600000,
      .handler = project_open,
    }
  );

  server.add_tool(
    McpTool{
      .name = "project_new",
      .title = "New project",
      .description = "Creates a project in <directory>/<name> with its asset folder and an empty scene, and opens "
                     "it. The directory must exist and the project folder must not. Absolute paths are fine here. "
                     "Replaces the current project, unsaved scenes are lost.",
      .input_schema = SchemaBuilder{}
                        .string("directory", "existing parent folder, absolute", true)
                        .string("name", "project name, also the folder and file name", true)
                        .string("asset_directory", "asset folder inside the project, default Assets", false)
                        .build(),
      .is_destructive = true,
      .handler = project_new,
    }
  );

  server.add_tool(
    McpTool{
      .name = "project_set",
      .title = "Set project settings",
      .description = "Changes the project name and start scene and saves the project file.",
      .input_schema = SchemaBuilder{}
                        .string("name", "new project name", false)
                        .string("start_scene", "project-relative .oxscene path inside the asset directory", false)
                        .build(),
      .is_idempotent = true,
      .handler = project_set,
    }
  );

  server.add_tool(
    McpTool{
      .name = "project_cook",
      .title = "Cook project",
      .description = "Cooks the project's assets the way a game build does, into <folder>/.cooked. Waits until the "
                     "cook is done.",
      .input_schema = SchemaBuilder{}.string("folder", "output folder, absolute or project-relative", true).build(),
      .timeout_ms = 600000,
      .handler = project_cook,
    }
  );
}
} // namespace ox
