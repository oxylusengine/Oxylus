#include <algorithm>
#include <chrono>
#include <fmt/format.h>
#include <memory>
#include <string>
#include <tracy/Tracy.hpp>
#include <vector>

#include "Actions/EntityActions.hpp"
#include "Core/App.hpp"
#include "Editor.hpp"
#include "Mcp/McpArgs.hpp"
#include "Mcp/McpRefs.hpp"
#include "Mcp/McpSchema.hpp"
#include "Mcp/McpServer.hpp"
#include "Mcp/McpTools.hpp"
#include "Utils/Command.hpp"

#ifdef OX_DOTNET
  #include "Actions/FileActions.hpp"
  #include "Scripting/DotNetScripting.hpp"
#endif

namespace ox {
constexpr static u64 BUILD_TIMEOUT_MS = 300000;
constexpr static usize STATUS_MAX_DIAGNOSTICS = 20;
constexpr static usize SCENE_SCRIPTS_MAX_ITEMS = 100;
constexpr static std::string_view NAME_ITEM = R"({"type":"string"})";

#ifdef OX_DOTNET
static auto kind_text(OxScriptKind kind) -> std::string_view {
  switch (kind) {
    case OxScriptKind::SceneBehaviour : return "SceneBehaviour";
    case OxScriptKind::EntityBehaviour: return "EntityBehaviour";
    case OxScriptKind::None           : break;
  }

  return "None";
}

static auto diagnostics_text(Editor& editor, const DotNetBuildResult& result) -> std::string {
  const auto is_error = [](const DotNetDiagnostic& diagnostic) {
    return diagnostic.severity == DotNetDiagnosticSeverity::Error;
  };
  const auto error_count = std::ranges::count_if(result.diagnostics, is_error);
  const auto warning_count = static_cast<i64>(result.diagnostics.size()) - error_count;

  std::string out = fmt::format("errors {}, warnings {}", error_count, warning_count);
  usize shown = 0;
  for (const auto& diagnostic : result.diagnostics) {
    if (shown == STATUS_MAX_DIAGNOSTICS) {
      break;
    }

    shown += 1;
    const auto file = diagnostic.file.empty() ? std::string("-") : project_relative_text(editor, diagnostic.file);
    out += fmt::format(
      "\n{} {}:{}:{} {} {}",
      is_error(diagnostic) ? "error" : "warning",
      file,
      diagnostic.line,
      diagnostic.column,
      diagnostic.code,
      diagnostic.message
    );
  }

  if (result.diagnostics.size() > shown) {
    out += fmt::format("\n... {} more diagnostics, see log_read", result.diagnostics.size() - shown);
  }

  return out;
}

static auto build_state_text(const DotNetBuildResult& result) -> std::string_view {
  if (result.is_dotnet_missing) {
    return "dotnet missing";
  }

  return result.is_success ? "ok" : "failed";
}

static auto last_build_text(Editor& editor) -> std::string {
  const auto& scripts = editor.csharp_scripts;
  const auto& last = scripts.last_build_result();
  if (!last.has_value()) {
    return "last build: none yet";
  }

  const auto finished = scripts.last_build_finish_time();
  const auto age = finished.has_value()
                     ? std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - *finished)
                         .count()
                     : 0;
  return fmt::format(
    "last build: {}, finished {}s ago, {}",
    build_state_text(*last),
    age,
    diagnostics_text(editor, *last)
  );
}

static auto script_status(Editor& editor, const McpArgs&) -> McpResult {
  ZoneScoped;

  const auto& scripts = editor.csharp_scripts;
  if (!scripts.is_open()) {
    return McpResult::ok("scripts: closed, the project has no C# scripts project or no project is open");
  }

  const auto& config = editor.active_project->get_config();
  return McpResult::ok(
    fmt::format(
      "scripts: open\ncsproj: {}\nbuilding: {}\nloading: {}\nbuild generation: {}\nload generation: {}\n{}",
      config.scripts_project.generic_string(),
      scripts.is_building() ? "yes" : "no",
      scripts.is_loading() ? "yes" : "no",
      scripts.build_generation(),
      scripts.load_generation(),
      last_build_text(editor)
    )
  );
}

