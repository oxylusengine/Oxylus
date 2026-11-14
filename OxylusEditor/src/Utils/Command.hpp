#pragma once

#include <ankerl/unordered_dense.h>
#include <chrono>
#include <functional>
#include <memory>
#include <simdjson.h>
#include <string>
#include <vector>

#include "Core/UUID.hpp"
#include "Scene/Scene.hpp"
#include "Utils/JsonWriter.hpp"
#include "Utils/Log.hpp"

namespace ox {
class Command {
public:
  virtual ~Command() = default;
  virtual auto execute() -> void = 0;
  virtual auto undo() -> void = 0;
  virtual auto get_id() const -> std::string_view = 0;
  virtual auto can_merge(const Command& other) const -> bool { return false; }
  virtual auto merge(std::unique_ptr<Command> other) -> std::unique_ptr<Command> { return nullptr; }
};

class LambdaCommand : public Command {
public:
  LambdaCommand(std::function<void()> execute, std::function<void()> undo, std::string id)
      : execute_func_(std::move(execute)),
        undo_func_(std::move(undo)),
        id_(id) {}

  auto execute() -> void override { execute_func_(); }
  auto undo() -> void override { undo_func_(); }
  auto get_id() const -> std::string_view override { return id_; }

private:
  std::function<void()> execute_func_;
  std::function<void()> undo_func_;
  std::string id_;
};

class CommandGroup : public Command {
public:
  explicit CommandGroup(const std::string id) : id_(id) {}

  template <typename T, typename... Args>
  auto add_command(Args&&... args) -> void {
    commands_.emplace_back(std::make_unique<T>(std::forward<Args>(args)...));
  }

  auto add_command(std::unique_ptr<Command> command) -> void { commands_.emplace_back(std::move(command)); }

  auto execute() -> void override {
    for (auto& cmd : commands_) {
      cmd->execute();
    }
  }

  auto undo() -> void override {
    for (auto it = commands_.rbegin(); it != commands_.rend(); ++it) {
      (*it)->undo();
    }
  }

  auto get_id() const -> std::string_view override { return id_; }

  auto empty() const -> bool { return commands_.empty(); }
  auto size() const -> usize { return commands_.size(); }

private:
  std::vector<std::unique_ptr<Command>> commands_;
  std::string id_;
};

template <typename T>
class PropertyChangeCommand : public Command {
public:
  PropertyChangeCommand(T* target, T old_val, T new_val, std::string id)
      : target_(target),
        old_value_(old_val),
        new_value_(new_val),
        id_(id) {}

  auto execute() -> void override { *target_ = new_value_; }
  auto undo() -> void override { *target_ = old_value_; }

  auto get_id() const -> std::string_view override { return id_; }

  auto can_merge(const Command& other) const -> bool override {
    if (auto* other_cmd = dynamic_cast<const PropertyChangeCommand<T>*>(&other)) {
      return target_ == other_cmd->target_ && get_id() == other_cmd->get_id();
    }
    return false;
  }

  auto merge(std::unique_ptr<Command> other) -> std::unique_ptr<Command> override {
    if (auto other_cmd = dynamic_cast<PropertyChangeCommand<T>*>(other.get())) {
      auto merged = std::make_unique<PropertyChangeCommand<T>>(target_, old_value_, other_cmd->new_value_, id_);
      return merged;
    }
    return nullptr;
  }

private:
  T* target_;
  T old_value_;
  T new_value_;
  std::string id_;
};

// Difference between this and PropertyChangeCommand is, this calls modified on given entity with the given component
template <typename T>
class ComponentChangeCommand : public Command {
public:
  ComponentChangeCommand(flecs::entity e, T* target, T old_val, T new_val, std::string id)
      : entity_(e),
        target_(target),
        old_value_(old_val),
        new_value_(new_val),
        id_(id) {}

  auto execute() -> void override {
    *target_ = new_value_;
    if (entity_.is_valid()) {
      entity_.modified<T>();
    }
  }
  auto undo() -> void override {
    *target_ = old_value_;
    if (entity_.is_valid()) {
      entity_.modified<T>();
    }
  }

  auto get_id() const -> std::string_view override { return id_; }

  auto can_merge(const Command& other) const -> bool override {
    if (auto* other_cmd = dynamic_cast<const ComponentChangeCommand<T>*>(&other)) {
      return target_ == other_cmd->target_ && get_id() == other_cmd->get_id();
    }
    return false;
  }

