#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fmt/chrono.h>
#include <fmt/format.h>
#include <string>
#include <tracy/Tracy.hpp>

#include "Actions/SceneActions.hpp"
#include "Core/App.hpp"
#include "Core/JobManager.hpp"
#include "Core/VFS.hpp"
#include "Editor.hpp"
#include "Mcp/McpArgs.hpp"
#include "Mcp/McpRefs.hpp"
#include "Mcp/McpSchema.hpp"
#include "Mcp/McpServer.hpp"
#include "Mcp/McpTools.hpp"
#include "OS/File.hpp"
#include "Panels/LoadingPanel.hpp"
#include "Panels/SceneHierarchyPanel.hpp"
#include "Panels/ViewportPanel.hpp"
#include "Project/Project.hpp"

namespace ox {
constexpr static usize STATE_LOG_ENTRIES = 5;
constexpr static usize STATE_JOB_NAMES = 8;
constexpr static usize STATE_TITLE_CHARS = 200;
constexpr static i64 LOG_DEFAULT_LIMIT = 100;
constexpr static i64 LOG_MAX_LIMIT = 1000;

static auto one_line(std::string_view text, usize max_chars) -> std::string {
  std::string out(text.substr(0, max_chars));
  std::ranges::replace(out, '\n', ' ');
  std::ranges::replace(out, '\r', ' ');
  return out;
}

static auto severity_rank(Notification::Type type) -> i32 {
  switch (type) {
    case Notification::Info   : return 0;
    case Notification::Loading: return 0;
    case Notification::Warn   : return 1;
    case Notification::Error  : return 2;
  }

  return 0;
}

static auto severity_letter(Notification::Type type) -> char {
  switch (type) {
    case Notification::Warn : return 'W';
    case Notification::Error: return 'E';
    default                 : return 'I';
  }
}

static auto to_lower(std::string_view text) -> std::string {
  std::string out(text);
  std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

static auto local_time_text(std::chrono::system_clock::time_point time) -> std::string {
  const auto seconds = std::chrono::system_clock::to_time_t(time);
  std::tm local = {};
#ifdef _WIN32
  localtime_s(&local, &seconds);
#else
  localtime_r(&seconds, &local);
#endif
  return fmt::format("{:%H:%M:%S}", local);
}

static auto editor_state(Editor& editor, const McpServer& server) -> std::string {
  ZoneScoped;

  editor.notification_system.drain_pending();

  std::string out;

  auto& project = *editor.active_project;
  const auto& project_dir = project.get_project_directory();
  if (project_dir.empty()) {
    out += "project: no project\n";
  } else {
    out += fmt::format("project: {} ({})\n", project.get_config().name, project_dir.string());
  }

  out += fmt::format("mcp: http://127.0.0.1:{}/mcp\n", server.bound_port);

  auto& hierarchy = editor.editor_panel_registry.get<SceneHierarchyPanel>();
  const auto* active_scene = hierarchy.get_scene();
  const auto active_id = active_scene != nullptr ? active_scene->get_id() : SceneID::Invalid;

  usize scene_count = 0;
  std::string scenes;
  editor.scene_manager.for_each_scene([&](SceneID id, EditorScene& scene) {
    scene_count += 1;
    const auto& path = scene.get_path();
    scenes += fmt::format(
      "  scene {} {} {}{}\n",
      std::to_underlying(id),
      path.empty() ? "unsaved" : path.string(),
      scene.is_playing() ? "Play" : "Edit",
      id == active_id ? " active" : ""
    );
  });
  out += fmt::format("scenes: {}\n{}", scene_count, scenes);

  const auto& context = editor.get_context();
  switch (context.type) {
    case EditorContext::Type::None  : out += "selection: none\n"; break;
    case EditorContext::Type::Entity: {
      if (context.entity.has_value() && context.entity->is_alive()) {
        out += fmt::format("selection: entity #{} {}\n", context.entity->id(), entity_path(context.entity.value()));
      } else {
        out += "selection: entity (gone)\n";
      }
      break;
    }
    case EditorContext::Type::File  : out += fmt::format("selection: file {}\n", context.str.value_or("")); break;
    case EditorContext::Type::Script: out += fmt::format("selection: script {}\n", context.str.value_or("")); break;
  }

  std::string panels;
  for (const auto& panel : editor.editor_panel_registry.panel_metadata) {
    if (panel.visible != nullptr && *panel.visible) {
      panels += panels.empty() ? panel.name : fmt::format(", {}", panel.name);
    }
  }
  out += fmt::format("panels: {}\n", panels.empty() ? "none" : panels);

  usize running_jobs = 0;
  std::string job_names;
  for (const auto& [name, is_running] : App::get_job_manager().get_tracker().get_status()) {
    if (!is_running) {
      continue;
    }

    running_jobs += 1;
    if (running_jobs <= STATE_JOB_NAMES) {
      job_names += fmt::format("{}{}", job_names.empty() ? "" : ", ", name);
    }
  }
  out += fmt::format("jobs: {} running{}{}\n", running_jobs, job_names.empty() ? "" : ": ", job_names);

  if (asset_scan_active()) {
    const auto progress = asset_scan_progress();
    out += fmt::format(
      "asset scan: active, compiled {}/{}, registered {}/{}, {}\n",
      progress.compiled_completed,
      progress.compiled_total,
      progress.registered_completed,
      progress.registered_total,
      progress.current_file
    );
  } else {
    out += "asset scan: idle\n";
  }

#ifdef OX_DOTNET
  out += fmt::format(
    "scripts: {}{}{}\n",
    editor.csharp_scripts.is_open() ? "open" : "closed",
    editor.csharp_scripts.is_building() ? ", building" : "",
    editor.csharp_scripts.is_loading() ? ", loading" : ""
  );
#endif

  const auto& undo_stack = editor.undo_redo_system->get_undo_stack();
  const auto& redo_stack = editor.undo_redo_system->get_redo_stack();
  out += fmt::format(
    "undo: {}{}\n",
    undo_stack.size(),
    undo_stack.empty() ? "" : fmt::format(", top '{}'", undo_stack.back()->get_id())
  );
  out += fmt::format(
    "redo: {}{}\n",
    redo_stack.size(),
    redo_stack.empty() ? "" : fmt::format(", top '{}'", redo_stack.back()->get_id())
  );

  std::string problems;
  usize problem_count = 0;
  const auto& history = editor.notification_system.notification_history;
  for (auto it = history.rbegin(); it != history.rend() && problem_count < STATE_LOG_ENTRIES; ++it) {
    if (severity_rank(it->type) >= 1) {
      problems = fmt::format(
                   "  {} {} {}\n",
                   it->id,
                   severity_letter(it->type),
                   one_line(it->title, STATE_TITLE_CHARS)
                 ) +
                 problems;
      problem_count += 1;
    }
  }
  out += fmt::format("recent problems: {}\n{}", problem_count == 0 ? "none" : std::to_string(problem_count), problems);

  return out;
}

static auto log_read(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto since_id = args.opt_integer("since_id");
  if (!since_id) {
    return McpResult::error(since_id.error());
  }

  const auto min_severity_arg = args.opt_str("min_severity");
  if (!min_severity_arg) {
    return McpResult::error(min_severity_arg.error());
  }

  const auto filter_arg = args.opt_str("filter");
  if (!filter_arg) {
    return McpResult::error(filter_arg.error());
  }

  const auto limit_arg = args.opt_integer("limit");
  if (!limit_arg) {
    return McpResult::error(limit_arg.error());
  }

  i32 min_rank = 0;
  const auto min_severity = min_severity_arg->value_or("info");
  if (min_severity == "warn") {
    min_rank = 1;
  } else if (min_severity == "error") {
    min_rank = 2;
  } else if (min_severity != "info") {
    return McpResult::error("argument 'min_severity': expected info, warn or error");
  }

  const auto limit = std::clamp<i64>(limit_arg->value_or(LOG_DEFAULT_LIMIT), 1, LOG_MAX_LIMIT);
  const auto after_id = static_cast<u64>(std::max<i64>(since_id->value_or(0), 0));
  const auto filter = to_lower(filter_arg->value_or(""));

  editor.notification_system.drain_pending();
  const auto& history = editor.notification_system.notification_history;

  std::vector<const Notification*> matches;
  for (const auto& entry : history) {
    if (entry.id <= after_id || severity_rank(entry.type) < min_rank) {
      continue;
    }

    if (!filter.empty() && to_lower(entry.title).find(filter) == std::string::npos) {
      continue;
    }

    matches.push_back(&entry);
  }

  const auto first = matches.size() > static_cast<usize>(limit) ? matches.size() - static_cast<usize>(limit) : 0;

  std::string out;
  for (usize i = first; i < matches.size(); i++) {
    const auto& entry = *matches[i];
    out += fmt::format(
      "{} {} {} {}",
      entry.id,
      local_time_text(entry.wall_time),
      severity_letter(entry.type),
      one_line(entry.title, entry.title.size())
    );
    if (entry.repeat_count > 1) {
      out += fmt::format(" (x{})", entry.repeat_count);
    }
    out += '\n';
  }

  const auto last_id = history.empty() ? after_id : std::max(after_id, history.back().id);
  out += fmt::format("last_id {}", last_id);

  return McpResult::ok(std::move(out));
}

static auto guide_dir() -> std::filesystem::path {
  return App::get_vfs().resolve_physical_dir(VFS::APP_DIR, "Mcp/guide");
}

static auto guide_topics() -> std::vector<std::pair<std::string, std::string>> {
  std::vector<std::pair<std::string, std::string>> topics;

  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(guide_dir(), error)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".md") {
      continue;
    }

    std::string heading;
    const auto content = File::to_string(entry.path());
    const auto line_end = content.find('\n');
    heading = content.substr(0, line_end);
    while (!heading.empty() && (heading.front() == '#' || heading.front() == ' ')) {
      heading.erase(heading.begin());
    }

    topics.emplace_back(entry.path().stem().string(), heading);
  }

