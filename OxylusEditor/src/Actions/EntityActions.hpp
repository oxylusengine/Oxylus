#pragma once

#include <array>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

#include "Core/Types.hpp"
#include "Core/UUID.hpp"
#include "Utils/Command.hpp"
#include "Utils/Log.hpp"

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

struct EntityPresetInfo {
  EntityPreset preset = EntityPreset::Empty;
  std::string_view name = {};
  std::string_view entity_name = {};
};

constexpr static auto ENTITY_PRESETS = std::array{
  EntityPresetInfo{EntityPreset::Empty, "empty", "entity"},
  EntityPresetInfo{EntityPreset::Sprite, "sprite", "sprite"},
  EntityPresetInfo{EntityPreset::Camera, "camera", "camera"},
  EntityPresetInfo{EntityPreset::Light, "light", "light"},
  EntityPresetInfo{EntityPreset::Sun, "sun", "sun"},
  EntityPresetInfo{EntityPreset::Terrain, "terrain", "terrain"},
  EntityPresetInfo{EntityPreset::ProbeVolume, "probe_volume", "probe_volume"},
  EntityPresetInfo{EntityPreset::AudioSource, "audio_source", "audio_source"},
  EntityPresetInfo{EntityPreset::AudioListener, "audio_listener", "audio_listener"},
  EntityPresetInfo{EntityPreset::ParticleSystem, "particle_system", "particle_system"},
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

template <typename Fn>
auto edit_scene(Editor& editor, Scene& scene, std::string label, Fn&& edit) {
  auto tx = begin_edit(editor, scene, std::move(label));
  auto result = edit(tx);
  if (!result) {
    OX_LOG_ERROR("{}", result.error());
  }

  tx.commit();
  return result;
}

// empty when `entity` can be called `name`
auto entity_name_error(const Scene& scene, flecs::entity entity, std::string_view name) -> std::string;

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