  auto merge(std::unique_ptr<Command> other) -> std::unique_ptr<Command> override {
    if (auto other_cmd = dynamic_cast<ComponentChangeCommand<T>*>(other.get())) {
      auto
        merged = std::make_unique<ComponentChangeCommand<T>>(entity_, target_, old_value_, other_cmd->new_value_, id_);
      return merged;
    }
    return nullptr;
  }

private:
  flecs::entity entity_;
  T* target_;
  T old_value_;
  T new_value_;
  std::string id_;
};

struct EntityRemap {
  ankerl::unordered_dense::map<flecs::entity_t, flecs::entity_t> map = {};

  auto record(this EntityRemap& self, flecs::entity_t old_id, flecs::entity_t new_id) -> void;
  auto resolve(this const EntityRemap& self, const flecs::world& world, flecs::entity_t id) -> flecs::entity;
};

struct EntitySnapshot {
  std::string json = {};
  // root first, then descendants in children() order
  std::vector<flecs::entity_t> ids = {};
  // names from the root down to each id, parallel to ids, children() order is not stable across a restore
  std::vector<std::string> paths = {};
  flecs::entity_t parent = 0;
  std::string name = {};
};

auto snapshot_entity(flecs::entity entity) -> EntitySnapshot;
auto restore_entity(Scene& scene, EntityRemap& remap, const EntitySnapshot& snapshot) -> flecs::entity;

class ComponentBytes {
public:
  ComponentBytes() = default;
  ComponentBytes(const ComponentBytes&) = delete;
  auto operator=(const ComponentBytes&) -> ComponentBytes& = delete;
  ComponentBytes(ComponentBytes&& other) noexcept;
  auto operator=(ComponentBytes&& other) noexcept -> ComponentBytes&;
  ~ComponentBytes();

  static auto copy_of(flecs::entity entity, flecs::id_t component) -> ComponentBytes;
  static auto copy_of(flecs::world& world, flecs::id_t component, const void* source) -> ComponentBytes;

  auto clone(this const ComponentBytes& self) -> ComponentBytes;

  auto is_empty(this const ComponentBytes& self) -> bool { return self.data_ == nullptr; }
  auto data(this const ComponentBytes& self) -> const void* { return self.data_; }
  auto mut_data(this ComponentBytes& self) -> void* { return self.data_; }
  auto size(this const ComponentBytes& self) -> usize { return self.data_ != nullptr ? self.info_.size : 0; }
  auto component(this const ComponentBytes& self) -> flecs::id_t { return self.component_; }

private:
  void* data_ = nullptr;
  flecs::id_t component_ = 0;
  // a copy, the pointer flecs hands out dies with the world and the hooks have to run after it
  ecs_type_info_t info_ = {};

  auto release(this ComponentBytes& self) -> void;
};

// writes `next` over the live component, taking a ref for every uuid it newly names and giving back the ones it
// replaces
auto write_component_bytes(flecs::entity entity, flecs::id_t component, const ComponentBytes& next) -> void;

class EntityDeleteCommand : public Command {
public:
  EntityDeleteCommand(Scene* scene, EntityRemap* remap, flecs::entity entity, std::string id);

  auto execute() -> void override;
  auto undo() -> void override;
  auto get_id() const -> std::string_view override { return id_; }

private:
  Scene* scene_;
  EntityRemap* remap_;
  flecs::entity_t entity_;
  EntitySnapshot snapshot_ = {};
  std::string id_;
};

class EntityCreateCommand : public Command {
public:
  EntityCreateCommand(Scene* scene, EntityRemap* remap, flecs::entity entity, std::string id);

  auto execute() -> void override;
  auto undo() -> void override;
  auto get_id() const -> std::string_view override { return id_; }

private:
  Scene* scene_;
  EntityRemap* remap_;
  flecs::entity_t entity_;
  EntitySnapshot snapshot_ = {};
  std::string id_;
};

class EntityReparentCommand : public Command {
public:
  EntityReparentCommand(
    Scene* scene,
    EntityRemap* remap,
    flecs::entity entity,
    flecs::entity_t old_parent,
    flecs::entity_t new_parent,
    std::string id
  );

