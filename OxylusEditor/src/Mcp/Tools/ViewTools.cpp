#include <array>
#include <fmt/format.h>
#include <memory>
#include <string>
#include <tracy/Tracy.hpp>

#include "Actions/ViewActions.hpp"
#include "Editor.hpp"
#include "Mcp/McpArgs.hpp"
#include "Mcp/McpRefs.hpp"
#include "Mcp/McpSchema.hpp"
#include "Mcp/McpServer.hpp"
#include "Mcp/McpTools.hpp"

namespace ox {
constexpr static u32 SCREENSHOT_DEFAULT_SIZE = 1024;
constexpr static i64 SCREENSHOT_MIN_SIZE = 64;
constexpr static i64 SCREENSHOT_MAX_SIZE = 4096;
constexpr static u32 SCREENSHOT_CAMERA_FRAMES = 2;

struct ViewTarget {
  ViewportPanel* viewport = nullptr;
  EditorScene* scene = nullptr;
};

static auto resolve_view(Editor& editor, const McpArgs& args) -> std::expected<ViewTarget, std::string> {
  const auto arg = scene_arg(args);
  if (!arg) {
    return std::unexpected(arg.error());
  }

  if (!arg->has_value()) {
    auto* viewport = default_viewport(editor);
    if (viewport == nullptr) {
      return std::unexpected("no viewport shows a scene, open one with scene_open or scene_focus");
    }

    return ViewTarget{.viewport = viewport, .scene = viewport->get_scene()};
  }

  const auto scene = resolve_scene(editor, std::string_view(arg->value()));
  if (!scene) {
    return std::unexpected(scene.error());
  }

  auto* viewport = editor.main_viewport_panel.find_viewport(*scene);
  if (viewport == nullptr) {
    return std::unexpected(
      fmt::format("scene {} has no viewport, scene_focus opens it", std::to_underlying((*scene)->get_id()))
    );
  }

  return ViewTarget{.viewport = viewport, .scene = *scene};
}

static auto number_array_arg(const McpArgs& args, std::string_view key, usize count)
  -> std::expected<option<std::array<f32, 3>>, std::string> {
  if (!args.has(key)) {
    return option<std::array<f32, 3>>(nullopt);
  }

  const auto array = args.array(key);
  if (!array) {
    return std::unexpected(array.error());
  }

  std::array<f32, 3> out = {};
  usize index = 0;
  for (const auto& element : *array) {
    double value = 0.0;
    if (index >= count || element.get(value)) {
      return std::unexpected(fmt::format("argument '{}': expected {} numbers", key, count));
    }

    out[index] = static_cast<f32>(value);
    index += 1;
  }

  if (index != count) {
    return std::unexpected(fmt::format("argument '{}': expected {} numbers", key, count));
  }

  return option<std::array<f32, 3>>(out);
}

static auto scene_view_text(const ViewTarget& target) -> std::string {
  return fmt::format("scene {}", std::to_underlying(target.scene->get_id()));
}

static auto camera_text(const ViewTarget& target) -> std::string {
  const auto pose = target.viewport->camera_pose();
  if (!pose.has_value()) {
    return fmt::format("{} has no editor camera", scene_view_text(target));
  }

  auto text = fmt::format(
    "{} camera position [{:.3f}, {:.3f}, {:.3f}] yaw {:.2f} pitch {:.2f} fov {:.2f} near {} far {} viewport {}x{}",
    scene_view_text(target),
    pose->position.x,
    pose->position.y,
    pose->position.z,
    pose->yaw_pitch.x,
    pose->yaw_pitch.y,
    pose->fov,
    pose->near_clip,
    pose->far_clip,
    static_cast<u32>(pose->viewport_size.x),
    static_cast<u32>(pose->viewport_size.y)
  );
  if (target.viewport->piloted_camera.is_alive()) {
    text += fmt::format(" looking through {}", entity_ref_text(target.viewport->piloted_camera));
  }

  return text;
}

static auto gizmo_text(const ViewTarget& target) -> std::string {
  const auto state = target.viewport->gizmo_state();
  constexpr std::array<std::string_view, 6> modes = {"none", "translate", "rotate", "scale", "universal", "bounds"};
  return fmt::format(
    "{} gizmo mode {} space {} snap {} snap_amount {} rotate_snap_amount {}",
    scene_view_text(target),
    modes[std::to_underlying(state.mode)],
    state.space == ViewGizmoSpace::World ? "world" : "local",
    state.use_snap ? "on" : "off",
    state.snap_amount,
    state.rotate_snap_amount
  );
}

struct ScreenshotState {
  Editor& editor;
  SceneID scene = SceneID::Invalid;
  u32 max_size = SCREENSHOT_DEFAULT_SIZE;
  flecs::entity_t camera_id = 0;
  flecs::entity_t previous_camera_id = 0;
  std::string camera_label = "editor";
  u64 frames_target = 0;
  std::shared_ptr<ViewCapture> capture = {};

