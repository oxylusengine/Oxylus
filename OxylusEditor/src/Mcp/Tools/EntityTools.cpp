#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <cctype>
#include <charconv>
#include <flecs/addons/meta.h>
#include <fmt/format.h>
#include <tracy/Tracy.hpp>

#include "Actions/EntityActions.hpp"
#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Editor.hpp"
#include "Mcp/McpArgs.hpp"
#include "Mcp/McpComponentCodec.hpp"
#include "Mcp/McpRefs.hpp"
#include "Mcp/McpSchema.hpp"
#include "Mcp/McpServer.hpp"
#include "Mcp/McpTools.hpp"
#include "Utils/SceneManager.hpp"

namespace ox {
constexpr static i64 TREE_DEFAULT_LIMIT = 500;
constexpr static i64 TREE_MAX_LIMIT = 5000;
constexpr static i64 FIND_DEFAULT_LIMIT = 100;
constexpr static i64 FIND_MAX_LIMIT = 5000;
constexpr static usize GET_MAX_ENTITIES = 50;
constexpr static usize CREATE_MAX_ITEMS = 200;
constexpr static usize UPDATE_MAX_ITEMS = 200;
constexpr static usize DELETE_MAX_ITEMS = 200;
constexpr static i64 DUPLICATE_MAX_COUNT = 100;

struct SceneRef {
  EditorScene* editor_scene = nullptr;
  std::shared_ptr<Scene> scene = nullptr;
};

struct Mutation {
  Editor& editor;
  SceneRef target;
  EditTransaction tx;
  flecs::entity last = flecs::entity::null();
  std::vector<std::string> lines = {};
  std::vector<std::string> warnings = {};
};

using Step = std::expected<void, std::string>;

static auto lower(std::string_view text) -> std::string {
  std::string out(text);
  std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

static auto entity_name_of(flecs::entity entity) -> std::string {
  const char* raw_name = entity.name().c_str();
  return raw_name != nullptr ? std::string(raw_name) : std::string();
}

static auto open_scene(Editor& editor, const McpArgs& args) -> std::expected<SceneRef, std::string> {
  const auto arg = scene_arg(args);
  if (!arg) {
    return std::unexpected(arg.error());
  }

  const auto text = arg->has_value() ? option<std::string_view>(std::string_view(arg->value())) : nullopt;
  const auto resolved = resolve_scene(editor, text);
  if (!resolved) {
    return std::unexpected(resolved.error());
  }

  auto scene = (*resolved)->get_scene();
  if (scene == nullptr) {
    return std::unexpected("the scene is not loaded");
  }

  return SceneRef{.editor_scene = *resolved, .scene = std::move(scene)};
}

static auto begin_mutation(Editor& editor, SceneRef target, std::string_view tool) -> Mutation {
  auto& scene = *target.scene;
  return Mutation{
    .editor = editor,
    .target = std::move(target),
    .tx = begin_edit(editor, scene, fmt::format("mcp: {}", tool)),
  };
}

static auto fail(Mutation& mutation, std::string_view what) -> McpResult {
  const auto is_undone = mutation.tx.rollback();
  return McpResult::error(
    fmt::format(
      "{}. {}",
      what,
      is_undone ? "Nothing was applied."
                : "The scene is in Play mode, so the steps before this one were applied and can't be undone."
    )
  );
}

static auto touch(Mutation& mutation, std::string_view verb, flecs::entity entity) -> void {
  mutation.last = entity;
  mutation.lines.push_back(fmt::format("{} {}", verb, entity_ref_text(entity)));
}

static auto finish(Mutation& mutation) -> McpResult {
  const auto is_recording = mutation.tx.is_recording;
  mutation.tx.commit();

  if (mutation.last.id() != 0 && mutation.last.is_alive()) {
    follow(mutation.editor, *mutation.target.editor_scene, mutation.last);
  }

  std::string out;
  for (const auto& line : mutation.lines) {
    out += line;
    out += '\n';
  }

  for (const auto& warning : mutation.warnings) {
    out += fmt::format("warning: {}\n", warning);
  }

  if (!is_recording) {
    out += "note: the scene is in Play mode, this edit is not undoable and is lost at Stop\n";
  }

  if (!out.empty() && out.back() == '\n') {
    out.pop_back();
  }

  return McpResult::ok(std::move(out));
}

static auto raw_field(const McpArgs& item, std::string_view key) -> option<simdjson::dom::element> {
  simdjson::dom::object object;
  if (item.raw().get(object)) {
    return nullopt;
  }

  simdjson::dom::element value;
  if (object.at_key(key).get(value)) {
    return nullopt;
  }

  return value;
}

static auto parse_decimal(std::string_view text) -> option<u64> {
  u64 value = 0;
  const auto* end = text.data() + text.size();
  const auto result = std::from_chars(text.data(), end, value);
  if (text.empty() || result.ec != std::errc() || result.ptr != end) {
    return nullopt;
  }

  return value;
}

static auto element_string(simdjson::dom::element element, std::string_view where)
  -> std::expected<std::string_view, std::string> {
  std::string_view value;
  if (element.get(value)) {
    return std::unexpected(fmt::format("{}: expected a string", where));
  }

  return value;
}

static auto item_args(simdjson::dom::element element, usize index, std::string_view array_name)
  -> std::expected<McpArgs, std::string> {
  return McpArgs::from(element, fmt::format("{}[{}]", array_name, index));
}

static auto flecs_id_of(Scene& scene, flecs::id component) -> flecs::id {
  return flecs::id(scene.world, component.raw_id());
}

static auto apply_patch(Mutation& mutation, flecs::entity entity, flecs::id component, simdjson::dom::element patch)
  -> Step {
  auto& scene = *mutation.target.scene;

  simdjson::dom::object object;
  if (!patch.get(object) && object.size() == 0) {
    return {};
  }

  auto before = ComponentBytes::copy_of(entity, component.raw_id());
  if (before.is_empty()) {
    return std::unexpected(fmt::format("{} has no fields to set", component_name(component)));
  }

  auto target = before.clone();
  auto warnings = patch_component(scene, entity, component, patch, target);
  if (!warnings) {
    return std::unexpected(warnings.error());
  }

  for (auto& warning : *warnings) {
    mutation.warnings.push_back(std::move(warning));
  }

  return apply_component_snapshot(mutation.tx, entity, component, std::move(before), std::move(target));
}

static auto apply_component_set(Mutation& mutation, flecs::entity entity, const McpArgs& set, bool is_add_missing)
  -> Step {
  auto& scene = *mutation.target.scene;

  for (const auto& [key, value] : simdjson::dom::object(set.element)) {
    const auto component = resolve_component(scene, key);
    if (!component) {
      return std::unexpected(component.error());
    }

    if (!entity.has(component->raw_id())) {
      if (!is_add_missing) {
        return std::unexpected(
          fmt::format(
            "'{}' has no {}, put it in 'add' first or in the same item",
            entity_path(entity),
            component_name(*component)
          )
        );
      }

      if (const auto added = add_component(mutation.tx, entity, flecs_id_of(scene, *component)); !added) {
        return added;
      }
    }

    if (const auto patched = apply_patch(mutation, entity, flecs_id_of(scene, *component), value); !patched) {
      return patched;
    }
  }

  return {};
}

static auto resolve_optional_parent(Scene& scene, const McpArgs& item) -> std::expected<flecs::entity, std::string> {
  const auto parent = item.opt_str("parent");
  if (!parent) {
    return std::unexpected(parent.error());
  }

  if (!parent->has_value()) {
    return flecs::entity::null();
  }

  const auto resolved = resolve_entity(scene, parent->value());
  if (!resolved) {
    return std::unexpected(fmt::format("parent: {}", resolved.error()));
  }

  return *resolved;
}

struct PresetName {
  std::string_view name = {};
  EntityPreset preset = EntityPreset::Empty;
};

constexpr static PresetName PRESETS[] = {
  {"empty", EntityPreset::Empty},
  {"sprite", EntityPreset::Sprite},
  {"camera", EntityPreset::Camera},
  {"light", EntityPreset::Light},
  {"sun", EntityPreset::Sun},
  {"terrain", EntityPreset::Terrain},
  {"probe_volume", EntityPreset::ProbeVolume},
  {"audio_source", EntityPreset::AudioSource},
  {"audio_listener", EntityPreset::AudioListener},
  {"particle_system", EntityPreset::ParticleSystem},
};

static auto parse_preset(std::string_view text) -> std::expected<EntityPreset, std::string> {
  const auto lowered = lower(text);
  for (const auto& entry : PRESETS) {
    if (entry.name == lowered) {
      return entry.preset;
    }
  }

  std::string valid;
  for (const auto& entry : PRESETS) {
    valid += fmt::format("{}{}", valid.empty() ? "" : ", ", entry.name);
  }

  return std::unexpected(fmt::format("preset: unknown preset '{}', valid: {}", text, valid));
}

using ComponentNames = ankerl::unordered_dense::map<flecs::entity_t, std::string>;

static auto build_component_names(Scene& scene) -> ComponentNames {
  ComponentNames names;
  for (const auto& component : known_components(scene)) {
    names.emplace(component.raw_id(), component_display_name(component));
  }

  return names;
}

struct TreeOptions {
  std::string filter = {};
  i64 max_depth = 0;
  bool is_showing_components = true;
  const ComponentNames* names = nullptr;
};

static auto tree_line(const TreeOptions& options, flecs::entity entity, i64 level) -> std::string {
  auto line = fmt::format("{:{}}#{} {}", "", static_cast<usize>(level) * 2, entity.id(), entity_name_of(entity));
  if (!entity.enabled()) {
    line += " (disabled)";
  }

  if (options.is_showing_components) {
    std::string components;
    entity.each([&](flecs::id id) {
      if (!id.is_entity()) {
        return;
      }

      if (const auto found = options.names->find(id.raw_id()); found != options.names->end()) {
        components += fmt::format("{}{}", components.empty() ? "" : ", ", found->second);
      }
    });

    if (!components.empty()) {
      line += fmt::format(" [{}]", components);
    }
  }

  return line;
}

static auto collect_tree(const TreeOptions& options, flecs::entity entity, i64 level, std::vector<std::string>& out)
  -> bool {
  std::vector<std::string> child_lines;
  auto has_child_match = false;
  if (options.max_depth <= 0 || level + 1 < options.max_depth) {
    for (const auto& child : scene_children(entity)) {
      has_child_match = collect_tree(options, child, level + 1, child_lines) || has_child_match;
    }
  }

  const auto is_match = options.filter.empty() || lower(entity_name_of(entity)).contains(options.filter);
  if (!is_match && !has_child_match) {
    return false;
  }

  out.push_back(tree_line(options, entity, level));
  for (auto& line : child_lines) {
    out.push_back(std::move(line));
  }

  return true;
}

static auto entity_tree(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto target = open_scene(editor, args);
  if (!target) {
    return McpResult::error(target.error());
  }

  const auto root_arg = args.opt_str("root");
  const auto depth_arg = args.opt_integer("depth");
  const auto filter_arg = args.opt_str("filter");
  const auto components_arg = args.opt_boolean("components");
  const auto limit_arg = args.opt_integer("limit");
  const auto cursor_arg = args.opt_str("cursor");
  if (!root_arg || !depth_arg || !filter_arg || !components_arg || !limit_arg || !cursor_arg) {
    return McpResult::error(
      !root_arg         ? root_arg.error()
      : !depth_arg      ? depth_arg.error()
      : !filter_arg     ? filter_arg.error()
      : !components_arg ? components_arg.error()
      : !limit_arg      ? limit_arg.error()
                        : cursor_arg.error()
    );
  }

  auto& scene = *target->scene;
  const auto names = build_component_names(scene);
  const auto options = TreeOptions{
    .filter = lower(filter_arg->value_or("")),
    .max_depth = std::max<i64>(depth_arg->value_or(0), 0),
    .is_showing_components = components_arg->value_or(true),
    .names = &names,
  };

  const auto limit = static_cast<usize>(std::clamp<i64>(limit_arg->value_or(TREE_DEFAULT_LIMIT), 1, TREE_MAX_LIMIT));
  usize skip = 0;
  if (cursor_arg->has_value()) {
    const auto parsed = parse_decimal(cursor_arg->value());
    if (!parsed.has_value()) {
      return McpResult::error("argument 'cursor': not a cursor returned by this tool");
    }

    skip = static_cast<usize>(*parsed);
  }

  std::vector<std::string> lines;
  std::string root_text = "/";
  if (root_arg->has_value()) {
    const auto root = resolve_entity(scene, root_arg->value());
    if (!root) {
      return McpResult::error(fmt::format("argument 'root': {}", root.error()));
    }

    root_text = fmt::format("#{} {}", root->id(), entity_path(*root));
    collect_tree(options, *root, 0, lines);
  } else {
    for (const auto& root : scene_roots(scene)) {
      collect_tree(options, root, 0, lines);
    }
  }

  const auto first = std::min(skip, lines.size());
  const auto last = std::min(first + limit, lines.size());
  const auto& scene_path = target->editor_scene->get_path();

  std::string out = fmt::format(
    "scene {} ({}) root {} lines {}-{} of {}\n",
    std::to_underlying(target->editor_scene->get_id()),
    scene_path.empty() ? "unsaved" : scene_path.string(),
    root_text,
    lines.empty() ? 0 : first + 1,
    last,
    lines.size()
  );
  for (usize i = first; i < last; i++) {
    out += lines[i];
    out += '\n';
  }

  if (last < lines.size()) {
    out += fmt::format("next_cursor {}", last);
  } else if (!out.empty()) {
    out.pop_back();
  }

  return McpResult::ok(std::move(out));
}

static auto resolve_component_list(Scene& scene, const McpArgs& args, std::string_view key)
  -> std::expected<std::vector<flecs::id>, std::string> {
  std::vector<flecs::id> out;
  if (!args.has(key)) {
    return out;
  }

  const auto array = args.array(key);
  if (!array) {
    return std::unexpected(array.error());
  }

  usize index = 0;
  for (const auto element : *array) {
    const auto text = element_string(element, fmt::format("argument '{}[{}]'", key, index));
    if (!text) {
      return std::unexpected(text.error());
    }

    const auto component = resolve_component(scene, *text);
    if (!component) {
      return std::unexpected(fmt::format("argument '{}[{}]': {}", key, index, component.error()));
    }

    out.push_back(*component);
    index += 1;
  }

  return out;
}

static auto walk_scene(flecs::entity entity, const std::function<bool(flecs::entity)>& visit) -> bool {
  if (!visit(entity)) {
    return false;
  }

  for (const auto& child : scene_children(entity)) {
    if (!walk_scene(child, visit)) {
      return false;
    }
  }

  return true;
}

static auto entity_find(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto target = open_scene(editor, args);
  if (!target) {
    return McpResult::error(target.error());
  }

  const auto name_arg = args.opt_str("name");
  const auto limit_arg = args.opt_integer("limit");
  if (!name_arg || !limit_arg) {
    return McpResult::error(!name_arg ? name_arg.error() : limit_arg.error());
  }

  auto& scene = *target->scene;
  const auto required = resolve_component_list(scene, args, "component");
  if (!required) {
    return McpResult::error(required.error());
  }

  if (!name_arg->has_value() && required->empty()) {
    return McpResult::error("give at least one of 'name' or 'component'");
  }

  const auto name_filter = lower(name_arg->value_or(""));
  const auto limit = static_cast<usize>(std::clamp<i64>(limit_arg->value_or(FIND_DEFAULT_LIMIT), 1, FIND_MAX_LIMIT));

  std::string out;
  usize found = 0;
  auto is_truncated = false;
  const auto visit = [&](flecs::entity entity) {
    if (!name_filter.empty() && !lower(entity_name_of(entity)).contains(name_filter)) {
      return true;
    }

    const auto has_all = std::ranges::all_of(*required, [&](flecs::id component) {
      return entity.has(component.raw_id());
    });
    if (!has_all) {
      return true;
    }

    if (found == limit) {
      is_truncated = true;
      return false;
    }

    out += entity_ref_text(entity);
    out += '\n';
    found += 1;
    return true;
  };

  for (const auto& root : scene_roots(scene)) {
    if (!walk_scene(root, visit)) {
      break;
    }
  }

  if (found == 0) {
    return McpResult::ok("no matches");
  }

  if (is_truncated) {
    out += fmt::format("limit {} reached, more matches exist", limit);
  } else {
    out.pop_back();
  }

  return McpResult::ok(std::move(out));
}

static auto is_flecs_builtin(flecs::entity entity) -> bool {
  return std::string_view(entity.path().c_str()).starts_with("flecs");
}

static auto write_entity_json(
  JsonWriter& writer,
  Scene& scene,
  flecs::entity entity,
  const ComponentNames& names,
  std::span<const flecs::id> restrict_to,
  std::vector<UUID>& assets
) -> void {
  writer.begin_obj();
  writer["id"] = fmt::format("#{}", entity.id());
  writer["path"] = entity_path(entity);
  writer["enabled"] = entity.enabled();

  writer.key("parent");
  if (const auto parent = entity.parent(); parent.id() != 0 && is_scene_entity(parent)) {
    writer.str(entity_ref_text(parent));
  } else {
    writer.raw("null");
  }

  writer.key("children");
  writer.begin_array();
  for (const auto& child : scene_children(entity)) {
    writer.str(entity_ref_text(child));
  }
  writer.end_array();

  std::vector<flecs::id> components;
  std::vector<std::string> tags;
  entity.each([&](flecs::id id) {
    if (!id.is_entity()) {
      return;
    }

    if (names.contains(id.raw_id())) {
      const auto is_wanted = restrict_to.empty() ||
                             std::ranges::any_of(restrict_to, [&](flecs::id wanted) { return wanted == id; });
      if (is_wanted) {
        components.push_back(id);
      }

      return;
    }

    const auto type = id.entity();
    if (!type.has<flecs::Component>() && !is_flecs_builtin(type)) {
      tags.push_back(entity_name_of(type));
    }
  });

  writer.key("components");
  writer.begin_obj();
  for (const auto& component : components) {
    const auto* data = ecs_get_id(scene.world, entity.id(), component.raw_id());
    writer.key(component_name(component));
    component_to_json(writer, scene.world, component, data);
    collect_asset_fields(scene.world, component, data, assets);
  }
  writer.end_obj();

  writer.key("tags");
  writer.begin_array();
  for (const auto& tag : tags) {
    writer.str(tag);
  }
  writer.end_array();

  writer.end_obj();
}

static auto entity_get(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto target = open_scene(editor, args);
  if (!target) {
    return McpResult::error(target.error());
  }

  auto& scene = *target->scene;
  const auto refs = args.array("entities");
  if (!refs) {
    return McpResult::error(refs.error());
  }

  if (refs->size() == 0 || refs->size() > GET_MAX_ENTITIES) {
    return McpResult::error(fmt::format("argument 'entities': give 1 to {} references", GET_MAX_ENTITIES));
  }

  const auto restrict_to = resolve_component_list(scene, args, "components");
  if (!restrict_to) {
    return McpResult::error(restrict_to.error());
  }

  std::vector<flecs::entity> entities;
  usize index = 0;
  for (const auto element : *refs) {
    const auto text = element_string(element, fmt::format("argument 'entities[{}]'", index));
    if (!text) {
      return McpResult::error(text.error());
    }

    const auto entity = resolve_entity(scene, *text);
    if (!entity) {
      return McpResult::error(fmt::format("argument 'entities[{}]': {}", index, entity.error()));
    }

    entities.push_back(*entity);
    index += 1;
  }

  const auto names = build_component_names(scene);
  std::vector<UUID> assets;

  JsonWriter writer;
  writer.is_pretty = false;
  writer.begin_obj();
  writer.key("entities");
  writer.begin_array();
  for (const auto& entity : entities) {
    write_entity_json(writer, scene, entity, names, *restrict_to, assets);
  }
  writer.end_array();

  ankerl::unordered_dense::set<UUID> seen;
  writer.key("assets");
  writer.begin_obj();
  for (const auto& uuid : assets) {
    if (!seen.insert(uuid).second) {
      continue;
    }

    auto type = AssetType::None;
    std::string path;
    {
      const auto guard = App::mod<AssetManager>().get_asset(uuid);
      if (!guard) {
        continue;
      }

      type = guard->type;
      path = (guard->source_path.empty() ? guard->path : guard->source_path).generic_string();
    }

    writer.key(uuid.str());
    writer.begin_obj();
    writer["type"] = AssetManager::to_asset_type_sv(type);
    writer["path"] = path;
    writer.end_obj();
  }
  writer.end_obj();
  writer.end_obj();

  return McpResult::ok(writer.stream.str());
}

static auto create_item(Mutation& mutation, const McpArgs& item) -> Step {
  auto& scene = *mutation.target.scene;

  const auto name = item.opt_str("name");
  const auto preset_arg = item.opt_str("preset");
  if (!name || !preset_arg) {
    return std::unexpected(!name ? name.error() : preset_arg.error());
  }

  auto preset = EntityPreset::Empty;
  if (preset_arg->has_value()) {
    const auto parsed = parse_preset(preset_arg->value());
    if (!parsed) {
      return std::unexpected(parsed.error());
    }

    preset = *parsed;
  }

  const auto parent = resolve_optional_parent(scene, item);
  if (!parent) {
    return std::unexpected(parent.error());
  }

  const auto entity = create_entity(mutation.tx, name->value_or(""), *parent, preset);
  if (!entity) {
    return std::unexpected(fmt::format("create failed: {}", entity.error()));
  }

  touch(mutation, "created", *entity);

  if (item.has("components")) {
    const auto components = item.object("components");
    if (!components) {
      return std::unexpected(components.error());
    }

    if (const auto applied = apply_component_set(mutation, *entity, *components, true); !applied) {
      return std::unexpected(fmt::format("components on '{}': {}", entity_path(*entity), applied.error()));
    }
  }

  return {};
}

static auto entity_create(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto target = open_scene(editor, args);
  if (!target) {
    return McpResult::error(target.error());
  }

  const auto items = args.array("entities");
  if (!items) {
    return McpResult::error(items.error());
  }

  if (items->size() == 0 || items->size() > CREATE_MAX_ITEMS) {
    return McpResult::error(fmt::format("argument 'entities': give 1 to {} items", CREATE_MAX_ITEMS));
  }

  auto mutation = begin_mutation(editor, *target, "entity_create");
  usize index = 0;
  for (const auto element : *items) {
    const auto item = item_args(element, index, "entities");
    if (!item) {
      return fail(mutation, item.error());
    }

    if (const auto created = create_item(mutation, *item); !created) {
      return fail(mutation, fmt::format("entities[{}]: {}", index, created.error()));
    }

    index += 1;
  }

  return finish(mutation);
}

static auto entity_create_from_asset(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto target = open_scene(editor, args);
  if (!target) {
    return McpResult::error(target.error());
  }

  auto& scene = *target->scene;
  const auto asset_arg = args.str("asset");
  const auto name_arg = args.opt_str("name");
  if (!asset_arg || !name_arg) {
    return McpResult::error(!asset_arg ? asset_arg.error() : name_arg.error());
  }

  const auto asset = resolve_asset(*asset_arg);
  if (!asset) {
    return McpResult::error(fmt::format("argument 'asset': {}", asset.error()));
  }

  const auto parent = resolve_optional_parent(scene, args);
  if (!parent) {
    return McpResult::error(parent.error());
  }

  auto mutation = begin_mutation(editor, *target, "entity_create_from_asset");
  const auto entity = create_entity_from_asset(mutation.tx, *asset, *parent);
  if (!entity) {
    return fail(mutation, fmt::format("create from asset '{}' failed: {}", *asset_arg, entity.error()));
  }

  if (name_arg->has_value()) {
    if (const auto renamed = rename_entity(mutation.tx, *entity, name_arg->value()); !renamed) {
      return fail(mutation, fmt::format("rename of '{}' failed: {}", entity_path(*entity), renamed.error()));
    }
  }

  if (args.has("transform")) {
    const auto transform = args.object("transform");
    if (!transform) {
      return fail(mutation, transform.error());
    }

    const auto transform_id = flecs::id(scene.world, scene.world.entity<TransformComponent>().id());
    if (const auto patched = apply_patch(mutation, *entity, transform_id, transform->element); !patched) {
      return fail(mutation, fmt::format("transform of '{}' failed: {}", entity_path(*entity), patched.error()));
    }
  }

  touch(mutation, "created", *entity);
  return finish(mutation);
}

static auto update_item(Mutation& mutation, const McpArgs& item, std::string_view where) -> Step {
  auto& scene = *mutation.target.scene;

  const auto ref = item.str("entity");
  if (!ref) {
    return std::unexpected(ref.error());
  }

  const auto resolved = resolve_entity(scene, *ref);
  if (!resolved) {
    return std::unexpected(fmt::format("{}: {}", where, resolved.error()));
  }

  const auto entity = *resolved;
  const auto label = entity_ref_text(entity);
  const auto step_error = [&](std::string_view operation, std::string_view message) {
    return std::unexpected(fmt::format("{} '{}': {} failed: {}", where, label, operation, message));
  };

  const auto name = item.opt_str("name");
  const auto enabled = item.opt_boolean("enabled");
  if (!name || !enabled) {
    return std::unexpected(!name ? name.error() : enabled.error());
  }

  if (name->has_value()) {
    if (const auto renamed = rename_entity(mutation.tx, entity, name->value()); !renamed) {
      return step_error("rename", renamed.error());
    }
  }

  if (const auto parent_field = raw_field(item, "parent"); parent_field.has_value()) {
    auto parent = flecs::entity::null();
    if (parent_field->type() != simdjson::dom::element_type::NULL_VALUE) {
      const auto text = element_string(*parent_field, fmt::format("{}.parent", where));
      if (!text) {
        return std::unexpected(text.error());
      }

      const auto resolved_parent = resolve_entity(scene, *text);
      if (!resolved_parent) {
        return step_error("reparent", resolved_parent.error());
      }

      parent = *resolved_parent;
    }

    if (const auto reparented = reparent_entity(mutation.tx, entity, parent); !reparented) {
      return step_error("reparent", reparented.error());
    }
  }

  if (enabled->has_value()) {
    if (const auto toggled = set_entity_enabled(mutation.tx, entity, enabled->value()); !toggled) {
      return step_error("enable", toggled.error());
    }
  }

  const auto edit_list = [&](std::string_view key, bool is_adding) -> Step {
    if (!item.has(key)) {
      return {};
    }

    const auto names = item.array(key);
    if (!names) {
      return std::unexpected(names.error());
    }

    for (const auto element : *names) {
      const auto text = element_string(element, fmt::format("{}.{}", where, key));
      if (!text) {
        return std::unexpected(text.error());
      }

      const auto component = resolve_component(scene, *text);
      if (!component) {
        return step_error(key, component.error());
      }

      const auto result = is_adding ? add_component(mutation.tx, entity, flecs_id_of(scene, *component))
                                    : remove_component(mutation.tx, entity, flecs_id_of(scene, *component));
      if (!result) {
        return step_error(fmt::format("{} {}", key, *text), result.error());
      }
    }

    return {};
  };

  if (const auto removed = edit_list("remove", false); !removed) {
    return removed;
  }

  if (const auto added = edit_list("add", true); !added) {
    return added;
  }

  if (item.has("set")) {
    const auto set = item.object("set");
    if (!set) {
      return std::unexpected(set.error());
    }

    if (const auto applied = apply_component_set(mutation, entity, *set, false); !applied) {
      return step_error("set", applied.error());
    }
  }

  touch(mutation, "updated", entity);
  return {};
}

static auto entity_update(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto target = open_scene(editor, args);
  if (!target) {
    return McpResult::error(target.error());
  }

  const auto items = args.array("entities");
  if (!items) {
    return McpResult::error(items.error());
  }

  if (items->size() == 0 || items->size() > UPDATE_MAX_ITEMS) {
    return McpResult::error(fmt::format("argument 'entities': give 1 to {} items", UPDATE_MAX_ITEMS));
  }

  auto mutation = begin_mutation(editor, *target, "entity_update");
  usize index = 0;
  for (const auto element : *items) {
    const auto item = item_args(element, index, "entities");
    if (!item) {
      return fail(mutation, item.error());
    }

    if (const auto updated = update_item(mutation, *item, fmt::format("entities[{}]", index)); !updated) {
      return fail(mutation, updated.error());
    }

    index += 1;
  }

  return finish(mutation);
}

static auto entity_delete(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto target = open_scene(editor, args);
  if (!target) {
    return McpResult::error(target.error());
  }

  auto& scene = *target->scene;
  const auto refs = args.array("entities");
  if (!refs) {
    return McpResult::error(refs.error());
  }

  if (refs->size() == 0 || refs->size() > DELETE_MAX_ITEMS) {
    return McpResult::error(fmt::format("argument 'entities': give 1 to {} references", DELETE_MAX_ITEMS));
  }

  std::vector<flecs::entity> entities;
  std::vector<std::string> labels;
  usize index = 0;
  for (const auto element : *refs) {
    const auto text = element_string(element, fmt::format("argument 'entities[{}]'", index));
    if (!text) {
      return McpResult::error(text.error());
    }

    const auto entity = resolve_entity(scene, *text);
    if (!entity) {
      return McpResult::error(fmt::format("argument 'entities[{}]': {}", index, entity.error()));
    }

    entities.push_back(*entity);
    labels.push_back(entity_ref_text(*entity));
    index += 1;
  }

  auto mutation = begin_mutation(editor, *target, "entity_delete");
  for (usize i = 0; i < entities.size(); i++) {
    if (!entities[i].is_alive()) {
      mutation.lines.push_back(fmt::format("skipped {} (already deleted with its parent)", labels[i]));
      continue;
    }

    if (const auto deleted = delete_entity(mutation.tx, entities[i]); !deleted) {
      return fail(mutation, fmt::format("entities[{}] '{}': delete failed: {}", i, labels[i], deleted.error()));
    }

    mutation.lines.push_back(fmt::format("deleted {}", labels[i]));
  }

  return finish(mutation);
}

static auto entity_duplicate(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto target = open_scene(editor, args);
  if (!target) {
    return McpResult::error(target.error());
  }

  auto& scene = *target->scene;
  const auto ref = args.str("entity");
  const auto count_arg = args.opt_integer("count");
  if (!ref || !count_arg) {
    return McpResult::error(!ref ? ref.error() : count_arg.error());
  }

  const auto source = resolve_entity(scene, *ref);
  if (!source) {
    return McpResult::error(fmt::format("argument 'entity': {}", source.error()));
  }

  const auto count = count_arg->value_or(1);
  if (count < 1 || count > DUPLICATE_MAX_COUNT) {
    return McpResult::error(fmt::format("argument 'count': give 1 to {}", DUPLICATE_MAX_COUNT));
  }

  option<glm::vec3> offset = nullopt;
  if (
    const auto offset_field = raw_field(args, "offset");
    offset_field.has_value() && offset_field->type() != simdjson::dom::element_type::NULL_VALUE
  ) {
    simdjson::dom::array array;
    if (offset_field->get(array) || array.size() != 3) {
      return McpResult::error("argument 'offset': expected an array of 3 numbers [x,y,z]");
    }

    glm::vec3 value = {};
    for (usize i = 0; i < 3; i++) {
      double number = 0.0;
      if (array.at(i).get(number)) {
        return McpResult::error("argument 'offset': expected an array of 3 numbers [x,y,z]");
      }

      value[static_cast<i32>(i)] = static_cast<f32>(number);
    }

    offset = value;
  }

  const auto source_label = entity_ref_text(*source);
  auto mutation = begin_mutation(editor, *target, "entity_duplicate");
  const auto transform_id = flecs::id(scene.world, scene.world.entity<TransformComponent>().id());
  for (i64 i = 0; i < count; i++) {
    const auto clone = duplicate_entity(mutation.tx, *source);
    if (!clone) {
      return fail(mutation, fmt::format("copy {} of '{}': duplicate failed: {}", i + 1, source_label, clone.error()));
    }

    if (offset.has_value() && clone->has<TransformComponent>()) {
      auto before = ComponentBytes::copy_of(*clone, transform_id.raw_id());
      auto after = before.clone();
      static_cast<TransformComponent*>(after.mut_data())->position += *offset * static_cast<f32>(i + 1);
      if (
        const auto
          moved = apply_component_snapshot(mutation.tx, *clone, transform_id, std::move(before), std::move(after));
        !moved
      ) {
        return fail(mutation, fmt::format("copy {} of '{}': offset failed: {}", i + 1, source_label, moved.error()));
      }
    }

    touch(mutation, "created", *clone);
  }

  return finish(mutation);
}

static auto field_count(flecs::world& world, flecs::id component) -> option<i32> {
  const auto* members = ecs_get(world, component.raw_id(), EcsStruct);
  if (members == nullptr) {
    return nullopt;
  }

  return ecs_vec_count(&members->members);
}

static auto component_types(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto target = open_scene(editor, args);
  if (!target) {
    return McpResult::error(target.error());
  }

  auto& scene = *target->scene;
  const auto name = args.opt_str("name");
  if (!name) {
    return McpResult::error(name.error());
  }

  if (name->has_value()) {
    const auto component = resolve_component(scene, name->value());
    if (!component) {
      return McpResult::error(component.error());
    }

    JsonWriter writer;
    writer.is_pretty = false;
    component_schema_json(writer, scene, *component);
    return McpResult::ok(writer.stream.str());
  }

  std::string out;
  for (const auto& component : known_components(scene)) {
    const auto* type_info = ecs_get_type_info(scene.world, component.raw_id());
    if (type_info == nullptr || type_info->size == 0) {
      out += fmt::format("{} (tag)\n", component_name(component));
      continue;
    }

    const auto fields = field_count(scene.world, component);
    out += fmt::format("{} ({} fields)\n", component_name(component), fields.value_or(0));
  }

  if (!out.empty()) {
    out.pop_back();
  }

  return McpResult::ok(std::move(out));
}

static auto component_reset(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto target = open_scene(editor, args);
  if (!target) {
    return McpResult::error(target.error());
  }

  auto& scene = *target->scene;
  const auto ref = args.str("entity");
  const auto name = args.str("component");
  if (!ref || !name) {
    return McpResult::error(!ref ? ref.error() : name.error());
  }

  const auto entity = resolve_entity(scene, *ref);
  if (!entity) {
    return McpResult::error(fmt::format("argument 'entity': {}", entity.error()));
  }

  const auto component = resolve_component(scene, *name);
  if (!component) {
    return McpResult::error(fmt::format("argument 'component': {}", component.error()));
  }

  auto mutation = begin_mutation(editor, *target, "component_reset");
  if (const auto reset = reset_component(mutation.tx, *entity, flecs_id_of(scene, *component)); !reset) {
    return fail(
      mutation,
      fmt::format("reset of {} on '{}' failed: {}", component_name(*component), entity_path(*entity), reset.error())
    );
  }

  touch(mutation, "reset " + component_name(*component) + " on", *entity);
  return finish(mutation);
}

static auto component_copy(Editor& editor, const McpArgs& args) -> McpResult {
  ZoneScoped;

  const auto target = open_scene(editor, args);
  if (!target) {
    return McpResult::error(target.error());
  }

  auto& scene = *target->scene;
  const auto from_ref = args.str("from");
  const auto to_ref = args.str("to");
  const auto name = args.str("component");
  if (!from_ref || !to_ref || !name) {
    return McpResult::error(!from_ref ? from_ref.error() : !to_ref ? to_ref.error() : name.error());
  }

  const auto source = resolve_entity(scene, *from_ref);
  if (!source) {
    return McpResult::error(fmt::format("argument 'from': {}", source.error()));
  }

  const auto destination = resolve_entity(scene, *to_ref);
  if (!destination) {
    return McpResult::error(fmt::format("argument 'to': {}", destination.error()));
  }

  const auto component = resolve_component(scene, *name);
  if (!component) {
    return McpResult::error(fmt::format("argument 'component': {}", component.error()));
  }

  auto mutation = begin_mutation(editor, *target, "component_copy");
  if (
    const auto pasted = paste_component(mutation.tx, *source, *destination, flecs_id_of(scene, *component)); !pasted
  ) {
    return fail(
      mutation,
      fmt::format(
        "copy of {} from '{}' to '{}' failed: {}",
        component_name(*component),
        entity_path(*source),
        entity_path(*destination),
        pasted.error()
      )
    );
  }

  touch(mutation, "copied " + component_name(*component) + " to", *destination);
  return finish(mutation);
}

static auto with_scene(SchemaBuilder builder) -> SchemaBuilder {
  return builder.string("scene", "scene id from editor_state or 'active', default active", false);
}

constexpr static std::string_view REF_ITEM = R"({"type":"string"})";

constexpr static std::string_view CREATE_ITEM =
  R"({"type":"object","properties":{"name":{"type":"string","description":"entity name, made unique among siblings"},)"
  R"("parent":{"type":"string","description":"parent path or #id, may name an entity created earlier in this call"},)"
  R"("preset":{"type":"string","enum":["empty","sprite","camera","light","sun","terrain","probe_volume","audio_source","audio_listener","particle_system"]},)"
  R"("components":{"type":"object","description":"{ComponentName: {field: value}}, adds the component if missing"}},)"
  R"("additionalProperties":false})";

constexpr static std::string_view UPDATE_ITEM =
  R"({"type":"object","properties":{"entity":{"type":"string","description":"path or #id"},)"
  R"("name":{"type":"string"},"parent":{"type":["string","null"],"description":"new parent, null moves to the root"},)"
  R"("enabled":{"type":"boolean"},"add":{"type":"array","items":{"type":"string"}},)"
  R"("remove":{"type":"array","items":{"type":"string"}},)"
  R"("set":{"type":"object","description":"{ComponentName: {field: value}}, only the given fields change"}},)"
  R"("required":["entity"],"additionalProperties":false})";

constexpr static std::string_view
  PATCH_OBJECT = R"({"type":"object","description":"fields to set, see component_types"})";

auto register_entity_tools(McpServer& server) -> void {
  ZoneScoped;

  server.add_tool(
    McpTool{
      .name = "entity_tree",
      .title = "Entity tree",
      .description = "Outline of the scene hierarchy, one line per entity: indentation per depth, '#id name', "
                     "'(disabled)' and the component names in brackets. The first line names the scene and root, "
                     "'next_cursor N' ends a truncated listing.",
      .input_schema =
        with_scene(
          SchemaBuilder{}
            .string("root", "entity path or #id to start from, default all roots", false)
            .integer("depth", "levels to show, 1 is only the root, default unlimited", false, 1)
            .string("filter", "case-insensitive name substring, matches are shown with their ancestors", false)
            .boolean("components", "list component names, default true", false)
            .integer("limit", "lines to return, default 500", false, 1, TREE_MAX_LIMIT)
            .string("cursor", "next_cursor from the previous call", false)
        )
          .build(),
      .is_read_only = true,
      .is_idempotent = true,
      .handler = entity_tree,
    }
  );

  server.add_tool(
    McpTool{
      .name = "entity_find",
      .title = "Find entities",
      .description = "Finds scene entities by name substring and or required components. Returns one '#id path' line "
                     "per match in hierarchy order.",
      .input_schema = with_scene(
                        SchemaBuilder{}
                          .string("name", "case-insensitive name substring", false)
                          .array("component", "component names the entity must all have", REF_ITEM, false)
                          .integer("limit", "matches to return, default 100", false, 1, FIND_MAX_LIMIT)
      )
                        .build(),
      .is_read_only = true,
      .is_idempotent = true,
      .handler = entity_find,
    }
  );

  server.add_tool(
    McpTool{
      .name = "entity_get",
      .title = "Get entities",
      .description = "Reads up to 50 entities in full. Returns {\"entities\":[{id,path,enabled,parent,children,"
                     "components:{Name:{fields}},tags}],\"assets\":{uuid:{type,path}}} where assets names every "
                     "asset the returned components reference.",
      .input_schema = with_scene(
                        SchemaBuilder{}
                          .array("entities", "entity paths or #ids", REF_ITEM, true)
                          .array("components", "only return these components", REF_ITEM, false)
      )
                        .build(),
      .is_read_only = true,
      .is_idempotent = true,
      .handler = entity_get,
    }
  );

  server.add_tool(
    McpTool{
      .name = "entity_create",
      .title = "Create entities",
      .description = "Creates up to 200 entities in one undo step, each with an optional preset and component "
                     "values. Returns 'created #id path' per entity. If any item fails nothing is applied.",
      .input_schema = with_scene(SchemaBuilder{}.array("entities", "entities to create", CREATE_ITEM, true)).build(),
      .handler = entity_create,
    }
  );

  server.add_tool(
    McpTool{
      .name = "entity_create_from_asset",
      .title = "Create entity from asset",
      .description = "Places a model or particle system asset as a new entity hierarchy. Returns 'created #id path'.",
      .input_schema = with_scene(
                        SchemaBuilder{}
                          .string("asset", "asset uuid or source path", true)
                          .string("parent", "parent path or #id, default the scene root", false)
                          .string("name", "rename the new entity", false)
                          .object("transform", "TransformComponent fields to set", PATCH_OBJECT, false)
      )
                        .build(),
      .handler = entity_create_from_asset,
    }
  );

  server.add_tool(
    McpTool{
      .name = "entity_update",
      .title = "Update entities",
      .description = "Edits up to 200 entities in one undo step. Per entity the order is name, parent, enabled, "
                     "remove, add, set. Returns 'updated #id path' per entity. If any item fails nothing is applied.",
      .input_schema = with_scene(SchemaBuilder{}.array("entities", "edits to apply", UPDATE_ITEM, true)).build(),
      .handler = entity_update,
    }
  );

  server.add_tool(
    McpTool{
      .name = "entity_delete",
      .title = "Delete entities",
      .description = "Deletes entities with all their children in one undo step. Returns 'deleted #id path' per "
                     "entity, references already removed with a deleted parent are skipped.",
      .input_schema = with_scene(SchemaBuilder{}.array("entities", "entity paths or #ids", REF_ITEM, true)).build(),
      .is_destructive = true,
      .handler = entity_delete,
    }
  );

  server.add_tool(
    McpTool{
      .name = "entity_duplicate",
      .title = "Duplicate entity",
      .description = "Duplicates an entity with its children 'count' times, optionally moving each successive copy "
                     "by 'offset'. Returns 'created #id path' per copy.",
      .input_schema =
        with_scene(
          SchemaBuilder{}
            .string("entity", "path or #id", true)
            .integer("count", "copies to make, default 1", false, 1, DUPLICATE_MAX_COUNT)
            .array("offset", "[x,y,z] added to the position of each successive copy", R"({"type":"number"})", false)
        )
          .build(),
      .handler = entity_duplicate,
    }
  );

  server.add_tool(
    McpTool{
      .name = "component_types",
      .title = "Component types",
      .description = "Without a name, lists every component as 'Name (N fields)' or 'Name (tag)'. With a name returns "
                     "its schema as JSON: fields in order with type, enum constants, default value and asset type.",
      .input_schema = with_scene(SchemaBuilder{}.string("name", "component name, omit to list all", false)).build(),
      .is_read_only = true,
      .is_idempotent = true,
      .handler = component_types,
    }
  );

  server.add_tool(
    McpTool{
      .name = "component_reset",
      .title = "Reset component",
      .description = "Resets a component on an entity to its default values. Returns the entity reference.",
      .input_schema =
        with_scene(SchemaBuilder{}.string("entity", "path or #id", true).string("component", "component name", true))
          .build(),
      .handler = component_reset,
    }
  );

  server.add_tool(
    McpTool{
      .name = "component_copy",
      .title = "Copy component",
      .description = "Copies a component's values from one entity to another, adding it if missing. Returns the "
                     "destination reference.",
      .input_schema = with_scene(
                        SchemaBuilder{}
                          .string("from", "source entity path or #id", true)
                          .string("to", "destination entity path or #id", true)
                          .string("component", "component name", true)
      )
                        .build(),
      .handler = component_copy,
    }
  );
}
} // namespace ox