  auto execute() -> void override;
  auto undo() -> void override;
  auto get_id() const -> std::string_view override { return id_; }

private:
  Scene* scene_;
  EntityRemap* remap_;
  flecs::entity_t entity_;
  flecs::entity_t old_parent_;
  flecs::entity_t new_parent_;
  std::string original_name_ = {};
  bool is_renamed_ = false;
  std::string id_;
};

class EntityRenameCommand : public Command {
public:
  EntityRenameCommand(
    Scene* scene, EntityRemap* remap, flecs::entity entity, std::string old_name, std::string new_name, std::string id
  );

  auto execute() -> void override;
  auto undo() -> void override;
  auto get_id() const -> std::string_view override { return id_; }

private:
  Scene* scene_;
  EntityRemap* remap_;
  flecs::entity_t entity_;
  std::string old_name_;
  std::string new_name_;
  std::string id_;
};

class EntityEnableCommand : public Command {
public:
  EntityEnableCommand(
    Scene* scene, EntityRemap* remap, flecs::entity entity, bool old_enabled, bool new_enabled, std::string id
  );

  auto execute() -> void override;
  auto undo() -> void override;
  auto get_id() const -> std::string_view override { return id_; }

private:
  Scene* scene_;
  EntityRemap* remap_;
  flecs::entity_t entity_;
  bool old_enabled_;
  bool new_enabled_;
  std::string id_;
};

class ComponentSnapshotCommand : public Command {
public:
  ComponentSnapshotCommand(
    Scene* scene,
    EntityRemap* remap,
    flecs::entity entity,
    flecs::id_t component,
    ComponentBytes before,
    ComponentBytes after,
    std::string id
  );

  auto execute() -> void override;
  auto undo() -> void override;
  auto get_id() const -> std::string_view override { return id_; }

private:
  Scene* scene_;
  EntityRemap* remap_;
  flecs::entity_t entity_;
  flecs::id_t component_;
  ComponentBytes before_;
  ComponentBytes after_;
  std::string id_;
};

class ComponentAddCommand : public Command {
public:
  ComponentAddCommand(Scene* scene, EntityRemap* remap, flecs::entity entity, flecs::id_t component, std::string id);

  auto execute() -> void override;
  auto undo() -> void override;
  auto get_id() const -> std::string_view override { return id_; }

private:
  Scene* scene_;
  EntityRemap* remap_;
  flecs::entity_t entity_;
  flecs::id_t component_;
  ComponentBytes bytes_;
  std::string id_;
};

class ComponentRemoveCommand : public Command {
public:
  ComponentRemoveCommand(Scene* scene, EntityRemap* remap, flecs::entity entity, flecs::id_t component, std::string id);

  auto execute() -> void override;
  auto undo() -> void override;
  auto get_id() const -> std::string_view override { return id_; }

private:
  Scene* scene_;
  EntityRemap* remap_;
  flecs::entity_t entity_;
  flecs::id_t component_;
  ComponentBytes bytes_;
  std::string id_;
};

class UndoRedoSystem {
public:
  explicit UndoRedoSystem(
    size_t max_history = 100,
    bool merge_enabled = true,
    std::chrono::milliseconds merge_timeout = std::chrono::milliseconds(500)
  )
      : max_history_size_(max_history),
        merge_enabled_(merge_enabled),
        merge_timeout_(merge_timeout),
        last_command_time_(std::chrono::steady_clock::now()) {}

  template <typename T, typename... Args>
  auto execute_command(this UndoRedoSystem& self, Args&&... args) -> UndoRedoSystem& {
    auto command = std::make_unique<T>(std::forward<Args>(args)...);
    self.execute_command(std::move(command));
    return self;
  }

  auto execute_command(this UndoRedoSystem& self, std::unique_ptr<Command> command) -> UndoRedoSystem& {
    if (self.merge_enabled_ && !self.undo_stack_.empty()) {
      auto now = std::chrono::steady_clock::now();
      if (now - self.last_command_time_ < self.merge_timeout_ && self.undo_stack_.back()->can_merge(*command)) {
        if (auto merged = self.undo_stack_.back()->merge(std::move(command))) {
          self.undo_stack_.back() = std::move(merged);
          self.last_command_time_ = now;
          return self;
        }
      }
    }

    OX_CHECK_NULL(command);

    command->execute();

    self.undo_stack_.emplace_back(std::move(command));

    self.redo_stack_.clear();

    if (self.undo_stack_.size() > self.max_history_size_) {
      self.undo_stack_.erase(self.undo_stack_.begin());
    }

    self.last_command_time_ = std::chrono::steady_clock::now();

    return self;
  }