  std::ranges::sort(topics);
  return topics;
}

static auto topic_list(std::span<const std::pair<std::string, std::string>> topics) -> std::string {
  std::string out;
  for (const auto& [name, heading] : topics) {
    out += fmt::format("{}: {}\n", name, heading);
  }

  return out;
}

static auto editor_guide(Editor&, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto topic = args.opt_str("topic");
  if (!topic) {
    return McpResult::error(topic.error());
  }

  const auto topics = guide_topics();
  if (!topic->has_value()) {
    return McpResult::ok("guide topics:\n" + topic_list(topics));
  }

  const auto name = topic->value();
  const auto is_valid_name = !name.empty() && std::ranges::all_of(name, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
  });

  const auto is_known = is_valid_name &&
                        std::ranges::any_of(topics, [&](const auto& item) { return item.first == name; });
  if (!is_known) {
    return McpResult::error(fmt::format("unknown topic '{}', valid topics:\n{}", name, topic_list(topics)));
  }

  const auto path = guide_dir() / fmt::format("{}.md", name);
  const auto content = File::try_to_string(path);
  if (!content) {
    return McpResult::error(fmt::format("could not read the guide '{}'", name));
  }

  return McpResult::ok(content.value());
}

static auto find_panel(Editor& editor, std::string_view name) -> PanelMetadata* {
  const auto wanted = to_lower(name);
  for (auto& panel : editor.editor_panel_registry.panel_metadata) {
    if (to_lower(panel.name) == wanted) {
      return &panel;
    }
  }

  return nullptr;
}