static auto script_build(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto wait = args.opt_boolean("wait");
  if (!wait) {
    return McpResult::error(wait.error());
  }

  auto& scripts = editor.csharp_scripts;
  if (!scripts.is_open()) {
    return McpResult::error("the project has no open C# scripts project");
  }

  const auto is_already_building = scripts.is_building();
  const auto target_generation = scripts.build_generation() + (is_already_building ? 2 : 1);
  scripts.request_build();
  if (!wait->value_or(true)) {
    return McpResult::ok("build requested, read the result with script_status or editor_wait with 'scripts'");
  }

  return McpResult::pending([&editor, target_generation, is_already_building]() -> option<McpResult> {
    const auto& current = editor.csharp_scripts;
    if (!current.is_open()) {
      return McpResult::error("the project was closed during the build");
    }

    if (current.build_generation() < target_generation) {
      return nullopt;
    }

    const auto& last = current.last_build_result();
    const auto is_deferred = current.is_loading() && !current.is_building() &&
                             editor.main_viewport_panel.is_any_scene_playing();
    if (current.is_loading() && !is_deferred) {
      return nullopt;
    }

    std::string out = fmt::format("build {}\n", last.has_value() ? build_state_text(*last) : "unknown");
    if (is_already_building) {
      out += "note: a build was already running, this is the build that started after it\n";
    }

    if (is_deferred) {
      out += "note: a scene is playing, the new build loads after Stop\n";
    }

    out += last.has_value() ? diagnostics_text(editor, *last) : std::string("no result");
    return last.has_value() && !last->is_success ? McpResult::error(std::move(out)) : McpResult::ok(std::move(out));
  });
}

static auto script_types(Editor&, const McpArgs&) -> McpResult {
  ZoneScoped;

  if (!App::has_mod<DotNetScripting>()) {
    return McpResult::error("the C# scripting module is not loaded");
  }

  const auto& dotnet = App::mod<DotNetScripting>();
  if (!dotnet.is_loaded()) {
    return McpResult::error("C# scripts aren't loaded yet, run script_build and check script_status");
  }

  if (dotnet.script_types.empty()) {
    return McpResult::ok("no script types, create one with script_create");
  }

  auto types = dotnet.script_types;
  std::ranges::sort(types, [](const DotNetScriptType& lhs, const DotNetScriptType& rhs) {
    return lhs.name < rhs.name;
  });

  std::string out;
  for (const auto& type : types) {
    out += fmt::format("{} {}\n", type.name, kind_text(type.kind));
  }

  out += "EntityBehaviour types go into ScriptComponent.type_name through entity_update, SceneBehaviour types go "
         "through scene_scripts";
  return McpResult::ok(std::move(out));
}

static auto script_create(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto path_arg = args.str("path");
  const auto name = args.str("name");
  const auto kind_arg = args.str("kind");
  if (!path_arg || !name || !kind_arg) {
    return McpResult::error(!path_arg ? path_arg.error() : !name ? name.error() : kind_arg.error());
  }

  auto kind = CSharpScriptKind::EntityBehaviour;
  if (*kind_arg == "SceneBehaviour") {
    kind = CSharpScriptKind::SceneBehaviour;
  } else if (*kind_arg != "EntityBehaviour") {
    return McpResult::error(fmt::format("argument 'kind': '{}' is not SceneBehaviour or EntityBehaviour", *kind_arg));
  }

  const auto dir = resolve_content_path(editor, *path_arg, true);
  if (!dir) {
    return McpResult::error(fmt::format("argument 'path': {}", dir.error()));
  }

  const auto created = create_csharp_script(editor, *dir, kind, *name);
  if (!created) {
    return McpResult::error(created.error());
  }

  return McpResult::ok(
    fmt::format(
      "created {}\nthe scripts rebuild automatically when the file changes, use script_build to wait for the result",
      project_relative_text(editor, *created)
    )
  );
}

static auto script_kind_of(std::string_view name) -> OxScriptKind {
  if (!App::has_mod<DotNetScripting>()) {
    return OxScriptKind::None;
  }

  for (const auto& type : App::mod<DotNetScripting>().script_types) {
    if (type.name == name) {
      return type.kind;
    }
  }

  return OxScriptKind::None;
}

static auto name_list(const McpArgs& args, std::string_view key)
  -> std::expected<std::vector<std::string>, std::string> {
  std::vector<std::string> out;
  if (!args.has(key)) {
    return out;
  }

  const auto array = args.array(key);
  if (!array) {
    return std::unexpected(array.error());
  }

  if (array->size() > SCENE_SCRIPTS_MAX_ITEMS) {
    return std::unexpected(fmt::format("argument '{}': give at most {} names", key, SCENE_SCRIPTS_MAX_ITEMS));
  }

  usize index = 0;
  for (const auto element : *array) {
    std::string_view text;
    if (element.get(text)) {
      return std::unexpected(fmt::format("argument '{}[{}]' must be a string", key, index));
    }

    out.emplace_back(text);
    index += 1;
  }

  return out;
}

