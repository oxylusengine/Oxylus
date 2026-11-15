#include "Actions/EntityActions.hpp"

#include <algorithm>
#include <ankerl/svector.h>
#include <glm/trigonometric.hpp>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Editor.hpp"
#include "Memory/Stack.hpp"

namespace ox {
static auto is_live(flecs::entity entity) -> bool { return entity.id() != 0 && entity.is_alive(); }

static auto remap_of(EditTransaction& tx) -> EntityRemap* { return &tx.editor.undo_redo_system->remap; }

static auto entity_label(flecs::entity entity) -> std::string {
  const auto* name = entity.name().c_str();
  return name[0] != '\0' ? std::string(name) : fmt::format("#{}", entity.id());
}

static auto component_label(flecs::id component) -> std::string { return std::string(component.str().c_str()); }

static auto default_name(const EntityPreset preset) -> std::string_view {
  const auto info = std::ranges::find(ENTITY_PRESETS, preset, &EntityPresetInfo::preset);
  return info != ENTITY_PRESETS.end() ? info->entity_name : "entity";
}

static auto apply_preset(flecs::entity entity, EntityPreset preset) -> void {
  switch (preset) {
    case EntityPreset::Empty : break;
    case EntityPreset::Sprite: {
      entity.add<SpriteComponent>();
    } break;
    case EntityPreset::Camera: {
      entity.add<CameraComponent>().get_mut<TransformComponent>().rotation = glm::quat(
        glm::vec3(0.0f, glm::radians(-90.0f), 0.0f)
      );
    } break;
    case EntityPreset::Light: {
      entity.add<LightComponent>();
    } break;
    case EntityPreset::Sun: {
      entity.set<LightComponent>(LightComponent{.type = LightComponent::Directional, .intensity = 10.0f})
        .add<AtmosphereComponent>();
    } break;
    case EntityPreset::Terrain: {
      entity //
        .add<TransformComponent>()
        .set<TerrainComponent>({
          .domain_size = 0.7f,
          .height_frequency = 1.1f,
          .height_amplitude = 3.335f,
          .height_lacunarity = 0.4f,
          .height_octaves = 6,
          .seed = 246,
          .erosion_scale = 0.450f,
          .erosion_strength = 0.220f,
          .ridge_rounding = 0.2f,
          .crease_rounding = -0.2f,
          .erosion_octaves = 16,
        });
    } break;
    case EntityPreset::ProbeVolume   : entity.add<ProbeVolumeComponent>(); break;
    case EntityPreset::AudioSource   : entity.add<AudioSourceComponent>(); break;
    case EntityPreset::AudioListener : entity.add<AudioListenerComponent>(); break;
    case EntityPreset::ParticleSystem: entity.add<ParticleSystemComponent>(); break;
  }
}

static auto collect_missing_colliders(flecs::entity entity, ankerl::svector<flecs::entity, 16>& out) -> bool {
  auto has_mesh = entity.has<MeshComponent>();
  if (has_mesh && !entity.has<MeshColliderComponent>()) {
    out.push_back(entity);
  }

  entity.children([&out, &has_mesh](flecs::entity child) {
    has_mesh = collect_missing_colliders(child, out) || has_mesh;
  });

  return has_mesh;
}

auto entity_name_error(const Scene& scene, const flecs::entity entity, const std::string_view name) -> std::string {
  if (name.empty()) {
    return "An entity name can't be empty";
  }

  if (name.contains('/')) {
    return fmt::format("Entity name '{}' can't contain '/'", name);
  }

  memory::ScopedStack stack;
  const auto* name_cstr = stack.null_terminate_cstr(name);
  const auto parent = entity.parent();
  const auto taken_by = parent.id() != 0 ? parent.lookup(name_cstr).id() : scene.world.lookup(name_cstr).id();
  if (taken_by != 0 && taken_by != entity.id()) {
    return fmt::format("The name '{}' is already used by a sibling", name);
  }

  return {};
}

auto EditTransaction::record(this EditTransaction& self, std::unique_ptr<Command> command) -> void {
  if (self.is_recording && self.group != nullptr) {
    self.group->add_command(std::move(command));
  }
}

auto EditTransaction::commit(this EditTransaction& self) -> void {
  if (self.group == nullptr) {
    return;
  }

  if (self.is_recording && !self.group->empty()) {
    self.editor.undo_redo_system->record(std::move(self.group));
  }

  self.group = nullptr;
}

auto EditTransaction::rollback(this EditTransaction& self) -> bool {
  if (!self.is_recording || self.group == nullptr) {
    return false;
  }

  self.group->undo();
  self.group = nullptr;

  return true;
}

auto begin_edit(Editor& editor, Scene& scene, std::string label) -> EditTransaction {
  return EditTransaction{
    .editor = editor,
    .scene = scene,
    .group = std::make_unique<CommandGroup>(std::move(label)),
    .is_recording = !scene.is_running(),
  };
}

auto create_entity(EditTransaction& tx, std::string_view name, flecs::entity parent, EntityPreset preset)
  -> std::expected<flecs::entity, std::string> {
  ZoneScoped;

  if (name.contains('/')) {
    return std::unexpected(fmt::format("Entity name '{}' can't contain '/'", name));
  }

  if (parent.id() != 0 && !parent.is_alive()) {
    return std::unexpected("The parent entity no longer exists");
  }

  auto& scene = tx.scene;

  // named once it sits in its final scope, `world.entity(name)` would resolve at the root instead
  auto entity = scene.create_entity();
  if (parent.id() != 0) {
    entity.child_of(parent);
  }

  const auto base_name = name.empty() ? default_name(preset) : name;
  entity.set_name(scene.safe_entity_name(std::string(base_name), parent).c_str());

  apply_preset(entity, preset);
  entity.modified<TransformComponent>();

  tx.record(
    std::make_unique<EntityCreateCommand>(&scene, remap_of(tx), entity, fmt::format("create {}", entity_label(entity)))
  );

  return entity;
}

auto create_entity_from_asset(EditTransaction& tx, const UUID& asset, flecs::entity parent)
  -> std::expected<flecs::entity, std::string> {
  ZoneScoped;

  if (parent.id() != 0 && !parent.is_alive()) {
    return std::unexpected("The parent entity no longer exists");
  }

  auto type = AssetType::None;
  {
    auto& asset_man = App::mod<AssetManager>();
    const auto guard = asset_man.get_asset(asset);
    if (!guard) {
      return std::unexpected(fmt::format("Asset {} is not registered", asset.str()));
    }

    type = guard->type;
  }

  auto& scene = tx.scene;
  auto entity = flecs::entity::null();
  if (type == AssetType::Model) {
    entity = scene.create_model_entity(asset, parent);
  } else if (type == AssetType::ParticleSystem) {
    entity = scene.create_particle_system_entity(asset);
    if (entity.id() != 0 && parent.id() != 0) {
      const auto unique_name = scene.safe_entity_name(entity.name().c_str(), parent);
      if (unique_name != entity.name().c_str()) {
        entity.set_name(unique_name.c_str());
      }

      entity.child_of(parent);
      entity.modified<TransformComponent>();
    }
  } else {
    return std::unexpected(
      fmt::format(
        "Can't create an entity from a {} asset, only models and particle systems",
        AssetManager::to_asset_type_sv(type)
      )
    );
  }

  if (entity.id() == 0) {
    return std::unexpected(fmt::format("Failed to create an entity from asset {}", asset.str()));
  }

  tx.record(
    std::make_unique<EntityCreateCommand>(&scene, remap_of(tx), entity, fmt::format("create {}", entity_label(entity)))
  );

  return entity;
}

auto delete_entity(EditTransaction& tx, flecs::entity entity) -> std::expected<void, std::string> {
  ZoneScoped;

  if (!is_live(entity)) {
    return std::unexpected("The entity no longer exists");
  }

  auto command = std::make_unique<EntityDeleteCommand>(
    &tx.scene,
    remap_of(tx),
    entity,
    fmt::format("delete entity {}", entity_label(entity))
  );
  command->execute();
  tx.record(std::move(command));

  return {};
}

auto duplicate_entity(EditTransaction& tx, flecs::entity entity) -> std::expected<flecs::entity, std::string> {
  ZoneScoped;

  if (!is_live(entity)) {
    return std::unexpected("The entity no longer exists");
  }

  const auto clone = tx.scene.duplicate_entity(entity);
  if (clone.id() == 0) {
    return std::unexpected(fmt::format("Failed to duplicate '{}'", entity_label(entity)));
  }

  tx.record(
    std::make_unique<EntityCreateCommand>(
      &tx.scene,
      remap_of(tx),
      clone,
      fmt::format("duplicate entity {}", entity_label(entity))
    )
  );

  return clone;
}

auto reparent_entity(EditTransaction& tx, flecs::entity entity, flecs::entity parent)
  -> std::expected<void, std::string> {
  ZoneScoped;

  if (!is_live(entity)) {
    return std::unexpected("The entity no longer exists");
  }

  if (parent.id() != 0 && !parent.is_alive()) {
    return std::unexpected("The new parent no longer exists");
  }

  for (auto ancestor = parent; ancestor.id() != 0; ancestor = ancestor.parent()) {
    if (ancestor.id() == entity.id()) {
      return std::unexpected(
        fmt::format("Can't parent '{}' to itself or to one of its own children", entity_label(entity))
      );
    }
  }

  const auto old_parent = entity.parent();
  if (old_parent.id() == parent.id()) {
    return {};
  }

  auto command = std::make_unique<EntityReparentCommand>(
    &tx.scene,
    remap_of(tx),
    entity,
    old_parent.id(),
    parent.id(),
    fmt::format("reparent entity {}", entity_label(entity))
  );
  command->execute();
  tx.record(std::move(command));

  return {};
}

auto rename_entity(EditTransaction& tx, flecs::entity entity, std::string_view name)
  -> std::expected<void, std::string> {
  ZoneScoped;

  if (!is_live(entity)) {
    return std::unexpected("The entity no longer exists");
  }

  const auto old_name = std::string(entity.name().c_str());
  if (old_name == name) {
    return {};
  }

  if (auto problem = entity_name_error(tx.scene, entity, name); !problem.empty()) {
    return std::unexpected(std::move(problem));
  }

  const auto new_name = std::string(name);

  auto command = std::make_unique<EntityRenameCommand>(
    &tx.scene,
    remap_of(tx),
    entity,
    old_name,
    new_name,
    fmt::format("rename entity {}", old_name)
  );
  command->execute();
  tx.record(std::move(command));

  return {};
}

auto set_entity_enabled(EditTransaction& tx, flecs::entity entity, bool is_enabled)
  -> std::expected<void, std::string> {
  ZoneScoped;

  if (!is_live(entity)) {
    return std::unexpected("The entity no longer exists");
  }

  const auto was_enabled = entity.enabled();
  if (was_enabled == is_enabled) {
    return {};
  }

  auto command = std::make_unique<EntityEnableCommand>(
    &tx.scene,
    remap_of(tx),
    entity,
    was_enabled,
    is_enabled,
    fmt::format("{} entity {}", is_enabled ? "enable" : "disable", entity_label(entity))
  );
  command->execute();
  tx.record(std::move(command));

  return {};
}

// the change is already applied, this only records it
static auto record_component(
  EditTransaction& tx, flecs::entity entity, flecs::id component, ComponentSnapshot before, std::string label
) -> void {
  tx.record(
    std::make_unique<ComponentSnapshotCommand>(
      &tx.scene,
      remap_of(tx),
      entity,
      component.raw_id(),
      std::move(before),
      ComponentSnapshot::of(entity, component.raw_id()),
      std::move(label)
    )
  );
}

auto add_component(EditTransaction& tx, flecs::entity entity, flecs::id component) -> std::expected<void, std::string> {
  ZoneScoped;

  if (!is_live(entity)) {
    return std::unexpected("The entity no longer exists");
  }

  if (entity.has(component)) {
    return std::unexpected(fmt::format("'{}' already has {}", entity_label(entity), component_label(component)));
  }

  entity.add(component);
  record_component(tx, entity, component, {}, fmt::format("add {}", component_label(component)));

  return {};
}

auto remove_component(EditTransaction& tx, flecs::entity entity, flecs::id component)
  -> std::expected<void, std::string> {
  ZoneScoped;

  if (!is_live(entity)) {
    return std::unexpected("The entity no longer exists");
  }

  if (component.raw_id() == entity.world().id<TransformComponent>()) {
    return std::unexpected("Can't remove TransformComponent!");
  }

  if (!entity.has(component)) {
    return std::unexpected(fmt::format("'{}' has no {}", entity_label(entity), component_label(component)));
  }

  auto before = ComponentSnapshot::of(entity, component.raw_id());
  entity.remove(component);
  record_component(tx, entity, component, std::move(before), fmt::format("remove {}", component_label(component)));

  return {};
}

auto reset_component(EditTransaction& tx, flecs::entity entity, flecs::id component)
  -> std::expected<void, std::string> {
  ZoneScoped;

  if (!is_live(entity)) {
    return std::unexpected("The entity no longer exists");
  }

  if (!entity.has(component)) {
    return std::unexpected(fmt::format("'{}' has no {}", entity_label(entity), component_label(component)));
  }

  // two steps so undo removes the fresh component before restoring the old one, writing the old bytes over it
  // would hand back runtime handles (physics bodies) the remove already destroyed
  const auto label = fmt::format("reset {}", component_label(component));
  auto before = ComponentSnapshot::of(entity, component.raw_id());
  entity.remove(component);
  record_component(tx, entity, component, std::move(before), label);
  entity.add(component);
  record_component(tx, entity, component, {}, label);

  return {};
}

auto paste_component(EditTransaction& tx, flecs::entity source, flecs::entity destination, flecs::id component)
  -> std::expected<void, std::string> {
  ZoneScoped;

  if (!is_live(source) || !is_live(destination)) {
    return std::unexpected("The entity no longer exists");
  }

  if (!source.has(component)) {
    return std::unexpected(fmt::format("'{}' has no {}", entity_label(source), component_label(component)));
  }

  auto before = ComponentSnapshot::of(destination, component.raw_id());
  if (!destination.has(component)) {
    destination.add(component);
  }

  write_component_bytes(destination, component.raw_id(), ComponentBytes::copy_of(source, component.raw_id()));
  record_component(tx, destination, component, std::move(before), fmt::format("paste {}", component_label(component)));

  return {};
}

auto add_mesh_colliders(EditTransaction& tx, flecs::entity entity) -> std::expected<void, std::string> {
  ZoneScoped;

  if (!is_live(entity)) {
    return std::unexpected("The entity no longer exists");
  }

  // gathered first, adding components moves entities out from under a children() walk
  auto missing = ankerl::svector<flecs::entity, 16>{};
  if (!collect_missing_colliders(entity, missing)) {
    return std::unexpected(fmt::format("'{}' has no meshes", entity_label(entity)));
  }

  auto& world = tx.scene.world;
  const auto collider_id = flecs::id(world, world.id<MeshColliderComponent>());
  for (auto mesh_entity : missing) {
    mesh_entity.add<MeshColliderComponent>();
    record_component(tx, mesh_entity, collider_id, {}, "add mesh collider");
  }

  if (!entity.has<RigidBodyComponent>()) {
    entity.set<RigidBodyComponent>({.type = RigidBodyComponent::Static});
    record_component(tx, entity, flecs::id(world, world.id<RigidBodyComponent>()), {}, "add rigid body");
  }

  return {};
}

auto apply_component_snapshot(
  EditTransaction& tx, flecs::entity entity, flecs::id component, ComponentBytes before, ComponentBytes after
) -> std::expected<void, std::string> {
  ZoneScoped;

  if (!is_live(entity)) {
    return std::unexpected("The entity no longer exists");
  }

  if (!entity.has(component)) {
    return std::unexpected(fmt::format("'{}' has no {}", entity_label(entity), component_label(component)));
  }

  if (before.is_empty() || after.is_empty() || before.size() != after.size()) {
    return std::unexpected(fmt::format("{} has no data to write", component_label(component)));
  }

  write_component_bytes(entity, component.raw_id(), after);
  record_component(
    tx,
    entity,
    component,
    {.is_present = true, .bytes = std::move(before)},
    fmt::format("edit {}", component_label(component))
  );

  return {};
}
} // namespace ox