static auto panel_names(Editor& editor) -> std::string {
  std::string names;
  for (const auto& panel : editor.editor_panel_registry.panel_metadata) {
    names += fmt::format("{}{}", names.empty() ? "" : ", ", panel.name);
  }

  return names;
}

static auto visible_panels(Editor& editor) -> std::string {
  std::string panels;
  for (const auto& panel : editor.editor_panel_registry.panel_metadata) {
    if (panel.visible != nullptr && *panel.visible) {
      panels += fmt::format("{}{}", panels.empty() ? "" : ", ", panel.name);
    }
  }

  return panels.empty() ? "none" : panels;
}

static auto editor_select(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto entity_arg = args.opt_str("entity");
  if (!entity_arg) {
    return McpResult::error(entity_arg.error());
  }

  const auto file_arg = args.opt_str("file");
  if (!file_arg) {
    return McpResult::error(file_arg.error());
  }

  if (entity_arg->has_value() && file_arg->has_value()) {
    return McpResult::error("give an entity or a file, not both");
  }

  if (file_arg->has_value()) {
    const auto path = resolve_project_path(editor, file_arg->value(), true);
    if (!path) {
      return McpResult::error(path.error());
    }

    editor.get_context().reset(EditorContext::Type::File, path->string());
    return McpResult::ok(fmt::format("selected file {}", project_relative_text(editor, *path)));
  }

  if (!entity_arg->has_value()) {
    editor.get_context().reset();
    return McpResult::ok("selection cleared");
  }

  const auto scene_text = scene_arg(args);
  if (!scene_text) {
    return McpResult::error(scene_text.error());
  }

  const auto scene_ref = scene_text->has_value() ? option<std::string_view>(std::string_view(scene_text->value()))
                                                 : nullopt;
  const auto target = resolve_scene(editor, scene_ref);
  if (!target) {
    return McpResult::error(target.error());
  }

  const auto scene = (*target)->get_scene();
  const auto entity = resolve_entity(*scene, entity_arg->value());
  if (!entity) {
    return McpResult::error(entity.error());
  }

  std::ignore = focus_scene(editor, **target);
  editor.get_context().reset(EditorContext::Type::Entity, nullopt, entity.value());
  return McpResult::ok(fmt::format("selected {}", entity_ref_text(entity.value())));
}

