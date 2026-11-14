#include "Mcp/McpRefs.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <flecs/addons/meta.h>
#include <fmt/format.h>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Core/VFS.hpp"
#include "Editor.hpp"
#include "Memory/Stack.hpp"
#include "Panels/SceneHierarchyPanel.hpp"
#include "Utils/SceneManager.hpp"

namespace ox {
constexpr static usize NEAREST_COUNT = 3;
constexpr static std::string_view COMPONENT_SUFFIX = "Component";

static auto lower(std::string_view text) -> std::string {
  std::string out(text);
  std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

static auto edit_distance(std::string_view a, std::string_view b) -> usize {
  std::vector<usize> previous(b.size() + 1);
  std::vector<usize> current(b.size() + 1);
  for (usize j = 0; j <= b.size(); j++) {
    previous[j] = j;
  }

  for (usize i = 1; i <= a.size(); i++) {
    current[0] = i;
    for (usize j = 1; j <= b.size(); j++) {
      const auto substitution = previous[j - 1] + (a[i - 1] == b[j - 1] ? 0_sz : 1_sz);
      current[j] = std::min({previous[j] + 1, current[j - 1] + 1, substitution});
    }

    std::swap(previous, current);
  }

  return previous[b.size()];
}

static auto nearest_names(std::string_view needle, std::span<const std::string> candidates) -> std::string {
  const auto lowered_needle = lower(needle);

  std::vector<std::pair<usize, const std::string*>> scored;
  scored.reserve(candidates.size());
  for (const auto& candidate : candidates) {
    scored.emplace_back(edit_distance(lowered_needle, lower(candidate)), &candidate);
  }

  const auto count = std::min(NEAREST_COUNT, scored.size());
  std::ranges::partial_sort(
    scored,
    scored.begin() + static_cast<std::ptrdiff_t>(count),
    [](const auto& a, const auto& b) { return a.first != b.first ? a.first < b.first : *a.second < *b.second; }
  );

  std::string out;
  for (usize i = 0; i < count; i++) {
    out += fmt::format("{}{}", i == 0 ? "" : ", ", *scored[i].second);
  }

  return out;
}

static auto entity_name(flecs::entity entity) -> std::string {
  const char* raw_name = entity.name().c_str();
  return raw_name != nullptr ? std::string(raw_name) : std::string();
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

auto scene_arg(const McpArgs& args) -> std::expected<option<std::string>, std::string> {
  simdjson::dom::object object;
  if (args.raw().get(object)) {
    return option<std::string>(nullopt);
  }

  simdjson::dom::element value;
  if (object.at_key("scene").get(value)) {
    return option<std::string>(nullopt);
  }

  switch (value.type()) {
    case simdjson::dom::element_type::NULL_VALUE: return option<std::string>(nullopt);
    case simdjson::dom::element_type::STRING    : return option<std::string>(std::string(std::string_view(value)));
    case simdjson::dom::element_type::INT64     : return option<std::string>(std::to_string(i64(value)));
    case simdjson::dom::element_type::UINT64    : return option<std::string>(std::to_string(u64(value)));
    default                                     : break;
  }

  return std::unexpected("argument 'scene': expected a scene id string or 'active'");
}

auto resolve_scene(Editor& editor, option<std::string_view> arg) -> std::expected<EditorScene*, std::string> {
  ZoneScoped;

  if (!arg.has_value() || *arg == "active") {
    auto* active = editor.editor_panel_registry.get<SceneHierarchyPanel>().get_scene();
    if (active == nullptr) {
      return std::unexpected("no scene is open");
    }

    return active;
  }

  const auto id = parse_decimal(*arg);
  if (!id.has_value()) {
    return std::unexpected(fmt::format("argument 'scene': '{}' is not a scene id, use a number or 'active'", *arg));
  }

  EditorScene* found = nullptr;
  editor.scene_manager.for_each_scene([&](SceneID scene_id, EditorScene& scene) {
    if (std::to_underlying(scene_id) == *id) {
      found = &scene;
    }
  });

  if (found == nullptr) {
    return std::unexpected(fmt::format("no scene with id {}, editor_state lists the open scenes", *arg));
  }

  return found;
}

auto is_scene_entity(flecs::entity entity) -> bool {
  return entity.id() != 0 && entity.is_alive() && entity.has<TransformComponent>() && !entity.has<Hidden>();
}

auto scene_roots(Scene& scene) -> std::vector<flecs::entity> {
  ZoneScoped;

  std::vector<flecs::entity> roots;
  scene.world.query_builder<TransformComponent>()
    .with(flecs::Disabled)
    .optional()
    .build()
    .each([&roots](flecs::entity entity, TransformComponent&) {
      if (entity.parent() == flecs::entity::null() && !entity.has<Hidden>()) {
        roots.push_back(entity);
      }
    });

  return roots;
}

auto scene_children(flecs::entity entity) -> std::vector<flecs::entity> {
  ZoneScoped;

  std::vector<flecs::entity> children;
  entity.children([&children](flecs::entity child) {
    if (is_scene_entity(child)) {
      children.push_back(child);
    }
  });

  return children;
}

auto entity_path(flecs::entity entity) -> std::string {
  std::string path;
  for (auto current = entity; current.is_valid() && current.is_alive(); current = current.parent()) {
    const char* raw_name = current.name().c_str();
    const std::string_view name = raw_name != nullptr ? raw_name : "";
    path = path.empty() ? std::string(name) : fmt::format("{}/{}", name, path);
  }

  return path;
}

auto entity_ref_text(flecs::entity entity) -> std::string {
  return fmt::format("#{} {}", entity.id(), entity_path(entity));
}

static auto sibling_names(Scene& scene, flecs::entity parent) -> std::vector<std::string> {
  std::vector<std::string> names;
  const auto siblings = parent.id() == 0 ? scene_roots(scene) : scene_children(parent);
  for (const auto& sibling : siblings) {
    names.push_back(entity_name(sibling));
  }

  return names;
}

auto resolve_entity(Scene& scene, std::string_view ref) -> std::expected<flecs::entity, std::string> {
  ZoneScoped;
  memory::ScopedStack stack;

  if (ref.empty()) {
    return std::unexpected("entity reference is empty, use a path like 'Level/Crate' or an id like '#1234'");
  }

  if (ref.front() == '#') {
    const auto id = parse_decimal(ref.substr(1));
    if (!id.has_value()) {
      return std::unexpected(fmt::format("'{}' is not an entity id, expected '#' followed by digits", ref));
    }

    const flecs::entity entity(scene.world, *id);
    if (!ecs_is_alive(scene.world, *id)) {
      return std::unexpected(fmt::format("entity {} does not exist", ref));
    }

    if (!is_scene_entity(entity)) {
      return std::unexpected(fmt::format("{} is not a scene entity", ref));
    }

    return entity;
  }

  auto parent = flecs::entity::null();
  std::string walked;
  auto remaining = ref;
  while (true) {
    const auto slash = remaining.find('/');
    const auto segment = remaining.substr(0, slash);
    if (segment.empty()) {
      return std::unexpected(fmt::format("entity path '{}' has an empty segment", ref));
    }

    const auto found_id = ecs_lookup_child(scene.world, parent.id(), stack.null_terminate_cstr(segment));
    const flecs::entity found(scene.world, found_id);
    const auto is_wanted = found_id != 0 && is_scene_entity(found) &&
                           (parent.id() == 0 ? found.parent() == flecs::entity::null() : true);
    if (!is_wanted) {
      const auto names = sibling_names(scene, parent);
      const auto nearest = nearest_names(segment, names);
      return std::unexpected(
        fmt::format(
          "no entity '{}' {}{}",
          segment,
          parent.id() == 0 ? std::string("at the scene root") : fmt::format("under '{}'", walked),
          nearest.empty() ? std::string() : fmt::format(", nearest: {}", nearest)
        )
      );
    }

    parent = found;
    walked = walked.empty() ? std::string(segment) : fmt::format("{}/{}", walked, segment);
    if (slash == std::string_view::npos) {
      return found;
    }

    remaining = remaining.substr(slash + 1);
  }
}

auto is_component_type(flecs::world& world, flecs::id id) -> bool {
  if (!id.is_entity()) {
    return false;
  }

  const auto type = id.entity();
  if (!type.is_alive() || !type.has<flecs::Component>()) {
    return false;
  }

  const auto* type_info = ecs_get(world, type.id(), EcsType);
  if (type_info == nullptr) {
    return true;
  }

  switch (type_info->kind) {
    case EcsStructType: return true;
    default           : return false;
  }
}

auto component_name(flecs::id component) -> std::string {
  const char* raw_name = component.entity().name().c_str();
  return raw_name != nullptr ? std::string(raw_name) : std::string();
}

auto component_display_name(flecs::id component) -> std::string {
  auto name = component_name(component);
  if (name.size() > COMPONENT_SUFFIX.size() && name.ends_with(COMPONENT_SUFFIX)) {
    name.resize(name.size() - COMPONENT_SUFFIX.size());
  }

  return name;
}

auto known_components(Scene& scene) -> std::vector<flecs::id> {
  ZoneScoped;

  std::vector<flecs::id> out;
  for (const auto& id : scene.component_db.get_components()) {
    if (is_component_type(scene.world, id)) {
      out.push_back(id);
    }
  }

  return out;
}

auto resolve_component(Scene& scene, std::string_view name) -> std::expected<flecs::id, std::string> {
  ZoneScoped;

  const auto components = known_components(scene);
  const auto lowered = lower(name);

  std::vector<flecs::id> exact;
  std::vector<flecs::id> loose;
  for (const auto& component : components) {
    const auto short_name = component_name(component);
    const auto path = std::string(component.entity().path().c_str());
    if (short_name == name || path == name) {
      exact.push_back(component);
      continue;
    }

    if (lower(short_name) == lowered || lower(component_display_name(component)) == lowered || lower(path) == lowered) {
      loose.push_back(component);
    }
  }

  const auto& matches = !exact.empty() ? exact : loose;
  if (matches.size() == 1) {
    return matches.front();
  }

  if (matches.size() > 1) {
    std::string candidates;
    for (const auto& match : matches) {
      candidates += fmt::format("{}{}", candidates.empty() ? "" : ", ", match.entity().path().c_str());
    }

    return std::unexpected(fmt::format("component '{}' is ambiguous, use one of: {}", name, candidates));
  }

  std::vector<std::string> names;
  for (const auto& component : components) {
    names.push_back(component_name(component));
  }

  return std::unexpected(
    fmt::format("unknown component '{}', nearest: {}, component_types lists all", name, nearest_names(name, names))
  );
}

auto resolve_asset(std::string_view ref) -> std::expected<UUID, std::string> {
  ZoneScoped;

  auto& asset_man = App::mod<AssetManager>();

  if (const auto uuid = UUID::from_string(ref); uuid.has_value()) {
    const auto guard = asset_man.get_asset(*uuid);
    if (!guard) {
      return std::unexpected(fmt::format("uuid {} is not a registered asset", ref));
    }

    return *uuid;
  }

  const std::filesystem::path as_given(ref);
  if (const auto found = asset_man.find_asset(as_given); found) {
    return found;
  }

  const auto in_assets = std::filesystem::path(VFS::ASSETS_DIR) / as_given;
  if (const auto found = asset_man.find_asset(in_assets); found) {
    return found;
  }

  // project-relative, or any form resolve_content_path takes. Standalone assets such as materials are registered
  // without their sidecar's extension
  if (App::has_mod<Editor>()) {
    if (const auto path = resolve_content_path(App::mod<Editor>(), ref, false)) {
      auto candidate = *path;
      if (candidate.extension() == ".oxasset") {
        candidate.replace_extension();
      }

      if (const auto found = asset_man.find_asset(candidate); found) {
        return found;
      }
    }
  }

  return std::unexpected(
    fmt::format(
      "no asset for '{}', tried it as a uuid, as the source path '{}' and as '{}'",
      ref,
      ref,
      in_assets.generic_string()
    )
  );
}

auto resolve_content_path(Editor& editor, const std::string_view text, const bool must_exist)
  -> std::expected<std::filesystem::path, std::string> {
  ZoneScoped;

  constexpr std::string_view virtual_prefix = VFS::ASSETS_DIR;
  if (
    text.starts_with(virtual_prefix) && (text.size() == virtual_prefix.size() || text[virtual_prefix.size()] == '/')
  ) {
    const auto physical = App::get_vfs().to_physical(std::filesystem::path(text));
    return resolve_project_path(editor, physical.string(), must_exist);
  }

  return resolve_project_path(editor, text, must_exist);
}

auto follow(Editor& editor, EditorScene& scene, flecs::entity entity) -> void {
  if (editor.editor_cvar.cvar_mcp_follow.get() == 0 || !entity.is_alive()) {
    return;
  }

  if (editor.editor_panel_registry.get<SceneHierarchyPanel>().get_scene() != &scene) {
    return;
  }

  editor.get_context().reset(EditorContext::Type::Entity, nullopt, entity);
}

auto resolve_project_path(Editor& editor, std::string_view text, bool must_exist)
  -> std::expected<std::filesystem::path, std::string> {
  ZoneScoped;

  const auto& project_dir = editor.active_project->get_project_directory();
  if (project_dir.empty()) {
    return std::unexpected("no project is open");
  }

  if (text.empty()) {
    return std::unexpected("the path is empty");
  }

  const std::filesystem::path given(text);
  for (const auto& part : given) {
    if (part == "..") {
      return std::unexpected(fmt::format("path '{}' must not contain '..'", text));
    }
  }

  std::error_code error;
  const auto root = std::filesystem::weakly_canonical(project_dir, error);
  const auto joined = given.is_absolute() ? given : project_dir / given;
  const auto resolved = std::filesystem::weakly_canonical(joined, error);
  if (error) {
    return std::unexpected(fmt::format("path '{}' could not be resolved: {}", text, error.message()));
  }

  const auto relative = resolved.lexically_relative(root);
  if (relative.empty() || *relative.begin() == "..") {
    return std::unexpected(fmt::format("path '{}' is outside the project directory", text));
  }

  if (must_exist && !std::filesystem::exists(resolved, error)) {
    return std::unexpected(fmt::format("path '{}' does not exist", text));
  }

  return resolved;
}

auto project_relative_text(Editor& editor, const std::filesystem::path& path) -> std::string {
  std::error_code error;
  const auto root = std::filesystem::weakly_canonical(editor.active_project->get_project_directory(), error);
  const auto resolved = std::filesystem::weakly_canonical(path, error);
  const auto relative = resolved.lexically_relative(root);
  if (relative.empty() || *relative.begin() == "..") {
    return resolved.generic_string();
  }

  return relative.generic_string();
}
} // namespace ox
