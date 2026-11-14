#include "Utils/Command.hpp"

#include <ankerl/svector.h>
#include <cstring>
#include <new>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Scene/EntitySerializer.hpp"

namespace ox {
constexpr c8 PATH_SEPARATOR = '\n';
constexpr auto UNRESOLVABLE_PATH = std::string_view("\x01");

struct UuidSlot {
  usize offset = 0;
  UUID uuid = {};
};

struct UuidScanner : IEntitySerializer {
  const u8* base = nullptr;
  ankerl::svector<UuidSlot, 4> slots = {};

  UuidScanner(flecs::world& world_, const void* base_)
      : IEntitySerializer(world_),
        base(static_cast<const u8*>(base_)) {}

  auto on_opaque_value(
    std::string_view name, flecs::entity_t field_type, void* field_ptr, flecs::entity_t opaque_type, const void* value
  ) -> void override {
    if (field_type != world.entity<UUID>()) {
      return;
    }

    slots.push_back(
      {.offset = static_cast<usize>(static_cast<const u8*>(field_ptr) - base), .uuid = *static_cast<UUID*>(field_ptr)}
    );
  }
};

static auto is_null_entity(flecs::entity entity) -> bool { return entity.id() == 0; }

static auto collect_uuid_slots(flecs::world& world, flecs::id_t component, const ComponentBytes& bytes)
  -> ankerl::svector<UuidSlot, 4> {
  if (bytes.is_empty()) {
    return {};
  }

  const auto type = flecs::id(world.c_ptr(), component).type_id();
  if (type.id() == 0 || !type.has<flecs::TypeSerializer>()) {
    return {};
  }

  auto scanner = UuidScanner(world, bytes.data());
  scanner.serialize(type, const_cast<void*>(bytes.data()));

  return std::move(scanner.slots);
}

static auto resolve_entity(const Scene* scene, const EntityRemap* remap, flecs::entity_t id, std::string_view command)
  -> flecs::entity {
  const auto entity = remap->resolve(scene->world, id);
  if (is_null_entity(entity)) {
    OX_LOG_WARN("{}: the entity no longer exists, skipping", command);
  }

  return entity;
}

static auto find_in_scope(const flecs::world& world, flecs::entity parent, const c8* name) -> flecs::entity_t {
  return !is_null_entity(parent) ? parent.lookup(name).id() : world.lookup(name).id();
}

static auto collect_nodes(flecs::entity entity, std::string_view path, EntitySnapshot& out) -> void {
  out.ids.push_back(entity.id());
  out.paths.emplace_back(path);

  entity.children([&out, path](flecs::entity child) {
    const auto* child_name = child.name().c_str();
    if (path == UNRESOLVABLE_PATH || child_name[0] == '\0') {
      collect_nodes(child, UNRESOLVABLE_PATH, out);
      return;
    }

    auto child_path = std::string(path);
    if (!child_path.empty()) {
      child_path += PATH_SEPARATOR;
    }

    child_path += child_name;
    collect_nodes(child, child_path, out);
  });
}

static auto reparent_to(Scene& scene, flecs::entity entity, flecs::entity parent, std::string_view name) -> void {
  if (is_null_entity(parent)) {
    entity.remove(flecs::ChildOf, flecs::Wildcard);
  } else {
    entity.child_of(parent);
  }

  if (!name.empty() && name != entity.name().c_str()) {
    entity.set_name(std::string(name).c_str());
  }

  if (entity.has<TransformComponent>()) {
    scene.set_dirty(entity);
  }
}

static auto set_unique_name(Scene& scene, flecs::entity entity, const std::string& name) -> void {
  const auto taken_by = find_in_scope(scene.world, entity.parent(), name.c_str());
  if (taken_by != 0 && taken_by != entity.id()) {
    entity.set_name(scene.safe_entity_name(name, entity.parent()).c_str());
    return;
  }

  entity.set_name(name.c_str());
}

static auto set_enabled(flecs::entity entity, bool is_enabled) -> void {
  if (is_enabled) {
    entity.enable();
  } else {
    entity.disable();
  }
}

auto EntityRemap::record(this EntityRemap& self, flecs::entity_t old_id, flecs::entity_t new_id) -> void {
  if (old_id == new_id) {
    return;
  }

  // commands recorded after an earlier restore hold that restore's id, so the whole chain has to lead here
  auto tail = old_id;
  for (auto hop = 0_sz; hop <= self.map.size(); hop++) {
    const auto it = self.map.find(tail);
    if (it == self.map.end() || it->second == new_id) {
      break;
    }

    tail = it->second;
  }

  self.map[old_id] = new_id;
  if (tail != new_id) {
    self.map[tail] = new_id;
  }
}

auto EntityRemap::resolve(this const EntityRemap& self, const flecs::world& world, flecs::entity_t id)
  -> flecs::entity {
  for (auto hop = 0_sz; hop <= self.map.size(); hop++) {
    if (id == 0) {
      break;
    }

    if (ecs_is_alive(world.c_ptr(), id)) {
      return flecs::entity(world.c_ptr(), id);
    }

    const auto it = self.map.find(id);
    if (it == self.map.end()) {
      break;
    }

    id = it->second;
  }

  return flecs::entity::null();
}

auto snapshot_entity(flecs::entity entity) -> EntitySnapshot {
  ZoneScoped;

  auto snapshot = EntitySnapshot{};

  auto writer = JsonWriter{};
  writer.begin_obj();
  writer["entities"].begin_array();
  Scene::entity_to_json(writer, entity);
  writer.end_array();
  writer.end_obj();
  snapshot.json = writer.stream.str();

  const auto parent = entity.parent();
  snapshot.parent = parent.id();
  snapshot.name = entity.name().c_str();
  collect_nodes(entity, {}, snapshot);

  return snapshot;
}

auto restore_entity(Scene& scene, EntityRemap& remap, const EntitySnapshot& snapshot) -> flecs::entity {
  ZoneScoped;

  auto parent = flecs::entity::null();
  if (snapshot.parent != 0) {
    parent = remap.resolve(scene.world, snapshot.parent);
    if (is_null_entity(parent)) {
      OX_LOG_WARN("The parent of '{}' no longer exists, restoring it at the root", snapshot.name);
    }
  }

  // named so it can't collide with whatever took its name since, which would make flecs hand back that entity
  const auto restored_name = snapshot.name.empty() ? std::string{} : scene.safe_entity_name(snapshot.name, parent);

  const auto content = simdjson::padded_string(snapshot.json);
  auto parser = simdjson::ondemand::parser{};
  auto doc = parser.iterate(content);

  auto restored = flecs::entity::null();
  auto requested_assets = std::vector<UUID>{};
  for (auto entity_json : doc["entities"].get_array()) {
    restored = Scene::json_to_entity(scene, parent, entity_json.value_unsafe(), requested_assets, restored_name);
  }

  // destroying the entity released every ref its components owned, so the restored ones take them back
  scene.load_requested_assets(requested_assets);

  if (is_null_entity(restored)) {
    OX_LOG_ERROR("Failed to restore '{}'", snapshot.name);
    return restored;
  }

  remap.record(snapshot.ids.front(), restored.id());

  auto missing_count = 0_sz;
  for (auto i = 1_sz; i < snapshot.ids.size(); i++) {
    if (snapshot.paths[i] == UNRESOLVABLE_PATH) {
      missing_count++;
      continue;
    }

    auto current = restored;
    auto begin = 0_sz;
    const auto& path = snapshot.paths[i];
    while (!is_null_entity(current) && begin <= path.size()) {
      const auto end = std::min(path.find(PATH_SEPARATOR, begin), path.size());
      current = current.lookup(std::string(path, begin, end - begin).c_str());
      begin = end + 1;
    }

    if (is_null_entity(current)) {
      missing_count++;
      continue;
    }

    remap.record(snapshot.ids[i], current.id());
  }

  if (missing_count != 0) {
    OX_LOG_WARN("{} unnamed entities under '{}' can't be tracked across undo", missing_count, snapshot.name);
  }

  return restored;
}

ComponentBytes::ComponentBytes(ComponentBytes&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)),
      component_(other.component_),
      info_(other.info_) {}