  explicit ScreenshotState(Editor& owner) : editor(owner) {}

  ScreenshotState(const ScreenshotState&) = delete;
  auto operator=(const ScreenshotState&) -> ScreenshotState& = delete;

  ~ScreenshotState() {
    if (camera_id == 0) {
      return;
    }

    auto* viewport = find_scene_viewport(editor, scene);
    if (viewport == nullptr) {
      return;
    }

    if (previous_camera_id != 0 && viewport->get_scene() != nullptr) {
      const auto previous = flecs::entity(viewport->get_scene()->get_scene()->world, previous_camera_id);
      if (previous.is_alive()) {
        viewport->piloted_camera = previous;
        return;
      }
    }

    std::ignore = look_through(*viewport, nullopt);
  }
};

static auto view_screenshot(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto max_size = args.opt_integer("max_size");
  if (!max_size) {
    return McpResult::error(max_size.error());
  }

  if (max_size->has_value() && (**max_size < SCREENSHOT_MIN_SIZE || **max_size > SCREENSHOT_MAX_SIZE)) {
    return McpResult::error(fmt::format("max_size must be {} to {}", SCREENSHOT_MIN_SIZE, SCREENSHOT_MAX_SIZE));
  }

  const auto camera_ref = args.opt_str("camera");
  if (!camera_ref) {
    return McpResult::error(camera_ref.error());
  }

  const auto target = resolve_view(editor, args);
  if (!target) {
    return McpResult::error(target.error());
  }

  auto state = std::make_shared<ScreenshotState>(editor);
  state->scene = target->scene->get_id();
  state->max_size = static_cast<u32>(max_size->value_or(SCREENSHOT_DEFAULT_SIZE));
  state->frames_target = target->viewport->rendered_frames;

  if (camera_ref->has_value()) {
    const auto camera = resolve_entity(*target->scene->get_scene(), **camera_ref);
    if (!camera) {
      return McpResult::error(camera.error());
    }

    const auto previous = target->viewport->piloted_camera;
    if (const auto looked = look_through(*target->viewport, *camera); !looked) {
      return McpResult::error(looked.error());
    }

    state->camera_id = camera->id();
    state->previous_camera_id = previous.is_alive() ? previous.id() : 0;
    state->camera_label = entity_ref_text(*camera);
    state->frames_target += SCREENSHOT_CAMERA_FRAMES;
  }

  return McpResult::pending([state]() -> option<McpResult> {
    auto* viewport = find_scene_viewport(state->editor, state->scene);
    if (viewport == nullptr) {
      return McpResult::error("the viewport was closed before the screenshot finished");
    }

    if (!state->capture) {
      if (state->camera_id != 0 && viewport->piloted_camera.id() != state->camera_id) {
        return McpResult::error("another panel took over the viewport camera, close the cinematic editor and retry");
      }

      if (viewport->rendered_frames < state->frames_target) {
        return nullopt;
      }

      state->capture = capture_viewport(*viewport, state->max_size);
      return nullopt;
    }

    if (!state->capture->is_done.load(std::memory_order_acquire)) {
      return nullopt;
    }

    if (!state->capture->error.empty()) {
      return McpResult::error(state->capture->error);
    }

    return McpResult::image(
      std::move(state->capture->png),
      fmt::format(
        "scene {}, {}x{}, camera {}",
        std::to_underlying(state->scene),
        state->capture->width,
        state->capture->height,
        state->camera_label
      )
    );
  });
}

static auto view_camera_get(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto target = resolve_view(editor, args);
  if (!target) {
    return McpResult::error(target.error());
  }

  if (!target->viewport->camera_pose().has_value()) {
    return McpResult::error(camera_text(*target));
  }

  return McpResult::ok(camera_text(*target));
}

static auto view_camera_set(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto position = number_array_arg(args, "position", 3);
  if (!position) {
    return McpResult::error(position.error());
  }

  const auto yaw_pitch = number_array_arg(args, "yaw_pitch", 2);
  if (!yaw_pitch) {
    return McpResult::error(yaw_pitch.error());
  }

  const auto look_at = number_array_arg(args, "look_at", 3);
  if (!look_at) {
    return McpResult::error(look_at.error());
  }

  const auto fov = args.opt_number("fov");
  if (!fov) {
    return McpResult::error(fov.error());
  }

  if (!position->has_value() && !yaw_pitch->has_value() && !look_at->has_value() && !fov->has_value()) {
    return McpResult::error("give at least one of position, yaw_pitch, look_at or fov");
  }

  if (yaw_pitch->has_value() && look_at->has_value()) {
    return McpResult::error("give either yaw_pitch or look_at, not both");
  }

  if (fov->has_value() && (**fov < 1.0 || **fov > 179.0)) {
    return McpResult::error("fov must be 1 to 179 degrees");
  }

  const auto target = resolve_view(editor, args);
  if (!target) {
    return McpResult::error(target.error());
  }

  const auto to_vec3 = [](const option<std::array<f32, 3>>& value) -> option<glm::vec3> {
    return value.has_value() ? option<glm::vec3>(glm::vec3((*value)[0], (*value)[1], (*value)[2])) : nullopt;
  };
  const auto pose_yaw_pitch = yaw_pitch->has_value() ? option<glm::vec2>(glm::vec2((**yaw_pitch)[0], (**yaw_pitch)[1]))
                                                     : nullopt;
  const auto set = set_camera_pose(
    *target->viewport,
    to_vec3(*position),
    pose_yaw_pitch,
    to_vec3(*look_at),
    fov->has_value() ? option<f32>(static_cast<f32>(**fov)) : nullopt
  );
  if (!set) {
    return McpResult::error(set.error());
  }

  return McpResult::ok(camera_text(*target));
}

static auto view_focus(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto entity_ref = args.str("entity");
  if (!entity_ref) {
    return McpResult::error(entity_ref.error());
  }

  const auto target = resolve_view(editor, args);
  if (!target) {
    return McpResult::error(target.error());
  }

  const auto entity = resolve_entity(*target->scene->get_scene(), *entity_ref);
  if (!entity) {
    return McpResult::error(entity.error());
  }

  if (const auto focused = focus_camera_on(*target->viewport, *entity); !focused) {
    return McpResult::error(focused.error());
  }

  return McpResult::ok(fmt::format("focused on {}, {}", entity_ref_text(*entity), camera_text(*target)));
}

static auto view_pick(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto uv_arg = number_array_arg(args, "uv", 2);
  if (!uv_arg) {
    return McpResult::error(uv_arg.error());
  }

  const auto pixel_arg = number_array_arg(args, "pixel", 2);
  if (!pixel_arg) {
    return McpResult::error(pixel_arg.error());
  }

  const auto image_size_arg = number_array_arg(args, "image_size", 2);
  if (!image_size_arg) {
    return McpResult::error(image_size_arg.error());
  }

  glm::vec2 uv = {};
  if (uv_arg->has_value()) {
    uv = {(**uv_arg)[0], (**uv_arg)[1]};
  } else if (pixel_arg->has_value() && image_size_arg->has_value()) {
    const auto size = glm::vec2((**image_size_arg)[0], (**image_size_arg)[1]);
    if (size.x < 1.0f || size.y < 1.0f) {
      return McpResult::error("image_size must be positive");
    }

    uv = glm::vec2((**pixel_arg)[0], (**pixel_arg)[1]) / size;
  } else {
    return McpResult::error("give uv, or pixel together with image_size");
  }

  if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) {
    return McpResult::error("the point is outside the image");
  }

