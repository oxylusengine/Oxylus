#pragma once

#include <expected>
#include <filesystem>
#include <flecs.h>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Option.hpp"
#include "Core/Types.hpp"
#include "Core/UUID.hpp"
#include "Mcp/McpArgs.hpp"
#include "Scene/Scene.hpp"

namespace ox {
class Editor;
class EditorScene;

// the `scene` argument as text, accepting a string or an integer
auto scene_arg(const McpArgs& args) -> std::expected<option<std::string>, std::string>;

auto resolve_scene(Editor& editor, option<std::string_view> arg) -> std::expected<EditorScene*, std::string>;

// what the hierarchy shows: a transform and no Hidden tag. Everything else in the world is invisible to MCP
auto is_scene_entity(flecs::entity entity) -> bool;
auto scene_roots(Scene& scene) -> std::vector<flecs::entity>;
auto scene_children(flecs::entity entity) -> std::vector<flecs::entity>;

// `#1234` is a flecs id, anything else is a path `A/B/C` of exact names
auto resolve_entity(Scene& scene, std::string_view ref) -> std::expected<flecs::entity, std::string>;

auto entity_path(flecs::entity entity) -> std::string;
auto entity_ref_text(flecs::entity entity) -> std::string;

// a registered type an entity can carry: not an enum, primitive, opaque or container
auto is_component_type(flecs::world& world, flecs::id id) -> bool;
// `TransformComponent`
auto component_name(flecs::id component) -> std::string;
// `Transform`
auto component_display_name(flecs::id component) -> std::string;
auto known_components(Scene& scene) -> std::vector<flecs::id>;

auto resolve_component(Scene& scene, std::string_view name) -> std::expected<flecs::id, std::string>;

// a uuid, or the path of an asset's file: project-relative, absolute, or `assets_dir/...`
auto resolve_asset(std::string_view ref) -> std::expected<UUID, std::string>;

// resolve_project_path that also takes the virtual form `assets_dir/...`
auto resolve_content_path(Editor& editor, std::string_view text, bool must_exist)
  -> std::expected<std::filesystem::path, std::string>;

// relative to the project directory, or absolute inside it. `..` and symlinks that leave the project are rejected
auto resolve_project_path(Editor& editor, std::string_view text, bool must_exist)
  -> std::expected<std::filesystem::path, std::string>;
// what tools print: relative to the project directory with '/' separators
auto project_relative_text(Editor& editor, const std::filesystem::path& path) -> std::string;

auto follow(Editor& editor, EditorScene& scene, flecs::entity entity) -> void;
} // namespace ox