auto ComponentBytes::operator=(ComponentBytes&& other) noexcept -> ComponentBytes& {
  if (this != &other) {
    release();
    data_ = std::exchange(other.data_, nullptr);
    component_ = other.component_;
    info_ = other.info_;
  }

  return *this;
}

ComponentBytes::~ComponentBytes() { release(); }

auto ComponentBytes::release(this ComponentBytes& self) -> void {
  if (self.data_ == nullptr) {
    return;
  }

  if (self.info_.hooks.dtor != nullptr) {
    self.info_.hooks.dtor(self.data_, 1, &self.info_);
  }

  ::operator delete(self.data_, std::align_val_t(self.info_.alignment));
  self.data_ = nullptr;
}

auto ComponentBytes::copy_of(flecs::entity entity, flecs::id_t component) -> ComponentBytes {
  auto world = entity.world();
  return copy_of(world, component, entity.get(component));
}

auto ComponentBytes::copy_of(flecs::world& world, flecs::id_t component, const void* source) -> ComponentBytes {
  auto bytes = ComponentBytes{};

  const auto* info = ecs_get_type_info(world.c_ptr(), component);
  if (info == nullptr || info->size <= 0 || source == nullptr) {
    return bytes;
  }

  if ((info->hooks.flags & ECS_TYPE_HOOK_COPY_CTOR_ILLEGAL) != 0) {
    OX_LOG_WARN("Component '{}' can't be copied", info->name != nullptr ? info->name : "?");
    return bytes;
  }

  bytes.info_ = *info;
  bytes.component_ = component;
  bytes.data_ = ::operator new(static_cast<usize>(info->size), std::align_val_t(info->alignment));
  if (info->hooks.copy_ctor != nullptr) {
    info->hooks.copy_ctor(bytes.data_, source, 1, &bytes.info_);
  } else {
    std::memcpy(bytes.data_, source, static_cast<usize>(info->size));
  }

  return bytes;
}