static auto editor_ui(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto layout = args.opt_str("layout");
  if (!layout) {
    return McpResult::error(layout.error());
  }

  const auto fullscreen = args.opt_boolean("fullscreen");
  if (!fullscreen) {
    return McpResult::error(fullscreen.error());
  }

  if (layout->has_value() && layout->value() != "classic" && layout->value() != "big_viewport") {
    return McpResult::error("argument 'layout': expected classic or big_viewport");
  }

  std::vector<std::pair<PanelMetadata*, bool>> changes;
  if (args.has("panels")) {
    const auto panels = args.object("panels");
    if (!panels) {
      return McpResult::error(panels.error());
    }

    simdjson::dom::object object;
    if (panels->raw().get(object)) {
      return McpResult::error("argument 'panels': expected an object");
    }

    for (const auto& field : object) {
      auto* panel = find_panel(editor, field.key);
      if (panel == nullptr) {
        return McpResult::error(fmt::format("no panel '{}', valid panels: {}", field.key, panel_names(editor)));
      }

      if (field.value.type() != simdjson::dom::element_type::BOOL) {
        return McpResult::error(fmt::format("panel '{}': expected true or false", field.key));
      }

      changes.emplace_back(panel, bool(field.value));
    }
  }

  for (const auto& [panel, is_visible] : changes) {
    *panel->visible = is_visible;
  }

  if (fullscreen->has_value() && fullscreen->value() != editor.main_viewport_panel.is_fullscreen()) {
    editor.main_viewport_panel.toggle_fullscreen();
  }

  if (layout->has_value()) {
    editor.pending_layout = layout->value() == "classic" ? Editor::EditorLayout::Classic
                                                         : Editor::EditorLayout::BigViewport;
  }

  return McpResult::ok(
    fmt::format(
      "visible panels: {}\nfullscreen viewport: {}{}",
      visible_panels(editor),
      editor.main_viewport_panel.is_fullscreen() ? "on" : "off",
      layout->has_value() ? fmt::format("\nlayout {} applies on the next frame", layout->value()) : std::string()
    )
  );
}

static auto step_history(Editor& editor, const McpArgs& args, bool is_undo) -> McpResult {
  ZoneScoped;

  const auto count_arg = args.opt_integer("count");
  if (!count_arg) {
    return McpResult::error(count_arg.error());
  }

  const auto count = std::clamp<i64>(count_arg->value_or(1), 1, 100);
  auto& history = *editor.undo_redo_system;
  std::string out;
  for (i64 i = 0; i < count; i++) {
    const auto& stack = is_undo ? history.get_undo_stack() : history.get_redo_stack();
    if (stack.empty()) {
      break;
    }

    const auto id = stack.back()->get_id();
    if (!(is_undo ? history.undo() : history.redo())) {
      break;
    }

    out += fmt::format("{} {}\n", is_undo ? "undid" : "redid", id);
  }

  if (out.empty()) {
    return McpResult::ok(is_undo ? "nothing to undo" : "nothing to redo");
  }

  out.pop_back();
  return McpResult::ok(std::move(out));
}

static auto wait_text(std::string_view what, i64 elapsed_ms) -> std::string {
  return fmt::format("idle after {} ms: {}", elapsed_ms, what);
}

static auto is_jobs_idle() -> bool {
  for (const auto& [name, is_running] : App::get_job_manager().get_tracker().get_status()) {
    if (is_running && name != "Completion callback") {
      return false;
    }
  }

  return true;
}