  const auto target = resolve_view(editor, args);
  if (!target) {
    return McpResult::error(target.error());
  }

  const auto scene_id = target->scene->get_id();
  const auto pick = request_pick(*target->viewport, uv);

  return McpResult::pending([&editor, scene_id, uv, pick]() -> option<McpResult> {
    if (!pick->is_done.load(std::memory_order_acquire)) {
      return nullopt;
    }

    if (!pick->error.empty()) {
      return McpResult::error(pick->error);
    }

    if (!pick->entity.has_value()) {
      return McpResult::ok(fmt::format("nothing under uv [{:.4f}, {:.4f}]", uv.x, uv.y));
    }

    const auto* viewport = find_scene_viewport(editor, scene_id);
    if (viewport == nullptr || viewport->get_scene() == nullptr) {
      return McpResult::error("the viewport was closed before the pick finished");
    }

    const auto entity = flecs::entity(viewport->get_scene()->get_scene()->world, *pick->entity);
    if (!entity.is_alive()) {
      return McpResult::ok("the picked entity was deleted");
    }

    return McpResult::ok(fmt::format("uv [{:.4f}, {:.4f}] {}", uv.x, uv.y, entity_ref_text(entity)));
  });
}

static auto view_gizmo(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto mode_arg = args.opt_str("mode");
  if (!mode_arg) {
    return McpResult::error(mode_arg.error());
  }

  const auto space_arg = args.opt_str("space");
  if (!space_arg) {
    return McpResult::error(space_arg.error());
  }

  const auto snap = args.opt_boolean("snap");
  if (!snap) {
    return McpResult::error(snap.error());
  }

  const auto snap_amount = args.opt_number("snap_amount");
  if (!snap_amount) {
    return McpResult::error(snap_amount.error());
  }

  const auto rotate_snap_amount = args.opt_number("rotate_snap_amount");
  if (!rotate_snap_amount) {
    return McpResult::error(rotate_snap_amount.error());
  }

  option<ViewGizmoMode> mode = nullopt;
  if (mode_arg->has_value()) {
    const auto text = **mode_arg;
    if (text == "none") {
      mode = ViewGizmoMode::None;
    } else if (text == "translate") {
      mode = ViewGizmoMode::Translate;
    } else if (text == "rotate") {
      mode = ViewGizmoMode::Rotate;
    } else if (text == "scale") {
      mode = ViewGizmoMode::Scale;
    } else if (text == "universal") {
      mode = ViewGizmoMode::Universal;
    } else {
      return McpResult::error(fmt::format("unknown gizmo mode '{}'", text));
    }
  }

  option<ViewGizmoSpace> space = nullopt;
  if (space_arg->has_value()) {
    const auto text = **space_arg;
    if (text == "local") {
      space = ViewGizmoSpace::Local;
    } else if (text == "world") {
      space = ViewGizmoSpace::World;
    } else {
      return McpResult::error(fmt::format("unknown gizmo space '{}', use local or world", text));
    }
  }

  if (
    (snap_amount->has_value() && **snap_amount <= 0.0) ||
    (rotate_snap_amount->has_value() && **rotate_snap_amount <= 0.0)
  ) {
    return McpResult::error("snap amounts must be positive");
  }

  const auto target = resolve_view(editor, args);
  if (!target) {
    return McpResult::error(target.error());
  }

  set_gizmo(
    *target->viewport,
    mode,
    space,
    *snap,
    snap_amount->has_value() ? option<f32>(static_cast<f32>(**snap_amount)) : nullopt,
    rotate_snap_amount->has_value() ? option<f32>(static_cast<f32>(**rotate_snap_amount)) : nullopt
  );

  return McpResult::ok(gizmo_text(*target));
}

