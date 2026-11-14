#include <fmt/format.h>
#include <memory>
#include <string>
#include <tracy/Tracy.hpp>

#include "Actions/SceneActions.hpp"
#include "Editor.hpp"
#include "Mcp/McpArgs.hpp"
#include "Mcp/McpRefs.hpp"
#include "Mcp/McpSchema.hpp"
#include "Mcp/McpServer.hpp"
#include "Mcp/McpTools.hpp"

namespace ox {
constexpr static f32 RAYCAST_DEFAULT_DISTANCE = 1000.0f;

static auto scene_exists(Editor& editor, SceneID id) -> bool {
  auto is_found = false;
  editor.scene_manager.for_each_scene([&](SceneID scene_id, EditorScene&) { is_found = is_found || scene_id == id; });
  return is_found;
}

static auto target_scene(Editor& editor, const McpArgs& args) -> std::expected<EditorScene*, std::string> {
  const auto arg = scene_arg(args);
  if (!arg) {
    return std::unexpected(arg.error());
  }

  const auto text = arg->has_value() ? option<std::string_view>(std::string_view(arg->value())) : nullopt;
  return resolve_scene(editor, text);
}

static auto scene_label(Editor& editor, EditorScene& scene) -> std::string {
  const auto& path = scene.get_path();
  return path.empty() ? std::string("unsaved") : project_relative_text(editor, path);
}

static auto entity_count(Scene& scene) -> usize {
  usize count = 0;
  scene.world.query_builder<TransformComponent>()
    .with(flecs::Disabled)
    .optional()
    .build()
    .each([&count](flecs::entity entity, TransformComponent&) {
      if (!entity.has<Hidden>()) {
        count += 1;
      }
    });

  return count;
}

static auto scene_list(Editor& editor, const McpArgs&) -> McpResult {
  ZoneScoped;

  const auto* active_scene = editor.editor_panel_registry.get<SceneHierarchyPanel>().get_scene();
  const auto active_id = active_scene != nullptr ? active_scene->get_id() : SceneID::Invalid;

  std::string out;
  usize count = 0;
  editor.scene_manager.for_each_scene([&](SceneID id, EditorScene& scene) {
    count += 1;
    out += fmt::format(
      "scene {} {} {}{}",
      std::to_underlying(id),
      scene_label(editor, scene),
      scene.is_playing() ? "Play" : "Edit",
      id == active_id ? " active" : ""
    );
    if (scene.is_playing() && scene.source != SceneID::Invalid) {
      out += fmt::format(" source {}", std::to_underlying(scene.source));
    }

    out += fmt::format(" entities {}\n", scene.is_valid() ? entity_count(*scene.get_scene()) : 0);
  });

  if (count == 0) {
    return McpResult::ok("no scenes are open");
  }

  out.pop_back();
  return McpResult::ok(std::move(out));
}

static auto scene_new(Editor& editor, const McpArgs&) -> McpResult {
  ZoneScoped;

  auto* scene = new_scene(editor);
  return McpResult::ok(fmt::format("created scene {}", std::to_underlying(scene->get_id())));
}

static auto scene_open(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto path_arg = args.str("path");
  if (!path_arg) {
    return McpResult::error(path_arg.error());
  }

  const auto path = resolve_project_path(editor, *path_arg, true);
  if (!path) {
    return McpResult::error(path.error());
  }

  if (path->extension() != ".oxscene") {
    return McpResult::error(fmt::format("'{}' is not an .oxscene file", *path_arg));
  }

  if (auto* open = find_scene_by_path(editor, *path)) {
    std::ignore = focus_scene(editor, *open);
    return McpResult::ok(fmt::format("scene {} was already open, focused it", std::to_underlying(open->get_id())));
  }

  const auto opened = open_scene(editor, *path);
  if (!opened) {
    return McpResult::error(opened.error());
  }

  std::ignore = focus_scene(editor, **opened);
  return McpResult::ok(fmt::format("opened scene {}", std::to_underlying((*opened)->get_id())));
}

static auto scene_save(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto path_arg = args.opt_str("path");
  if (!path_arg) {
    return McpResult::error(path_arg.error());
  }

  const auto scene = target_scene(editor, args);
  if (!scene) {
    return McpResult::error(scene.error());
  }

  option<std::filesystem::path> path = nullopt;
  if (path_arg->has_value()) {
    const auto resolved = resolve_project_path(editor, path_arg->value(), false);
    if (!resolved) {
      return McpResult::error(resolved.error());
    }

    path = resolved.value();
  }

  const auto status = save_scene(editor, **scene, path);
  if (!status) {
    return McpResult::error(status.error());
  }

  return McpResult::pending([&editor, save_status = status.value()]() -> option<McpResult> {
    if (!save_status->is_done.load(std::memory_order_acquire)) {
      return nullopt;
    }

    if (!save_status->is_ok.load(std::memory_order_relaxed)) {
      return McpResult::error(save_status->error);
    }

    return McpResult::ok(fmt::format("saved {}", project_relative_text(editor, save_status->path)));
  });
}

static auto scene_close(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  if (!args.has("scene")) {
    return McpResult::error("argument 'scene' is required");
  }

  const auto scene = target_scene(editor, args);
  if (!scene) {
    return McpResult::error(scene.error());
  }

  const auto id = (*scene)->get_id();
  const auto was_playing = (*scene)->is_playing();
  const auto closed = close_scene(editor, **scene);
  if (!closed) {
    return McpResult::error(closed.error());
  }

  return McpResult::pending([&editor, id, was_playing]() -> option<McpResult> {
    if (scene_exists(editor, id)) {
      return nullopt;
    }

    return McpResult::ok(
      fmt::format(
        "closed scene {}{}",
        std::to_underlying(id),
        was_playing ? " (stopped the play scene)" : ", unsaved changes are lost, the editor does not track them"
      )
    );
  });
}

static auto scene_focus(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  if (!args.has("scene")) {
    return McpResult::error("argument 'scene' is required");
  }

  const auto scene = target_scene(editor, args);
  if (!scene) {
    return McpResult::error(scene.error());
  }

  const auto focused = focus_scene(editor, **scene);
  if (!focused) {
    return McpResult::error(focused.error());
  }

  const auto id = (*scene)->get_id();
  return McpResult::pending([&editor, id]() -> option<McpResult> {
    const auto* active = editor.editor_panel_registry.get<SceneHierarchyPanel>().get_scene();
    if (active == nullptr || active->get_id() != id) {
      return nullopt;
    }

    return McpResult::ok(fmt::format("scene {} is active", std::to_underlying(id)));
  });
}

static auto scene_play(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto scene = target_scene(editor, args);
  if (!scene) {
    return McpResult::error(scene.error());
  }

  const auto request = play_scene(editor, **scene);
  if (!request) {
    return McpResult::error(request.error());
  }

  return McpResult::pending([&editor, play_request = request.value()]() -> option<McpResult> {
    if (!scene_exists(editor, play_request.source)) {
      return McpResult::error("the scene was closed before the play scene started");
    }

    const auto* play = find_play_scene(editor, play_request.source);
    if (play == nullptr) {
      return nullopt;
    }

    return McpResult::ok(
      fmt::format(
        "play scene {} started from scene {}. Edits there are not undoable and vanish at Stop{}",
        std::to_underlying(play->get_id()),
        std::to_underlying(play_request.source),
        play_request.is_debugger_skipped ? ". 'Wait for Debugger on Play' is on and was ignored for this call" : ""
      )
    );
  });
}

static auto scene_stop(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto scene = target_scene(editor, args);
  if (!scene) {
    return McpResult::error(scene.error());
  }

  const auto stopped = stop_scene(editor, **scene);
  if (!stopped) {
    return McpResult::error(stopped.error());
  }

  return McpResult::pending([&editor, id = stopped.value()]() -> option<McpResult> {
    if (scene_exists(editor, id)) {
      return nullopt;
    }

    return McpResult::ok(fmt::format("stopped play scene {}", std::to_underlying(id)));
  });
}

static auto vec3_arg(const McpArgs& args, std::string_view key) -> std::expected<glm::vec3, std::string> {
  const auto array = args.array(key);
  if (!array) {
    return std::unexpected(array.error());
  }

  glm::vec3 out = {};
  usize index = 0;
  for (const auto& element : *array) {
    double value = 0.0;
    if (index >= 3 || element.get(value)) {
      return std::unexpected(fmt::format("argument '{}': expected [x, y, z] numbers", key));
    }

    out[static_cast<glm::length_t>(index)] = static_cast<f32>(value);
    index += 1;
  }

  if (index != 3) {
    return std::unexpected(fmt::format("argument '{}': expected [x, y, z] numbers", key));
  }

  return out;
}

static auto vec3_text(glm::vec3 value) -> std::string {
  return fmt::format("[{:.3f}, {:.3f}, {:.3f}]", value.x, value.y, value.z);
}

static auto scene_raycast(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto origin = vec3_arg(args, "origin");
  if (!origin) {
    return McpResult::error(origin.error());
  }

  const auto direction = vec3_arg(args, "direction");
  if (!direction) {
    return McpResult::error(direction.error());
  }

  const auto max_distance = args.opt_number("max_distance");
  if (!max_distance) {
    return McpResult::error(max_distance.error());
  }

  const auto target = target_scene(editor, args);
  if (!target) {
    return McpResult::error(target.error());
  }

  const auto distance = static_cast<f32>(max_distance->value_or(RAYCAST_DEFAULT_DISTANCE));
  if (distance <= 0.0f || glm::length(*direction) < 1e-6f) {
    return McpResult::error("max_distance must be positive and direction must not be zero");
  }

  const auto scene = (*target)->get_scene();
  const auto hit = raycast_scene(*scene, *origin, *direction, distance);
  if (!hit.has_value()) {
    return McpResult::ok("no hit");
  }

  return McpResult::ok(
    fmt::format(
      "hit {} point {} normal {} distance {:.3f}",
      hit->entity.id() != 0 ? entity_ref_text(hit->entity) : std::string("a body without an entity"),
      vec3_text(hit->point),
      vec3_text(hit->normal),
      hit->distance
    )
  );
}

static auto scene_property(SchemaBuilder builder) -> SchemaBuilder {
  return builder.string("scene", "scene id from scene_list or 'active', default active", false);
}

auto register_scene_tools(McpServer& server) -> void {
  ZoneScoped;

  server.add_tool(
    McpTool{
      .name = "scene_list",
      .title = "List scenes",
      .description = "One line per open scene: id, project-relative path or unsaved, Edit or Play, active, the source "
                     "scene of a play scene and the entity count.",
      .input_schema = SchemaBuilder{}.build(),
      .is_read_only = true,
      .is_idempotent = true,
      .handler = scene_list,
    }
  );

  server.add_tool(
    McpTool{
      .name = "scene_new",
      .title = "New scene",
      .description = "Opens a new scene with a sun and a camera and returns its id.",
      .input_schema = SchemaBuilder{}.build(),
      .handler = scene_new,
    }
  );

  server.add_tool(
    McpTool{
      .name = "scene_open",
      .title = "Open scene",
      .description = "Opens a project-relative .oxscene file and returns its scene id. A scene that is already open "
                     "is focused instead.",
      .input_schema = SchemaBuilder{}.string("path", "project-relative .oxscene path", true).build(),
      .handler = scene_open,
    }
  );

  server.add_tool(
    McpTool{
      .name = "scene_save",
      .title = "Save scene",
      .description = "Saves a scene and waits for the file to be written. Give a project-relative .oxscene path to "
                     "save as, it is required for a scene that has no file yet. Play scenes cannot be saved.",
      .input_schema = scene_property(SchemaBuilder{})
                        .string("path", "project-relative .oxscene path to save as", false)
                        .build(),
      .is_idempotent = true,
      .timeout_ms = 60000,
      .handler = scene_save,
    }
  );

  server.add_tool(
    McpTool{
      .name = "scene_close",
      .title = "Close scene",
      .description = "Closes a scene. Unsaved changes are lost, the editor does not track them, so save first. "
                     "Closing a play scene stops it. A scene with a running play scene cannot be closed.",
      .input_schema = SchemaBuilder{}.string("scene", "scene id from scene_list or 'active'", true).build(),
      .is_destructive = true,
      .handler = scene_close,
    }
  );

  server.add_tool(
    McpTool{
      .name = "scene_focus",
      .title = "Focus scene",
      .description = "Makes a scene the active one: its viewport takes focus and the hierarchy and inspector follow.",
      .input_schema = SchemaBuilder{}.string("scene", "scene id from scene_list", true).build(),
      .is_idempotent = true,
      .handler = scene_focus,
    }
  );

  server.add_tool(
    McpTool{
      .name = "scene_play",
      .title = "Play scene",
      .description = "Starts play mode. The runtime runs in a copy of the scene with its own id, which is returned. "
                     "A C# build may run first. Waits until the play scene exists.",
      .input_schema = scene_property(SchemaBuilder{}).build(),
      .timeout_ms = 120000,
      .handler = scene_play,
    }
  );

  server.add_tool(
    McpTool{
      .name = "scene_stop",
      .title = "Stop scene",
      .description = "Stops play mode and discards the play scene. Takes the play scene or the edit scene it came "
                     "from. Waits until the play scene is gone.",
      .input_schema = scene_property(SchemaBuilder{}).build(),
      .timeout_ms = 60000,
      .handler = scene_stop,
    }
  );

  server.add_tool(
    McpTool{
      .name = "scene_raycast",
      .title = "Raycast scene",
      .description = "Casts a ray against the scene's physics bodies, so only entities with colliders can be hit, and "
                     "terrain. Returns the first hit entity, point, normal and distance, or no hit. Play scenes have "
                     "the live physics state.",
      .input_schema =
        scene_property(SchemaBuilder{})
          .array("origin", "ray start [x, y, z] in world space", R"({"type":"number"})", true)
          .array("direction", "ray direction [x, y, z], need not be normalized", R"({"type":"number"})", true)
          .number("max_distance", "ray length, default 1000", false)
          .build(),
      .is_read_only = true,
      .is_idempotent = true,
      .handler = scene_raycast,
    }
  );
}
} // namespace ox