static auto is_assets_idle(Editor& editor) -> bool {
  return !asset_scan_active() && !editor.pending_start_scene.has_value() &&
         !editor.editor_panel_registry.get<LoadingPanel>().is_loading();
}

static auto is_scripts_idle(Editor& editor) -> bool {
#ifdef OX_DOTNET
  return !editor.csharp_scripts.is_building() && !editor.csharp_scripts.is_loading();
#else
  std::ignore = editor;
  return true;
#endif
}

static auto editor_wait(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto timeout_arg = args.opt_integer("timeout_ms");
  if (!timeout_arg) {
    return McpResult::error(timeout_arg.error());
  }

  bool is_jobs = false;
  bool is_assets = false;
  bool is_scripts = false;
  if (args.has("for")) {
    const auto list = args.array("for");
    if (!list) {
      return McpResult::error(list.error());
    }

    for (const auto& item : *list) {
      std::string_view name;
      if (item.get(name)) {
        return McpResult::error("argument 'for': expected strings");
      }

      if (name == "jobs") {
        is_jobs = true;
      } else if (name == "assets") {
        is_assets = true;
      } else if (name == "scripts") {
        is_scripts = true;
      } else if (name == "all") {
        is_jobs = is_assets = is_scripts = true;
      } else {
        return McpResult::error("argument 'for': expected jobs, assets, scripts or all");
      }
    }
  }

  if (!is_jobs && !is_assets && !is_scripts) {
    is_jobs = is_assets = is_scripts = true;
  }

  const auto timeout_ms = std::clamp<i64>(timeout_arg->value_or(30000), 1, 600000);
  const auto started = std::chrono::steady_clock::now();
  std::string what;
  what += is_jobs ? "jobs " : "";
  what += is_assets ? "assets " : "";
  what += is_scripts ? "scripts " : "";
  what.pop_back();

  return McpResult::pending(
    [&editor, is_jobs, is_assets, is_scripts, timeout_ms, started, what]() -> option<McpResult> {
      editor.notification_system.drain_pending();
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started
      )
                             .count();

      std::string busy;
      busy += is_jobs && !is_jobs_idle() ? "jobs " : "";
      busy += is_assets && !is_assets_idle(editor) ? "assets " : "";
      busy += is_scripts && !is_scripts_idle(editor) ? "scripts " : "";
      if (busy.empty()) {
        return McpResult::ok(wait_text(what, elapsed));
      }

      if (elapsed >= timeout_ms) {
        busy.pop_back();
        return McpResult::error(fmt::format("still busy after {} ms: {}", elapsed, busy));
      }

      return nullopt;
    }
  );
}

static auto console_exec(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto command = args.str("command");
  if (!command) {
    return McpResult::error(command.error());
  }

  if (command->empty()) {
    return McpResult::error("the command is empty");
  }

  auto* viewport = editor.main_viewport_panel.get_focused_viewport();
  if (viewport == nullptr) {
    const auto visible = editor.main_viewport_panel.get_visible_viwports();
    for (auto* candidate : visible) {
      if (candidate->get_scene() != nullptr) {
        viewport = candidate;
        break;
      }
    }
  }

  if (viewport == nullptr) {
    return McpResult::error("no viewport is open, the console lives in a viewport");
  }

  const auto lines = viewport->execute_console(*command);
  if (lines.empty()) {
    return McpResult::ok("ok, the command logged nothing");
  }

  std::string out;
  for (const auto& line : lines) {
    out += line;
    out += '\n';
  }

  out.pop_back();
  return McpResult::ok(std::move(out));
}