auto ComponentBytes::clone(this const ComponentBytes& self) -> ComponentBytes {
  auto bytes = ComponentBytes{};
  if (self.data_ == nullptr) {
    return bytes;
  }

  bytes.info_ = self.info_;
  bytes.component_ = self.component_;
  bytes.data_ = ::operator new(static_cast<usize>(self.info_.size), std::align_val_t(self.info_.alignment));
  if (self.info_.hooks.copy_ctor != nullptr) {
    self.info_.hooks.copy_ctor(bytes.data_, self.data_, 1, &bytes.info_);
  } else {
    std::memcpy(bytes.data_, self.data_, static_cast<usize>(self.info_.size));
  }

  return bytes;
}

auto write_component_bytes(flecs::entity entity, flecs::id_t component, const ComponentBytes& next) -> void {
  ZoneScoped;

  if (next.is_empty()) {
    if (!entity.has(component)) {
      entity.add(component);
    }

    return;
  }

  auto world = entity.world();
  auto& asset_man = App::mod<AssetManager>();

  const auto current = ComponentBytes::copy_of(entity, component);
  const auto current_slots = collect_uuid_slots(world, component, current);

  auto written = next.clone();
  const auto written_slots = collect_uuid_slots(world, component, written);

  auto replaced = ankerl::svector<UUID, 4>{};
  for (const auto& slot : written_slots) {
    auto previous = UUID(nullptr);
    for (const auto& current_slot : current_slots) {
      if (current_slot.offset == slot.offset) {
        previous = current_slot.uuid;
        break;
      }
    }

    if (slot.uuid == previous) {
      continue;
    }

    // an asset that no longer exists can't be taken back, so the field keeps what it holds
    if (slot.uuid && !asset_man.load_asset(slot.uuid)) {
      OX_LOG_WARN("Asset {} can't be loaded anymore, leaving the field as it is", slot.uuid.str());
      *reinterpret_cast<UUID*>(static_cast<u8*>(written.mut_data()) + slot.offset) = previous;
      continue;
    }

    if (previous) {
      replaced.push_back(previous);
    }
  }

  ecs_set_id(world.c_ptr(), entity.id(), component, written.size(), written.data());

  for (const auto& uuid : replaced) {
    asset_man.unload_asset(uuid);
  }
}

