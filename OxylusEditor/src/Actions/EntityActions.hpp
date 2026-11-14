#pragma once

#include <expected>
#include <memory>
#include <string>
#include <string_view>

#include "Core/Types.hpp"
#include "Core/UUID.hpp"
#include "Utils/Command.hpp"

namespace ox {
class Editor;

enum class EntityPreset : u8 {
  Empty,
  Sprite,
  Camera,
  Light,
  Sun,
  Terrain,
  ProbeVolume,
  AudioSource,
  AudioListener,
  ParticleSystem,
};

// is_recording is false for a scene in play mode, its changes are transient and never enter undo history
struct EditTransaction {
  Editor& editor;
  Scene& scene;
  std::unique_ptr<CommandGroup> group;
  bool is_recording = true;

  auto record(this EditTransaction& self, std::unique_ptr<Command> command) -> void;
  auto commit(this EditTransaction& self) -> void;
  // undoes everything recorded so far and drops the group, false when a play scene kept no record to undo from
  auto rollback(this EditTransaction& self) -> bool;
};

auto begin_edit(Editor& editor, Scene& scene, std::string label) -> EditTransaction;

auto is_entity_name_free(const Scene& scene, flecs::entity entity, std::string_view name) -> bool;

auto create_entity(EditTransaction& tx, std::string_view name, flecs::entity parent, EntityPreset preset)
  -> std::expected<flecs::entity, std::string>;
auto create_entity_from_asset(EditTransaction& tx, const UUID& asset, flecs::entity parent)
  -> std::expected<flecs::entity, std::string>;
auto delete_entity(EditTransaction& tx, flecs::entity entity) -> std::expected<void, std::string>;
auto duplicate_entity(EditTransaction& tx, flecs::entity entity) -> std::expected<flecs::entity, std::string>;
auto reparent_entity(EditTransaction& tx, flecs::entity entity, flecs::entity parent)
  -> std::expected<void, std::string>;
auto rename_entity(EditTransaction& tx, flecs::entity entity, std::string_view name)
  -> std::expected<void, std::string>;
auto set_entity_enabled(EditTransaction& tx, flecs::entity entity, bool is_enabled) -> std::expected<void, std::string>;

auto add_component(EditTransaction& tx, flecs::entity entity, flecs::id component) -> std::expected<void, std::string>;
auto remove_component(EditTransaction& tx, flecs::entity entity, flecs::id component)
  -> std::expected<void, std::string>;
auto reset_component(EditTransaction& tx, flecs::entity entity, flecs::id component)
  -> std::expected<void, std::string>;
auto paste_component(EditTransaction& tx, flecs::entity source, flecs::entity destination, flecs::id component)
  -> std::expected<void, std::string>;
auto add_mesh_colliders(EditTransaction& tx, flecs::entity entity) -> std::expected<void, std::string>;

// `before` is a copy of the component taken ahead of the edit, `after` is what to write over it
auto apply_component_snapshot(
  EditTransaction& tx, flecs::entity entity, flecs::id component, ComponentBytes before, ComponentBytes after
) -> std::expected<void, std::string>;
} // namespace ox