  auto execute_lambda(
    this UndoRedoSystem& self, std::function<void()> execute, std::function<void()> undo, std::string id
  ) -> UndoRedoSystem& {
    self.execute_command(std::make_unique<LambdaCommand>(std::move(execute), std::move(undo), std::move(id)));
    return self;
  }

  auto record(this UndoRedoSystem& self, std::unique_ptr<Command> command) -> UndoRedoSystem& {
    OX_CHECK_NULL(command);

    self.undo_stack_.emplace_back(std::move(command));
    self.redo_stack_.clear();

    if (self.undo_stack_.size() > self.max_history_size_) {
      self.undo_stack_.erase(self.undo_stack_.begin());
    }

    self.last_command_time_ = std::chrono::steady_clock::now();

    return self;
  }

  auto begin_group(this UndoRedoSystem&, std::string id) -> std::unique_ptr<CommandGroup> {
    return std::make_unique<CommandGroup>(std::move(id));
  }

  auto execute_group(this UndoRedoSystem& self, std::unique_ptr<CommandGroup> group) -> UndoRedoSystem& {
    if (!group->empty()) {
      self.execute_command(std::move(group));
    }
    return self;
  }

  auto undo(this UndoRedoSystem& self) -> bool {
    if (self.undo_stack_.empty())
      return false;

    auto command = std::move(self.undo_stack_.back());
    self.undo_stack_.pop_back();

    command->undo();
    OX_LOG_INFO("Undo: {}", command->get_id());
    self.redo_stack_.emplace_back(std::move(command));

    return true;
  }

  auto redo(this UndoRedoSystem& self) -> bool {
    if (self.redo_stack_.empty())
      return false;

    auto command = std::move(self.redo_stack_.back());
    self.redo_stack_.pop_back();

    command->execute();
    OX_LOG_INFO("Redo: {}", command->get_id());
    self.undo_stack_.emplace_back(std::move(command));

    return true;
  }

  auto can_undo(this const UndoRedoSystem& self) -> bool { return !self.undo_stack_.empty(); }
  auto can_redo(this const UndoRedoSystem& self) -> bool { return !self.redo_stack_.empty(); }

  auto get_undo_stack(this const UndoRedoSystem& self) -> const std::vector<std::unique_ptr<Command>>& {
    return self.undo_stack_;
  }

  auto get_redo_stack(this const UndoRedoSystem& self) -> const std::vector<std::unique_ptr<Command>>& {
    return self.redo_stack_;
  }

  auto get_undo_count(this const UndoRedoSystem& self) -> usize { return self.undo_stack_.size(); }
  auto get_redo_count(this const UndoRedoSystem& self) -> usize { return self.redo_stack_.size(); }

  auto clear(this UndoRedoSystem& self) -> void {
    self.undo_stack_.clear();
    self.redo_stack_.clear();
    self.remap.map.clear();
  }

  EntityRemap remap = {};

  auto get_merge_timeout(this const UndoRedoSystem& self) -> std::chrono::milliseconds { return self.merge_timeout_; }
  auto set_merge_enabled(this UndoRedoSystem& self, bool enabled) -> UndoRedoSystem& {
    self.merge_enabled_ = enabled;
    return self;
  }
  auto set_merge_timeout(this UndoRedoSystem& self, std::chrono::milliseconds timeout) -> UndoRedoSystem& {
    self.merge_timeout_ = timeout;
    return self;
  }
  auto get_max_history_size(this const UndoRedoSystem& self) -> usize { return self.max_history_size_; }
  auto set_max_history_size(this UndoRedoSystem& self, usize size) -> UndoRedoSystem& {
    self.max_history_size_ = size;
    while (self.undo_stack_.size() > self.max_history_size_) {
      self.undo_stack_.erase(self.undo_stack_.begin());
    }
    return self;
  }

private:
  std::vector<std::unique_ptr<Command>> undo_stack_;
  std::vector<std::unique_ptr<Command>> redo_stack_;
  usize max_history_size_;
  bool merge_enabled_;
  std::chrono::milliseconds merge_timeout_;
  std::chrono::steady_clock::time_point last_command_time_;
};
} // namespace ox