EntityDeleteCommand::EntityDeleteCommand(Scene* scene, EntityRemap* remap, flecs::entity entity, std::string id)
    : scene_(scene),
      remap_(remap),
      entity_(entity.id()),
      id_(std::move(id)) {}

auto EntityDeleteCommand::execute() -> void {
  ZoneScoped;

  auto entity = resolve_entity(scene_, remap_, entity_, id_);
  if (is_null_entity(entity)) {
    return;
  }

  entity_ = entity.id();
  snapshot_ = snapshot_entity(entity);
  entity.destruct();
}

auto EntityDeleteCommand::undo() -> void {
  ZoneScoped;

  if (snapshot_.json.empty()) {
    return;
  }

  restore_entity(*scene_, *remap_, snapshot_);
}

EntityCreateCommand::EntityCreateCommand(Scene* scene, EntityRemap* remap, flecs::entity entity, std::string id)
    : scene_(scene),
      remap_(remap),
      entity_(entity.id()),
      snapshot_(snapshot_entity(entity)),
      id_(std::move(id)) {}

auto EntityCreateCommand::execute() -> void {
  ZoneScoped;

  restore_entity(*scene_, *remap_, snapshot_);
}

auto EntityCreateCommand::undo() -> void {
  ZoneScoped;

  auto entity = resolve_entity(scene_, remap_, entity_, id_);
  if (is_null_entity(entity)) {
    return;
  }

  entity.destruct();
}

EntityReparentCommand::EntityReparentCommand(
  Scene* scene,
  EntityRemap* remap,
  flecs::entity entity,
  flecs::entity_t old_parent,
  flecs::entity_t new_parent,
  std::string id
)
    : scene_(scene),
      remap_(remap),
      entity_(entity.id()),
      old_parent_(old_parent),
      new_parent_(new_parent),
      id_(std::move(id)) {}

auto EntityReparentCommand::execute() -> void {
  ZoneScoped;

  auto entity = resolve_entity(scene_, remap_, entity_, id_);
  if (is_null_entity(entity)) {
    return;
  }

  auto parent = flecs::entity::null();
  if (new_parent_ != 0) {
    parent = resolve_entity(scene_, remap_, new_parent_, id_);
    if (is_null_entity(parent)) {
      return;
    }
  }

  original_name_ = entity.name().c_str();
  is_renamed_ = false;

  auto final_name = original_name_;
  if (!original_name_.empty()) {
    const auto taken_by = find_in_scope(scene_->world, parent, original_name_.c_str());
    if (taken_by != 0 && taken_by != entity.id()) {
      final_name = scene_->safe_entity_name(original_name_, parent);
      is_renamed_ = true;
    }
  }

  // the unique name may still be taken in the scope the entity is leaving, so it goes nameless for the move
  if (is_renamed_) {
    entity.remove<flecs::Identifier>(flecs::Name);
  }

  reparent_to(*scene_, entity, parent, is_renamed_ ? std::string_view(final_name) : std::string_view());
}

auto EntityReparentCommand::undo() -> void {
  ZoneScoped;

  auto entity = resolve_entity(scene_, remap_, entity_, id_);
  if (is_null_entity(entity)) {
    return;
  }

  auto parent = flecs::entity::null();
  if (old_parent_ != 0) {
    parent = resolve_entity(scene_, remap_, old_parent_, id_);
    if (is_null_entity(parent)) {
      return;
    }
  }

  if (is_renamed_) {
    entity.remove<flecs::Identifier>(flecs::Name);
  }

  const auto restored_name = is_renamed_ ? scene_->safe_entity_name(original_name_, parent) : std::string{};
  reparent_to(*scene_, entity, parent, restored_name);
}

EntityRenameCommand::EntityRenameCommand(
  Scene* scene, EntityRemap* remap, flecs::entity entity, std::string old_name, std::string new_name, std::string id
)
    : scene_(scene),
      remap_(remap),
      entity_(entity.id()),
      old_name_(std::move(old_name)),
      new_name_(std::move(new_name)),
      id_(std::move(id)) {}