auto register_editor_tools(McpServer& server) -> void {
  ZoneScoped;

  server.add_tool(
    McpTool{
      .name = "editor_state",
      .title = "Editor state",
      .description = "Summary of the live editor: project, open scenes, selection, visible panels, running jobs, "
                     "asset scan, script state, undo depth and recent warnings. Call this first to orient.",
      .input_schema = SchemaBuilder{}.build(),
      .is_read_only = true,
      .is_idempotent = true,
      .handler = [&server](Editor& editor, const McpArgs&) -> McpResult {
        return McpResult::ok(editor_state(editor, server));
      },
    }
  );

  server.add_tool(
    McpTool{
      .name = "log_read",
      .title = "Read editor log",
      .description = "Reads the editor log, one line per entry: id, time, severity (I, W or E) and text. Ends with "
                     "'last_id N', pass it as since_id to read only newer entries.",
      .input_schema =
        SchemaBuilder{}
          .integer("since_id", "only entries with an id greater than this", false, 0)
          .string("min_severity", "lowest severity to include, default info", false, {"info", "warn", "error"})
          .string("filter", "case-insensitive substring the text must contain", false)
          .integer("limit", "newest entries to return, default 100", false, 1, LOG_MAX_LIMIT)
          .build(),
      .is_read_only = true,
      .is_idempotent = true,
      .handler = log_read,
    }
  );

  server.add_tool(
    McpTool{
      .name = "editor_guide",
      .title = "Editor guide",
      .description = "Workflow guides for driving the editor. Without a topic it lists the topics, with one it "
                     "returns that guide. Read the relevant guide before a multi-step job.",
      .input_schema = SchemaBuilder{}.string("topic", "guide topic, omit to list all", false).build(),
      .is_read_only = true,
      .is_idempotent = true,
      .handler = editor_guide,
    }
  );

  server.add_tool(
    McpTool{
      .name = "editor_select",
      .title = "Select",
      .description = "Sets the editor selection the inspector shows. Give an entity (path or #id, in the scene "
                     "argument's scene) or a project-relative file. With neither the selection is cleared.",
      .input_schema = SchemaBuilder{}
                        .string("entity", "entity path or #id", false)
                        .string("file", "project-relative file path", false)
                        .string("scene", "scene id from scene_list or 'active', default active", false)
                        .build(),
      .is_idempotent = true,
      .handler = editor_select,
    }
  );

  server.add_tool(
    McpTool{
      .name = "editor_ui",
      .title = "Editor UI",
      .description = "Shows or hides editor panels, switches the dock layout and toggles the fullscreen viewport. "
                     "Returns the visible panels.",
      .input_schema = SchemaBuilder{}
                        .object(
                          "panels",
                          "panel name to true or false, names match case-insensitively",
                          "{\"type\":\"object\",\"additionalProperties\":{\"type\":\"boolean\"}}",
                          false
                        )
                        .string("layout", "dock layout", false, {"classic", "big_viewport"})
                        .boolean("fullscreen", "make the viewport fill the window", false)
                        .build(),
      .is_idempotent = true,
      .handler = editor_ui,
    }
  );

  server.add_tool(
    McpTool{
      .name = "editor_undo",
      .title = "Undo",
      .description = "Undoes the last edits and returns the id of each. Play scene edits are never in the history.",
      .input_schema = SchemaBuilder{}.integer("count", "how many to undo, default 1", false, 1, 100).build(),
      .handler = [](Editor& editor, const McpArgs& args) -> McpResult { return step_history(editor, args, true); },
    }
  );

  server.add_tool(
    McpTool{
      .name = "editor_redo",
      .title = "Redo",
      .description = "Redoes undone edits and returns the id of each.",
      .input_schema = SchemaBuilder{}.integer("count", "how many to redo, default 1", false, 1, 100).build(),
      .handler = [](Editor& editor, const McpArgs& args) -> McpResult { return step_history(editor, args, false); },
    }
  );

  server.add_tool(
    McpTool{
      .name = "editor_wait",
      .title = "Wait for idle",
      .description = "Waits until background work is done: jobs (imports, saves, cooks), assets (asset scan and "
                     "project open) or scripts (C# build and load). Default is all of them.",
      .input_schema = SchemaBuilder{}
                        .array(
                          "for",
                          "what to wait for: jobs, assets, scripts or all",
                          R"({"type":"string","enum":["jobs","assets","scripts","all"]})",
                          false
                        )
                        .integer("timeout_ms", "give up after this long, default 30000", false, 1, 600000)
                        .build(),
      .is_read_only = true,
      .is_idempotent = true,
      .timeout_ms = 600000,
      .handler = editor_wait,
    }
  );

  server.add_tool(
    McpTool{
      .name = "console_exec",
      .title = "Run console command",
      .description = "Runs a runtime console command (a cvar name with an optional value, help, clear) in the focused "
                     "or first viewport and returns the lines it logged.",
      .input_schema = SchemaBuilder{}.string("command", "console command line", true).build(),
      .handler = console_exec,
    }
  );
}
} // namespace ox