static auto scene_property(SchemaBuilder builder) -> SchemaBuilder {
  return builder.string("scene", "scene id from scene_list or 'active', default the focused viewport's scene", false);
}

auto register_view_tools(McpServer& server) -> void {
  ZoneScoped;

  server.add_tool(
    McpTool{
      .name = "view_screenshot",
      .title = "Screenshot viewport",
      .description =
        "Returns the viewport image as a PNG, downscaled so its longest side is at most max_size. It shows what "
        "the scene renders plus the editor grid and the selection highlight. Transform gizmos and component icons "
        "are drawn by the UI on top and are not in it. Give camera, an entity with a camera component, to shoot "
        "through it: the viewport looks through it for the shot and goes back afterwards. Waits for the viewport "
        "to render, so a hidden or minimized viewport times out.",
      .input_schema =
        scene_property(SchemaBuilder{})
          .string(
            "camera",
            "camera entity to look through for this shot, path or #id, default the editor camera",
            false
          )
          .integer("max_size", "longest side in pixels, default 1024", false, SCREENSHOT_MIN_SIZE, SCREENSHOT_MAX_SIZE)
          .build(),
      .is_read_only = true,
      .timeout_ms = 30000,
      .handler = view_screenshot,
    }
  );

  server.add_tool(
    McpTool{
      .name = "view_camera_get",
      .title = "Get viewport camera",
      .description = "The viewport camera: position, yaw and pitch in degrees, fov, clip planes, the viewport size "
                     "and the camera entity it looks through, if any.",
      .input_schema = scene_property(SchemaBuilder{}).build(),
      .is_read_only = true,
      .is_idempotent = true,
      .handler = view_camera_get,
    }
  );

  server.add_tool(
    McpTool{
      .name = "view_camera_set",
      .title = "Set viewport camera",
      .description = "Moves the editor camera instantly, with no smoothing. Give position and look_at together to aim "
                     "at a point, or position with yaw_pitch in degrees. Yaw 0 looks down -Z, positive yaw turns "
                     "left, positive pitch looks up. Parts you leave out keep their value. Ends looking through a "
                     "scene camera.",
      .input_schema =
        scene_property(SchemaBuilder{})
          .array("position", "camera position [x, y, z] in world space", R"({"type":"number"})", false)
          .array("yaw_pitch", "[yaw, pitch] in degrees", R"({"type":"number"})", false)
          .array("look_at", "world point [x, y, z] to aim at, instead of yaw_pitch", R"({"type":"number"})", false)
          .number("fov", "vertical field of view in degrees, 1 to 179", false)
          .build(),
      .is_idempotent = true,
      .handler = view_camera_set,
    }
  );

  server.add_tool(
    McpTool{
      .name = "view_focus",
      .title = "Focus viewport on entity",
      .description = "Moves the editor camera to look at an entity, the same as the F shortcut.",
      .input_schema = scene_property(SchemaBuilder{}).string("entity", "entity path or #id", true).build(),
      .is_idempotent = true,
      .handler = view_focus,
    }
  );

  server.add_tool(
    McpTool{
      .name = "view_pick",
      .title = "Pick entity in viewport",
      .description = "Returns the entity under a point of the viewport image, or nothing. Give uv, [0, 1] over the "
                     "image with (0, 0) at the top left, or pixel with image_size to use pixel coordinates of a "
                     "screenshot. Coordinates refer to the latest view_screenshot of the same camera, so move the "
                     "camera only through the same tools in between. Picks what the viewport renders now and "
                     "does not change the selection.",
      .input_schema = scene_property(SchemaBuilder{})
                        .array("uv", "[u, v] in 0 to 1", R"({"type":"number"})", false)
                        .array("pixel", "[x, y] in pixels of the screenshot", R"({"type":"number"})", false)
                        .array("image_size", "[width, height] of that screenshot", R"({"type":"number"})", false)
                        .build(),
      .is_read_only = true,
      .timeout_ms = 10000,
      .handler = view_pick,
    }
  );

  server.add_tool(
    McpTool{
      .name = "view_gizmo",
      .title = "Viewport gizmo",
      .description = "Sets the transform gizmo mode, space and snapping, and returns the resulting state. Call it "
                     "without arguments to read the state.",
      .input_schema = scene_property(SchemaBuilder{})
                        .string("mode", "gizmo mode", false, {"none", "translate", "rotate", "scale", "universal"})
                        .string("space", "gizmo space", false, {"local", "world"})
                        .boolean("snap", "snap to the step sizes", false)
                        .number("snap_amount", "translate and scale step", false)
                        .number("rotate_snap_amount", "rotation step in degrees", false)
                        .build(),
      .is_idempotent = true,
      .handler = view_gizmo,
    }
  );
}
} // namespace ox