auto EntityRenameCommand::execute() -> void {
  ZoneScoped;

  if (auto entity = resolve_entity(scene_, remap_, entity_, id_); !is_null_entity(entity)) {
    set_unique_name(*scene_, entity, new_name_);
  }
}

auto EntityRenameCommand::undo() -> void {
  ZoneScoped;

  if (auto entity = resolve_entity(scene_, remap_, entity_, id_); !is_null_entity(entity)) {
    set_unique_name(*scene_, entity, old_name_);
  }
}

EntityEnableCommand::EntityEnableCommand(
  Scene* scene, EntityRemap* remap, flecs::entity entity, bool old_enabled, bool new_enabled, std::string id
)
    : scene_(scene),
      remap_(remap),
      entity_(entity.id()),
      old_enabled_(old_enabled),
      new_enabled_(new_enabled),
      id_(std::move(id)) {}

auto EntityEnableCommand::execute() -> void {
  ZoneScoped;

  if (auto entity = resolve_entity(scene_, remap_, entity_, id_); !is_null_entity(entity)) {
    set_enabled(entity, new_enabled_);
  }
}

auto EntityEnableCommand::undo() -> void {
  ZoneScoped;

  if (auto entity = resolve_entity(scene_, remap_, entity_, id_); !is_null_entity(entity)) {
    set_enabled(entity, old_enabled_);
  }
}

ComponentSnapshotCommand::ComponentSnapshotCommand(
  Scene* scene,
  EntityRemap* remap,
  flecs::entity entity,
  flecs::id_t component,
  ComponentBytes before,
  ComponentBytes after,
  std::string id
)
    : scene_(scene),
      remap_(remap),
      entity_(entity.id()),
      component_(component),
      before_(std::move(before)),
      after_(std::move(after)),
      id_(std::move(id)) {}

auto ComponentSnapshotCommand::execute() -> void {
  ZoneScoped;

  if (auto entity = resolve_entity(scene_, remap_, entity_, id_); !is_null_entity(entity)) {
    write_component_bytes(entity, component_, after_);
  }
}

auto ComponentSnapshotCommand::undo() -> void {
  ZoneScoped;

  if (auto entity = resolve_entity(scene_, remap_, entity_, id_); !is_null_entity(entity)) {
    write_component_bytes(entity, component_, before_);
  }
}

ComponentAddCommand::ComponentAddCommand(
  Scene* scene, EntityRemap* remap, flecs::entity entity, flecs::id_t component, std::string id
)
    : scene_(scene),
      remap_(remap),
      entity_(entity.id()),
      component_(component),
      bytes_(ComponentBytes::copy_of(entity, component)),
      id_(std::move(id)) {}

auto ComponentAddCommand::execute() -> void {
  ZoneScoped;

  auto entity = resolve_entity(scene_, remap_, entity_, id_);
  if (is_null_entity(entity)) {
    return;
  }

  if (!entity.has(component_)) {
    entity.add(component_);
  }

  write_component_bytes(entity, component_, bytes_);
}

auto ComponentAddCommand::undo() -> void {
  ZoneScoped;

  if (auto entity = resolve_entity(scene_, remap_, entity_, id_); !is_null_entity(entity)) {
    entity.remove(component_);
  }
}

ComponentRemoveCommand::ComponentRemoveCommand(
  Scene* scene, EntityRemap* remap, flecs::entity entity, flecs::id_t component, std::string id
)
    : scene_(scene),
      remap_(remap),
      entity_(entity.id()),
      component_(component),
      bytes_(ComponentBytes::copy_of(entity, component)),
      id_(std::move(id)) {}

auto ComponentRemoveCommand::execute() -> void {
  ZoneScoped;

  if (auto entity = resolve_entity(scene_, remap_, entity_, id_); !is_null_entity(entity)) {
    entity.remove(component_);
  }
}

auto ComponentRemoveCommand::undo() -> void {
  ZoneScoped;

  auto entity = resolve_entity(scene_, remap_, entity_, id_);
  if (is_null_entity(entity)) {
    return;
  }

  if (entity.has(component_)) {
    OX_LOG_WARN("{}: the component is already back, skipping", id_);
    return;
  }

  write_component_bytes(entity, component_, bytes_);
}
} // namespace ox
