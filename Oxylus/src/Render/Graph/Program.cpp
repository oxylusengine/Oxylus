#include "Render/Graph/Program.hpp"

#include "Render/Graph/Types.hpp"
#include "Utils/Log.hpp"

namespace ox::vir {
auto make_callback(std::function<auto(Recorder&)->void> body) -> PassCallback {
  return std::make_shared<const std::function<auto(Recorder&)->void>>(std::move(body));
}

auto kind_of(const Value& value) -> option<ir::VariableKind> {
  using Tag = ir::VariableKindTag;
  return std::visit(
    [](const auto& held) -> option<ir::VariableKind> {
      using T = std::decay_t<decltype(held)>;
      if constexpr (std::is_same_v<T, bool>) {
        return ir::VariableKind{.tag = Tag::Bool};
      } else if constexpr (std::is_same_v<T, i32>) {
        return ir::VariableKind{.tag = Tag::I32};
      } else if constexpr (std::is_same_v<T, u32>) {
        return ir::VariableKind{.tag = Tag::U32};
      } else if constexpr (std::is_same_v<T, Extent2D>) {
        return ir::VariableKind{.tag = Tag::Extent2D};
      } else if constexpr (std::is_same_v<T, Extent3D>) {
        return ir::VariableKind{.tag = Tag::Extent3D};
      } else if constexpr (std::is_same_v<T, ClearValue>) {
        return ir::VariableKind{.tag = Tag::ClearValue};
      } else if constexpr (std::is_same_v<T, Bytes>) {
        return ir::VariableKind{.tag = Tag::Bytes, .size = static_cast<u32>(held.data.size())};
      } else if constexpr (std::is_same_v<T, AcquiredSwapchain>) {
        return ir::VariableKind{.tag = Tag::Swapchain};
      } else if constexpr (std::is_same_v<T, Buffer>) {
        return ir::VariableKind{.tag = Tag::Buffer};
      } else if constexpr (std::is_same_v<T, ImageAttachment>) {
        return ir::VariableKind{.tag = Tag::ImageAttachment};
      } else if constexpr (std::is_same_v<T, PassCallback>) {
        return ir::VariableKind{.tag = Tag::Callback};
      } else if constexpr (std::is_same_v<T, AccelerationStructureBuilds>) {
        return ir::VariableKind{.tag = Tag::AccelerationStructureBuilds};
      } else if constexpr (std::is_same_v<T, QueryPool>) {
        return ir::VariableKind{.tag = Tag::QueryPool};
      } else {
        return nullopt;
      }
    },
    value
  );
}

Program::Program(
  std::vector<Instr> instrs, std::vector<Variable> vars, ankerl::unordered_dense::map<ValueID, u32> var_slots
)
    : instructions(std::move(instrs)),
      variables(std::move(vars)) {
  auto present = ankerl::unordered_dense::set<ValueID>{};
  for (usize index = 0; index < instructions.size(); index++) {
    const auto& instruction = instructions[index];
    present.insert(instruction.id);
    if (const auto* label = std::get_if<ir::Label>(&instruction.ir)) {
      labels.emplace(label->label, index);
    }
  }

  for (const auto& [variable, slot] : var_slots) {
    if (present.contains(variable)) {
      slots.emplace(variable, slot);
    }
  }

  for (u32 slot = 0; slot < variables.size(); slot++) {
    const auto resource = variables[slot].resource;
    if (is_valid(resource) && present.contains(resource)) {
      bound.emplace(resource, slot);
    }
  }
}

auto Program::bound_variable(this const Program& self, ValueID resource) -> const Variable* {
  const auto it = self.bound.find(resource);
  return it == self.bound.end() ? nullptr : &self.variables[it->second];
}

auto Program::label_index(this const Program& self, LabelID label) -> option<usize> {
  const auto it = self.labels.find(label);
  if (it == self.labels.end()) {
    return nullopt;
  }

  return it->second;
}

auto Program::variable(this const Program& self, u32 slot) -> const Variable* {
  return slot < self.variables.size() ? &self.variables[slot] : nullptr;
}

auto Program::has_variable(this const Program& self, ValueID variable) -> bool { return self.slots.contains(variable); }

static auto check_bound(const Program& program, ValueID resource, const Value& value) -> void {
  const auto* attachment = std::get_if<ImageAttachment>(&value);
  if (!attachment) {
    return;
  }

  for (const auto& instruction : program.instructions) {
    if (instruction.id != resource) {
      continue;
    }

    const auto* construct = std::get_if<ir::ConstructImage>(&instruction.ir);
    if (!construct) {
      return;
    }

    OX_ASSERT(
      attachment->format == construct->format && attachment->samples == construct->samples,
      "image bound to %u does not have the format and sample count it was declared with",
      std::to_underlying(resource)
    );
    return;
  }
}

// a variable the shape never recorded, or whose readers were compiled away, has nothing to set
auto Program::set(this Program& self, ValueID variable, Value value) -> void {
  const auto it = self.slots.find(variable);
  if (it == self.slots.end()) {
    return;
  }

  auto& declared = self.variables[it->second];
  const auto kind = kind_of(value);
  OX_ASSERT(
    kind.has_value() && *kind == declared.kind,
    "variable %s holds %s but was given another kind",
    declared.name.c_str(),
    ir::to_string(declared.kind).c_str()
  );

  if (is_valid(declared.resource)) {
    check_bound(self, declared.resource, value);
  }

  declared.value = std::move(value);
}

auto Program::set_bytes(this Program& self, ValueID variable, std::span<const u8> bytes) -> void {
  const auto it = self.slots.find(variable);
  if (it == self.slots.end()) {
    return;
  }

  auto& declared = self.variables[it->second];
  OX_ASSERT(
    declared.kind.tag == ir::VariableKindTag::Bytes && declared.kind.size == bytes.size(),
    "variable %s holds %s but was given %zu bytes",
    declared.name.c_str(),
    ir::to_string(declared.kind).c_str(),
    bytes.size()
  );

  // most frames rewrite the same block, so the storage it already has is reused
  if (auto* held = std::get_if<Bytes>(&declared.value)) {
    held->data.assign(bytes.begin(), bytes.end());
    return;
  }

  declared.value = Bytes{.data = {bytes.begin(), bytes.end()}};
}

auto Program::set_callback(this Program& self, ValueID variable, std::function<auto(Recorder&)->void> body) -> void {
  self.set(variable, make_callback(std::move(body)));
}
} // namespace ox::vir