static auto scene_scripts_listing(Scene& scene) -> McpResult {
  std::vector<std::pair<std::string, std::string>> entries;
  for (const auto& [key, script] : scene.get_scripts()) {
    entries.emplace_back(key.type_name, script->display_name());
  }

  if (entries.empty()) {
    return McpResult::ok("the scene has no scripts");
  }

  std::ranges::sort(entries);
  std::string out;
  for (const auto& [type_name, display_name] : entries) {
    out += fmt::format("{} ({})\n", type_name, display_name);
  }

  out.pop_back();
  return McpResult::ok(std::move(out));
}

static auto scene_scripts_failed(EditTransaction& tx, std::string_view what) -> McpResult {
  const auto is_undone = tx.rollback();
  return McpResult::error(
    fmt::format(
      "{}. {}",
      what,
      is_undone ? "Nothing was applied."
                : "The scene is in Play mode, so the steps before this one were applied and can't be undone."
    )
  );
}

static auto scene_scripts(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto scene_text = scene_arg(args);
  if (!scene_text) {
    return McpResult::error(scene_text.error());
  }

  const auto text = scene_text->has_value() ? option<std::string_view>(std::string_view(scene_text->value())) : nullopt;
  const auto resolved = resolve_scene(editor, text);
  if (!resolved) {
    return McpResult::error(resolved.error());
  }

  auto scene = (*resolved)->get_scene();
  if (scene == nullptr) {
    return McpResult::error("the scene is not loaded");
  }

  const auto to_add = name_list(args, "add");
  const auto to_remove = name_list(args, "remove");
  if (!to_add || !to_remove) {
    return McpResult::error(!to_add ? to_add.error() : to_remove.error());
  }

  if (to_add->empty() && to_remove->empty()) {
    return scene_scripts_listing(*scene);
  }

  auto tx = begin_edit(editor, *scene, "mcp: scene_scripts");
  const auto weak_scene = std::weak_ptr<Scene>(scene);
  std::vector<std::string> lines;

  for (const auto& name : *to_remove) {
    const auto key = ScriptKey{.type_name = name};
    if (!scene->get_scripts().contains(key)) {
      return scene_scripts_failed(tx, fmt::format("remove '{}': the scene has no such script", name));
    }

    scene->remove_script(key);
    tx.record(
      std::make_unique<LambdaCommand>(
        [weak_scene, name] {
          if (const auto locked = weak_scene.lock()) {
            locked->remove_script(ScriptKey{.type_name = name});
          }
        },
        [weak_scene, name] {
          const auto locked = weak_scene.lock();
          if (!locked) {
            return;
          }

          if (const auto added = locked->add_script_type(name); !added) {
            OX_LOG_ERROR("{}", added.error());
          }
        },
        "mcp: scene_scripts"
      )
    );
    lines.push_back(fmt::format("removed {}", name));
  }

  for (const auto& name : *to_add) {
    const auto kind = script_kind_of(name);
    if (kind != OxScriptKind::SceneBehaviour) {
      return scene_scripts_failed(
        tx,
        fmt::format(
          "add '{}': {}",
          name,
          kind == OxScriptKind::None ? "no such script type, see script_types"
                                     : "not a SceneBehaviour, entity behaviours go through ScriptComponent"
        )
      );
    }

    if (scene->get_scripts().contains(ScriptKey{.type_name = name})) {
      return scene_scripts_failed(tx, fmt::format("add '{}': the scene already has this script", name));
    }

    if (const auto added = scene->add_script_type(name); !added) {
      return scene_scripts_failed(tx, fmt::format("add '{}': {}", name, added.error()));
    }

    tx.record(
      std::make_unique<LambdaCommand>(
        [weak_scene, name] {
          const auto locked = weak_scene.lock();
          if (!locked) {
            return;
          }

          if (const auto added = locked->add_script_type(name); !added) {
            OX_LOG_ERROR("{}", added.error());
          }
        },
        [weak_scene, name] {
          if (const auto locked = weak_scene.lock()) {
            locked->remove_script(ScriptKey{.type_name = name});
          }
        },
        "mcp: scene_scripts"
      )
    );
    lines.push_back(fmt::format("added {}", name));
  }

  const auto is_recording = tx.is_recording;
  tx.commit();

  std::string out;
  for (const auto& line : lines) {
    out += line;
    out += '\n';
  }

  if (!is_recording) {
    out += "note: the scene is in Play mode, this edit is not undoable and is lost at Stop\n";
  }

  out.pop_back();
  return McpResult::ok(std::move(out));
}
#else
constexpr static std::string_view NOT_COMPILED = "C# scripting is not compiled into this editor";

