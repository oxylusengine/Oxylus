#include "Actions/EntityActions.hpp"

#include <ankerl/svector.h>
#include <glm/trigonometric.hpp>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Editor.hpp"

namespace ox {
static auto is_live(flecs::entity entity) -> bool { return entity.id() != 0 && entity.is_alive(); }

static auto remap_of(EditTransaction& tx) -> EntityRemap* { return &tx.editor.undo_redo_system->remap; }

static auto entity_label(flecs::entity entity) -> std::string {
  const auto* name = entity.name().c_str();
  return name[0] != '\0' ? std::string(name) : fmt::format("#{}", entity.id());
}

static auto component_label(flecs::id component) -> std::string { return std::string(component.str().c_str()); }

static auto default_name(EntityPreset preset) -> std::string_view {
  switch (preset) {
    case EntityPreset::Empty         : return "entity";
    case EntityPreset::Sprite        : return "sprite";
    case EntityPreset::Camera        : return "camera";
    case EntityPreset::Light         : return "light";
    case EntityPreset::Sun           : return "sun";
    case EntityPreset::Terrain       : return "terrain";
    case EntityPreset::ProbeVolume   : return "probe_volume";
    case EntityPreset::AudioSource   : return "audio_source";
    case EntityPreset::AudioListener : return "audio_listener";
    case EntityPreset::ParticleSystem: return "particle_system";
  }

  return "entity";
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

auto is_entity_name_free(const Scene& scene, flecs::entity entity, std::string_view name) -> bool {
  const auto name_cstr = std::string(name);
  const auto parent = entity.parent();
  const auto taken_by = parent.id() != 0 ? parent.lookup(name_cstr.c_str()).id()
                                         : scene.world.lookup(name_cstr.c_str()).id();

  return taken_by == 0 || taken_by == entity.id();
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

  if (name.empty()) {
    return std::unexpected("An entity name can't be empty");
  }

  if (name.contains('/')) {
    return std::unexpected(fmt::format("Entity name '{}' can't contain '/'", name));
  }

  const auto old_name = std::string(entity.name().c_str());
  if (old_name == name) {
    return {};
  }

  const auto new_name = std::string(name);
  if (!is_entity_name_free(tx.scene, entity, new_name)) {
    return std::unexpected(fmt::format("The name '{}' is already used by a sibling", name));
  }

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

auto add_component(EditTransaction& tx, flecs::entity entity, flecs::id component) -> std::expected<void, std::string> {
  ZoneScoped;

  if (!is_live(entity)) {
    return std::unexpected("The entity no longer exists");
  }

  if (entity.has(component)) {
    return std::unexpected(fmt::format("'{}' already has {}", entity_label(entity), component_label(component)));
  }

  entity.add(component);
  tx.record(
    std::make_unique<ComponentAddCommand>(
      &tx.scene,
      remap_of(tx),
      entity,
      component.raw_id(),
      fmt::format("add {}", component_label(component))
    )
  );

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

  auto command = std::make_unique<ComponentRemoveCommand>(
    &tx.scene,
    remap_of(tx),
    entity,
    component.raw_id(),
    fmt::format("remove {}", component_label(component))
  );
  command->execute();
  tx.record(std::move(command));

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

  auto remove_command = std::make_unique<ComponentRemoveCommand>(
    &tx.scene,
    remap_of(tx),
    entity,
    component.raw_id(),
    fmt::format("reset {}", component_label(component))
  );
  remove_command->execute();
  tx.record(std::move(remove_command));

  entity.add(component);
  tx.record(
    std::make_unique<ComponentAddCommand>(
      &tx.scene,
      remap_of(tx),
      entity,
      component.raw_id(),
      fmt::format("reset {}", component_label(component))
    )
  );

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

  auto copied = ComponentBytes::copy_of(source, component.raw_id());

  if (!destination.has(component)) {
    destination.add(component);
    tx.record(
      std::make_unique<ComponentAddCommand>(
        &tx.scene,
        remap_of(tx),
        destination,
        component.raw_id(),
        fmt::format("paste {}", component_label(component))
      )
    );
  }

  if (copied.is_empty()) {
    return {};
  }

  auto before = ComponentBytes::copy_of(destination, component.raw_id());
  write_component_bytes(destination, component.raw_id(), copied);
  tx.record(
    std::make_unique<ComponentSnapshotCommand>(
      &tx.scene,
      remap_of(tx),
      destination,
      component.raw_id(),
      std::move(before),
      std::move(copied),
      fmt::format("paste {}", component_label(component))
    )
  );

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

  const auto collider_id = entity.world().id<MeshColliderComponent>();
  for (auto mesh_entity : missing) {
    mesh_entity.add<MeshColliderComponent>();
    tx.record(
      std::make_unique<ComponentAddCommand>(&tx.scene, remap_of(tx), mesh_entity, collider_id, "add mesh collider")
    );
  }

  if (!entity.has<RigidBodyComponent>()) {
    entity.set<RigidBodyComponent>({.type = RigidBodyComponent::Static});
    tx.record(
      std::make_unique<
        ComponentAddCommand>(&tx.scene, remap_of(tx), entity, entity.world().id<RigidBodyComponent>(), "add rigid body")
    );
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
  tx.record(
    std::make_unique<ComponentSnapshotCommand>(
      &tx.scene,
      remap_of(tx),
      entity,
      component.raw_id(),
      std::move(before),
      std::move(after),
      fmt::format("edit {}", component_label(component))
    )
  );

  return {};
}
} // namespace ox
