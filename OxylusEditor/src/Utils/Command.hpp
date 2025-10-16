#pragma once

#include <Utils/JsonWriter.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <simdjson.h>
#include <string>
#include <vector>

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
      other.release();
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
      other.release();
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

class EntityDeleteCommand : public Command {
public:
  EntityDeleteCommand(Scene* scene, flecs::entity entity, std::string entity_name, std::string id)
      : scene_(scene),
        entity_(entity),
        entity_name_(entity_name),
        id_(id) {}

  auto serialize_entity(flecs::entity entity) -> void {
    JsonWriter writer{};
    writer.begin_obj();
    writer["entities"].begin_array();
    Scene::entity_to_json(writer, entity);
    writer.end_array();
    writer.end_obj();
    serialized_entity_ = writer.stream.str();
  }

  auto execute() -> void override {
    serialize_entity(entity_);
    entity_.destruct();
  }

  auto undo() -> void override {
    auto content = simdjson::padded_string(serialized_entity_);
    simdjson::ondemand::parser parser;
    auto doc = parser.iterate(content);
    auto entities_array = doc["entities"];
    std::vector<UUID> requested_assets = {};
    for (auto entity_json : entities_array.get_array()) {
      entity_ = Scene::json_to_entity(
        *scene_, //
        flecs::entity::null(),
        entity_json.value_unsafe(),
        requested_assets
      );
    }
  }

  auto get_id() const -> std::string_view override { return id_; }
  auto get_entity() const -> flecs::entity { return entity_; }

  auto can_merge(const Command& other) const -> bool override { return false; }

  auto merge(std::unique_ptr<Command> other) -> std::unique_ptr<Command> override { return nullptr; }

private:
  Scene* scene_;
  flecs::entity entity_;
  std::string serialized_entity_;
  std::string entity_name_;
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

  auto
  execute_lambda(this UndoRedoSystem& self, std::function<void()> execute, std::function<void()> undo, std::string id)
    -> UndoRedoSystem& {
    self.execute_command(std::make_unique<LambdaCommand>(std::move(execute), std::move(undo), std::move(id)));
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
  }

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