static auto script_unavailable(Editor&, const McpArgs&) -> McpResult {
  return McpResult::error(std::string(NOT_COMPILED));
}
#endif

auto register_script_tools(McpServer& server) -> void {
  ZoneScoped;

#ifdef OX_DOTNET
  constexpr auto status_handler = script_status;
  constexpr auto build_handler = script_build;
  constexpr auto types_handler = script_types;
  constexpr auto create_handler = script_create;
  constexpr auto scene_scripts_handler = scene_scripts;
#else
  constexpr auto status_handler = script_unavailable;
  constexpr auto build_handler = script_unavailable;
  constexpr auto types_handler = script_unavailable;
  constexpr auto create_handler = script_unavailable;
  constexpr auto scene_scripts_handler = script_unavailable;
#endif

  server.add_tool(
    McpTool{
      .name = "script_status",
      .title = "C# script status",
      .description = "State of the project's C# scripts: whether the scripts project is open, building or loading, the "
                     "build and load generation counters, and the last build with the time since it finished, error "
                     "and warning counts and its first 20 diagnostics as 'severity file:line:col code message'.",
      .input_schema = SchemaBuilder{}.build(),
      .is_read_only = true,
      .is_idempotent = true,
      .handler = status_handler,
    }
  );

  server.add_tool(
    McpTool{
      .name = "script_build",
      .title = "Build C# scripts",
      .description = "Builds the project's C# scripts now. With wait (default true) returns when the build is done and "
                     "loaded, with the diagnostics block of script_status, as an error when it failed. When a scene is "
                     "playing the new build loads after Stop and the call returns after the build with a note. When a "
                     "build is already running this waits for the build after it, so the result reflects the files as "
                     "they are now. Takes up to 300 s.",
      .input_schema = SchemaBuilder{}.boolean("wait", "wait for the build and load, default true", false).build(),
      .is_idempotent = true,
      .timeout_ms = BUILD_TIMEOUT_MS,
      .handler = build_handler,
    }
  );

  server.add_tool(
    McpTool{
      .name = "script_types",
      .title = "C# script types",
      .description = "Lists the script types of the loaded C# build as 'name kind'. EntityBehaviour types go into "
                     "ScriptComponent.type_name through entity_update, SceneBehaviour types go through scene_scripts.",
      .input_schema = SchemaBuilder{}.build(),
      .is_read_only = true,
      .is_idempotent = true,
      .handler = types_handler,
    }
  );

  server.add_tool(
    McpTool{
      .name = "script_create",
      .title = "Create C# script",
      .description = "Creates a new C# script file from the editor template. Returns the file path. The scripts "
                     "rebuild automatically when the file changes, use script_build to wait for the result.",
      .input_schema = SchemaBuilder{}
                        .string("path", "project-relative directory inside the asset directory", true)
                        .string("name", "script name, becomes the class name", true)
                        .string("kind", "what the script derives from", true, {"EntityBehaviour", "SceneBehaviour"})
                        .build(),
      .handler = create_handler,
    }
  );

  server.add_tool(
    McpTool{
      .name = "scene_scripts",
      .title = "Scene scripts",
      .description = "Lists the scene's C# scripts as 'type_name (display_name)', or adds and removes them. Only "
                     "SceneBehaviour types can be added, see script_types. Removes apply first. One call is one undo "
                     "step, but undoing a remove creates a fresh script and a removed script's runtime state is not "
                     "restored. If any name fails, everything the call did is undone.",
      .input_schema = SchemaBuilder{}
                        .string("scene", "scene id from editor_state or 'active', default active", false)
                        .array("add", "SceneBehaviour type names to add", NAME_ITEM, false)
                        .array("remove", "type names to remove", NAME_ITEM, false)
                        .build(),
      .handler = scene_scripts_handler,
    }
  );
}
} // namespace ox
