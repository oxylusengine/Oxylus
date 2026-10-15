#pragma once

#include <functional>
#include <memory>
#include <string_view>

#include "Render/Graph/IR.hpp"
#include "Render/Graph/Types.hpp"

namespace ox::vir {
using PassCallback = std::shared_ptr<const std::function<auto(Recorder&)->void>>;
using AccelerationStructureBuilds = std::shared_ptr<const std::vector<AccelerationStructureBuild>>;

auto make_callback(std::function<auto(Recorder&)->void> body) -> PassCallback;

struct Reference {
  ValueID id = ValueID::Invalid;
};

struct Bytes {
  std::vector<u8> data = {};
};

using Slice = std::vector<ValueID>;

using Value = std::variant<
  std::monostate,
  bool,
  i32,
  u32,
  u64,
  Extent2D,
  Extent3D,
  std::string_view,
  ImageAttachment,
  Buffer,
  Bytes,
  AcquiredSwapchain,
  Slice,
  Reference,
  Access,
  ClearValue,
  PassCallback,
  AccelerationStructureBuilds,
  QueryPool>;

auto kind_of(const Value& value) -> option<ir::VariableKind>;

struct Variable {
  ir::VariableKind kind = {};
  std::string name = {};
  Value value = {};
  // a slot that fills in a resource construct rather than a value of its own
  ValueID resource = ValueID::Invalid;
};

struct Program {
  // the owning RenderContext stamps programs that present with its swapchain generation
  u64 presentation_generation = 0;
  std::vector<Instr> instructions = {};
  std::vector<Variable> variables = {};
  ankerl::unordered_dense::map<ValueID, u32> slots = {};
  ankerl::unordered_dense::map<LabelID, usize> labels = {};
  // resource construct to the slot that fills it
  ankerl::unordered_dense::map<ValueID, u32> bound = {};

  Program() = default;
  Program(std::vector<Instr> instrs, std::vector<Variable> vars, ankerl::unordered_dense::map<ValueID, u32> var_slots);

  auto is_empty(this const Program& self) -> bool { return self.instructions.empty(); }
  auto bound_variable(this const Program& self, ValueID resource) -> const Variable*;
  auto label_index(this const Program& self, LabelID label) -> option<usize>;
  auto variable(this const Program& self, u32 slot) -> const Variable*;
  auto has_variable(this const Program& self, ValueID variable) -> bool;

  auto set(this Program& self, ValueID variable, Value value) -> void;
  auto set_bytes(this Program& self, ValueID variable, std::span<const u8> bytes) -> void;

  template <typename T>
  auto set_bytes(this Program& self, ValueID variable, const T& value) -> void {
    static_assert(std::is_trivially_copyable_v<T>);
    self.set_bytes(variable, std::span(reinterpret_cast<const u8*>(&value), sizeof(T)));
  }

  auto set_callback(this Program& self, ValueID variable, std::function<auto(Recorder&)->void> body) -> void;

  auto dump(this const Program& self, bool is_highlighted = false) -> std::string;
};
} // namespace ox::vir
