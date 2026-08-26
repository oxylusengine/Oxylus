#include "Asset/ParticleGraph.hpp"

#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <cstring>
#include <utility>

#include "Core/Option.hpp"

namespace ox {
constexpr static ParticleNodeDesc PARTICLE_NODE_DESCS[] = {
  {"Constant", "Input", 0, true, 1},

  {"Position", "Attribute", 0, true, 0},
  {"Velocity", "Attribute", 0, true, 0},
  {"Size", "Attribute", 0, true, 0},
  {"Rotation", "Attribute", 0, true, 0},
  {"Color", "Attribute", 0, true, 0},
  {"Life Remaining", "Attribute", 0, true, 0},
  {"Life Total", "Attribute", 0, true, 0},
  {"Seed", "Attribute", 0, true, 0},
  {"Age", "Attribute", 0, true, 0},
  {"Time", "Attribute", 0, true, 0},
  {"Delta Time", "Attribute", 0, true, 0},

  {"Random", "Input", 2, true, 2},
  {"Curve", "Input", 1, true, 1},
  {"Gradient", "Input", 1, true, 1},
  {"Noise", "Input", 2, true, 2},

  {"Add", "Math", 2, true, 2},
  {"Subtract", "Math", 2, true, 2},
  {"Multiply", "Math", 2, true, 2},
  {"Divide", "Math", 2, true, 2},
  {"Multiply Add", "Math", 3, true, 3},
  {"Minimum", "Math", 2, true, 2},
  {"Maximum", "Math", 2, true, 2},
  {"Clamp", "Math", 3, true, 3},
  {"Lerp", "Math", 3, true, 3},
  {"Absolute", "Math", 1, true, 1},
  {"Floor", "Math", 1, true, 1},
  {"Fract", "Math", 1, true, 1},
  {"Power", "Math", 2, true, 2},
  {"Dot", "Vector", 2, true, 2},
  {"Cross", "Vector", 2, true, 2},
  {"Normalize", "Vector", 1, true, 1},
  {"Length", "Vector", 1, true, 1},
  {"Sine", "Math", 1, true, 1},
  {"Cosine", "Math", 1, true, 1},
  {"Step", "Math", 2, true, 2},
  {"Smoothstep", "Math", 3, true, 3},
  {"Select", "Math", 3, true, 3},

  {"Set Position", "Output", 1, false, 1},
  {"Set Velocity", "Output", 1, false, 1},
  {"Set Size", "Output", 1, false, 1},
  {"Set Rotation", "Output", 1, false, 1},
  {"Set Color", "Output", 1, false, 1},
  {"Set Lifetime", "Output", 1, false, 1},
  {"Add Position", "Output", 1, false, 1},
  {"Add Velocity", "Output", 1, false, 1},
};

static_assert(
  std::size(PARTICLE_NODE_DESCS) == static_cast<usize>(ParticleNodeType::Count),
  "PARTICLE_NODE_DESCS drifted from ParticleNodeType"
);

auto particle_node_desc(const ParticleNodeType type) -> const ParticleNodeDesc& {
  const auto index = std::min(static_cast<usize>(type), std::size(PARTICLE_NODE_DESCS) - 1);
  return PARTICLE_NODE_DESCS[index];
}

auto is_particle_output_node(const ParticleNodeType type) -> bool {
  return type >= ParticleNodeType::SetPosition && type <= ParticleNodeType::AddVelocity;
}

auto ParticleGraph::find_node(this const ParticleGraph& self, const ParticleNodeID id) -> const ParticleNode* {
  const auto it = std::ranges::find(self.nodes, id, &ParticleNode::id);
  return it == self.nodes.end() ? nullptr : &*it;
}

auto ParticleGraph::add_node(this ParticleGraph& self, const ParticleNodeType type, const glm::vec2 canvas_position)
  -> ParticleNodeID {
  auto next = 0_u32;
  for (const auto& node : self.nodes) {
    next = std::max(next, std::to_underlying(node.id) + 1);
  }

  const auto& desc = particle_node_desc(type);
  auto node = ParticleNode{
    .id = static_cast<ParticleNodeID>(next),
    .type = type,
    .canvas_position = canvas_position,
    .index = 0,
  };
  node.params.resize(desc.param_count, glm::vec4(0.0f));

  self.nodes.emplace_back(std::move(node));

  return static_cast<ParticleNodeID>(next);
}

auto ParticleGraph::remove_node(this ParticleGraph& self, const ParticleNodeID id) -> void {
  std::erase_if(self.links, [id](const ParticleLink& link) { return link.from_node == id || link.to_node == id; });
  std::erase_if(self.nodes, [id](const ParticleNode& node) { return node.id == id; });
}

auto ParticleGraph::add_link(
  this ParticleGraph& self, const ParticleNodeID from, const ParticleNodeID to, const u32 to_pin
) -> ParticleLinkID {
  const auto* target = self.find_node(to);
  if (from == to || !target || !self.find_node(from) || to_pin >= particle_node_desc(target->type).input_count) {
    return ParticleLinkID::Invalid;
  }

  std::erase_if(self.links, [to, to_pin](const ParticleLink& link) {
    return link.to_node == to && link.to_pin == to_pin;
  });

  auto next = 0_u32;
  for (const auto& link : self.links) {
    next = std::max(next, std::to_underlying(link.id) + 1);
  }

  self.links.emplace_back(
    ParticleLink{
      .id = static_cast<ParticleLinkID>(next),
      .from_node = from,
      .to_node = to,
      .to_pin = to_pin,
    }
  );

  return static_cast<ParticleLinkID>(next);
}

auto ParticleGraph::remove_link(this ParticleGraph& self, const ParticleLinkID id) -> void {
  std::erase_if(self.links, [id](const ParticleLink& link) { return link.id == id; });
}

struct ParticleValue {
  GPU::ParticleOperandKind kind = GPU::ParticleOperandKind::Constant;
  u32 payload = 0;

  auto encode(this const ParticleValue& self) -> u32 {
    return GPU::ParticleInstruction::encode_operand(self.kind, self.payload);
  }
};

struct ParticleCompiler {
  ParticleProgram program = {};
  ankerl::unordered_dense::map<u32, ParticleValue> node_values = {};
  ankerl::unordered_dense::map<u32, u32> last_use = {};
  std::vector<u32> free_registers = {};

  ParticleCompiler() {
    for (auto reg = GPU::PARTICLE_REGISTER_COUNT; reg-- > GPU::PARTICLE_ATTRIBUTE_REGISTERS;) {
      free_registers.push_back(reg);
    }
  }

  auto constant(this ParticleCompiler& self, const glm::vec4& value) -> ParticleValue {
    for (auto i = 0_u32; i < self.program.constants.size(); i++) {
      if (std::memcmp(&self.program.constants[i], &value, sizeof(glm::vec4)) == 0) {
        return {GPU::ParticleOperandKind::Constant, i};
      }
    }

    self.program.constants.push_back(value);

    return {GPU::ParticleOperandKind::Constant, static_cast<u32>(self.program.constants.size() - 1)};
  }

  auto alloc_register(this ParticleCompiler& self) -> std::expected<u32, std::string> {
    if (self.free_registers.empty()) {
      return std::unexpected("particle graph needs more registers than the VM has");
    }

    const auto reg = self.free_registers.back();
    self.free_registers.pop_back();

    return reg;
  }

  auto free_value(this ParticleCompiler& self, const ParticleValue& value) -> void {
    if (value.kind != GPU::ParticleOperandKind::Register || value.payload < GPU::PARTICLE_ATTRIBUTE_REGISTERS) {
      return;
    }

    self.free_registers.push_back(value.payload);
  }

  auto emit(
    this ParticleCompiler& self,
    GPU::ParticleOpCode op,
    u32 dst_register,
    u32 write_mask,
    const ParticleValue& a = {},
    const ParticleValue& b = {},
    const ParticleValue& c = {}
  ) -> void {
    self.program.instructions.push_back(
      GPU::ParticleInstruction{
        .op_dst = GPU::ParticleInstruction::encode_op(op, dst_register, write_mask),
        .src0 = a.encode(),
        .src1 = b.encode(),
        .src2 = c.encode(),
      }
    );
  }

  static auto immediate(u32 value) -> ParticleValue { return {GPU::ParticleOperandKind::Immediate, value}; }

  static auto swizzle_mask(u32 x, u32 y, u32 z, u32 w) -> ParticleValue {
    return immediate(x | (y << 2u) | (z << 4u) | (w << 6u));
  }
};

struct ParticleWriteTarget {
  u32 reg = 0;
  u32 mask = 0;
  bool broadcast_x = false;
};

auto particle_write_target(const ParticleNodeType type) -> ParticleWriteTarget {
  switch (type) {
    case ParticleNodeType::SetPosition:
    case ParticleNodeType::AddPosition: return {GPU::PARTICLE_REG_POSITION_LIFE, 0b0111, false};
    case ParticleNodeType::SetVelocity:
    case ParticleNodeType::AddVelocity: return {GPU::PARTICLE_REG_VELOCITY_TOTAL, 0b0111, false};
    case ParticleNodeType::SetSize    : return {GPU::PARTICLE_REG_SIZE_ROT_SEED, 0b0011, false};
    case ParticleNodeType::SetRotation: return {GPU::PARTICLE_REG_SIZE_ROT_SEED, 0b0100, true};
    case ParticleNodeType::SetColor   : return {GPU::PARTICLE_REG_COLOR, 0b1111, false};
    case ParticleNodeType::SetLifetime: return {GPU::PARTICLE_REG_POSITION_LIFE, 0b1000, true};
    default                           : return {};
  }
}

auto particle_arithmetic_opcode(const ParticleNodeType type) -> GPU::ParticleOpCode {
  switch (type) {
    case ParticleNodeType::Add        : return GPU::ParticleOpCode::Add;
    case ParticleNodeType::Subtract   : return GPU::ParticleOpCode::Sub;
    case ParticleNodeType::Multiply   : return GPU::ParticleOpCode::Mul;
    case ParticleNodeType::Divide     : return GPU::ParticleOpCode::Div;
    case ParticleNodeType::MultiplyAdd: return GPU::ParticleOpCode::Mad;
    case ParticleNodeType::Minimum    : return GPU::ParticleOpCode::Min;
    case ParticleNodeType::Maximum    : return GPU::ParticleOpCode::Max;
    case ParticleNodeType::Clamp      : return GPU::ParticleOpCode::Clamp;
    case ParticleNodeType::Lerp       : return GPU::ParticleOpCode::Lerp;
    case ParticleNodeType::Absolute   : return GPU::ParticleOpCode::Abs;
    case ParticleNodeType::Floor      : return GPU::ParticleOpCode::Floor;
    case ParticleNodeType::Fract      : return GPU::ParticleOpCode::Frac;
    case ParticleNodeType::Power      : return GPU::ParticleOpCode::Pow;
    case ParticleNodeType::Dot        : return GPU::ParticleOpCode::Dot;
    case ParticleNodeType::Cross      : return GPU::ParticleOpCode::Cross;
    case ParticleNodeType::Normalize  : return GPU::ParticleOpCode::Normalize;
    case ParticleNodeType::Length     : return GPU::ParticleOpCode::Length;
    case ParticleNodeType::Sine       : return GPU::ParticleOpCode::Sin;
    case ParticleNodeType::Cosine     : return GPU::ParticleOpCode::Cos;
    case ParticleNodeType::Step       : return GPU::ParticleOpCode::Step;
    case ParticleNodeType::Smoothstep : return GPU::ParticleOpCode::Smoothstep;
    case ParticleNodeType::Select     : return GPU::ParticleOpCode::Select;
    case ParticleNodeType::Random     : return GPU::ParticleOpCode::Random;
    case ParticleNodeType::Curve      : return GPU::ParticleOpCode::Curve;
    case ParticleNodeType::Gradient   : return GPU::ParticleOpCode::Gradient;
    case ParticleNodeType::Noise      : return GPU::ParticleOpCode::Noise;
    default                           : return GPU::ParticleOpCode::Nop;
  }
}

auto compile_particle_graph(const ParticleGraph& graph, const ParticleProgramKind kind)
  -> std::expected<ParticleProgram, std::string> {
  ZoneScoped;

  auto inputs = ankerl::unordered_dense::map<u32, ankerl::svector<ParticleNodeID, 3>>{};
  for (const auto& node : graph.nodes) {
    auto& pins = inputs[std::to_underlying(node.id)];
    pins.resize(particle_node_desc(node.type).input_count, ParticleNodeID::Invalid);
  }

  for (const auto& link : graph.links) {
    const auto it = inputs.find(std::to_underlying(link.to_node));
    if (it == inputs.end()) {
      return std::unexpected("link references a node that is not in the graph");
    }

    if (link.to_pin >= it->second.size()) {
      return std::unexpected("link references an input pin the target node does not have");
    }

    if (!graph.find_node(link.from_node)) {
      return std::unexpected("link references a source node that is not in the graph");
    }

    it->second[link.to_pin] = link.from_node;
  }

  // only what an output node actually feeds gets compiled
  auto reachable = ankerl::unordered_dense::set<u32>{};
  auto pending = std::vector<ParticleNodeID>{};
  for (const auto& node : graph.nodes) {
    if (is_particle_output_node(node.type)) {
      pending.push_back(node.id);
    }
  }

  while (!pending.empty()) {
    const auto id = pending.back();
    pending.pop_back();
    if (!reachable.emplace(std::to_underlying(id)).second) {
      continue;
    }

    for (const auto source : inputs[std::to_underlying(id)]) {
      if (source != ParticleNodeID::Invalid) {
        pending.push_back(source);
      }
    }
  }

  auto in_degree = ankerl::unordered_dense::map<u32, u32>{};
  auto consumers = ankerl::unordered_dense::map<u32, std::vector<u32>>{};
  for (const auto id : reachable) {
    auto degree = 0_u32;
    for (const auto source : inputs[id]) {
      if (source != ParticleNodeID::Invalid) {
        degree += 1;
        consumers[std::to_underlying(source)].push_back(id);
      }
    }
    in_degree[id] = degree;
  }

  auto order = std::vector<const ParticleNode*>{};
  order.reserve(reachable.size());
  auto ready = std::vector<u32>{};
  for (const auto& node : graph.nodes) {
    const auto id = std::to_underlying(node.id);
    if (reachable.contains(id) && in_degree[id] == 0) {
      ready.push_back(id);
    }
  }

  while (!ready.empty()) {
    const auto id = ready.front();
    ready.erase(ready.begin());
    order.push_back(graph.find_node(static_cast<ParticleNodeID>(id)));

    for (const auto consumer : consumers[id]) {
      if (--in_degree[consumer] == 0) {
        ready.push_back(consumer);
      }
    }
  }

  if (order.size() != reachable.size()) {
    return std::unexpected("particle graph contains a cycle");
  }

  auto compiler = ParticleCompiler();

  auto position_of = ankerl::unordered_dense::map<u32, u32>{};
  for (auto i = 0_u32; i < order.size(); i++) {
    position_of[std::to_underlying(order[i]->id)] = i;
  }
  for (auto i = 0_u32; i < order.size(); i++) {
    for (const auto source : inputs[std::to_underlying(order[i]->id)]) {
      if (source != ParticleNodeID::Invalid) {
        compiler.last_use[std::to_underlying(source)] = i;
      }
    }
  }

  const auto operand_for = [&](const ParticleNode& node, u32 pin) -> ParticleValue {
    const auto source = inputs[std::to_underlying(node.id)][pin];
    if (source != ParticleNodeID::Invalid) {
      return compiler.node_values[std::to_underlying(source)];
    }

    return compiler.constant(pin < node.params.size() ? node.params[pin] : glm::vec4(0.0f));
  };

  for (auto i = 0_u32; i < order.size(); i++) {
    const auto& node = *order[i];
    const auto node_id = std::to_underlying(node.id);

    if (is_particle_output_node(node.type)) {
      const auto target = particle_write_target(node.type);
      const auto value = operand_for(node, 0);

      switch (node.type) {
        case ParticleNodeType::AddPosition:
        case ParticleNodeType::AddVelocity: {
          compiler.emit(
            GPU::ParticleOpCode::Add,
            target.reg,
            target.mask,
            ParticleValue{GPU::ParticleOperandKind::Register, target.reg},
            value
          );
        } break;
        case ParticleNodeType::SetLifetime: {
          compiler.emit(
            GPU::ParticleOpCode::Swizzle,
            target.reg,
            target.mask,
            value,
            ParticleCompiler::swizzle_mask(0, 0, 0, 0)
          );
          compiler.emit(
            GPU::ParticleOpCode::Swizzle,
            GPU::PARTICLE_REG_VELOCITY_TOTAL,
            0b1000,
            value,
            ParticleCompiler::swizzle_mask(0, 0, 0, 0)
          );
        } break;
        default: {
          if (target.broadcast_x) {
            compiler.emit(
              GPU::ParticleOpCode::Swizzle,
              target.reg,
              target.mask,
              value,
              ParticleCompiler::swizzle_mask(0, 0, 0, 0)
            );
          } else {
            compiler.emit(GPU::ParticleOpCode::Mov, target.reg, target.mask, value);
          }
        } break;
      }
    } else if (node.type == ParticleNodeType::Constant) {
      compiler.node_values[node_id] = compiler.constant(node.params.empty() ? glm::vec4(0.0f) : node.params[0]);
    } else {
      auto direct_register = [&]() -> option<u32> {
        switch (node.type) {
          case ParticleNodeType::ReadPosition: return GPU::PARTICLE_REG_POSITION_LIFE;
          case ParticleNodeType::ReadVelocity: return GPU::PARTICLE_REG_VELOCITY_TOTAL;
          case ParticleNodeType::ReadSize    : return GPU::PARTICLE_REG_SIZE_ROT_SEED;
          case ParticleNodeType::ReadColor   : return GPU::PARTICLE_REG_COLOR;
          default                            : return nullopt;
        }
      }();

      if (direct_register) {
        compiler.node_values[node_id] = {GPU::ParticleOperandKind::Register, *direct_register};
      } else {
        auto dst = compiler.alloc_register();
        if (!dst) {
          return std::unexpected(dst.error());
        }

        const auto reg = *dst;
        compiler.node_values[node_id] = {GPU::ParticleOperandKind::Register, reg};

        switch (node.type) {
          case ParticleNodeType::ReadLifeRemaining: {
            compiler.emit(
              GPU::ParticleOpCode::Swizzle,
              reg,
              0b1111,
              ParticleValue{GPU::ParticleOperandKind::Register, GPU::PARTICLE_REG_POSITION_LIFE},
              ParticleCompiler::swizzle_mask(3, 3, 3, 3)
            );
          } break;
          case ParticleNodeType::ReadLifeTotal: {
            compiler.emit(
              GPU::ParticleOpCode::Swizzle,
              reg,
              0b1111,
              ParticleValue{GPU::ParticleOperandKind::Register, GPU::PARTICLE_REG_VELOCITY_TOTAL},
              ParticleCompiler::swizzle_mask(3, 3, 3, 3)
            );
          } break;
          case ParticleNodeType::ReadRotation: {
            compiler.emit(
              GPU::ParticleOpCode::Swizzle,
              reg,
              0b1111,
              ParticleValue{GPU::ParticleOperandKind::Register, GPU::PARTICLE_REG_SIZE_ROT_SEED},
              ParticleCompiler::swizzle_mask(2, 2, 2, 2)
            );
          } break;
          case ParticleNodeType::ReadSeed: {
            compiler.emit(
              GPU::ParticleOpCode::Swizzle,
              reg,
              0b1111,
              ParticleValue{GPU::ParticleOperandKind::Register, GPU::PARTICLE_REG_SIZE_ROT_SEED},
              ParticleCompiler::swizzle_mask(3, 3, 3, 3)
            );
          } break;
          case ParticleNodeType::ReadAge: {
            compiler.emit(GPU::ParticleOpCode::AgeNorm, reg, 0b1111);
          } break;
          case ParticleNodeType::ReadTime: {
            compiler.emit(GPU::ParticleOpCode::Time, reg, 0b1111);
          } break;
          case ParticleNodeType::ReadDeltaTime: {
            compiler.emit(
              GPU::ParticleOpCode::Swizzle,
              reg,
              0b1111,
              ParticleValue{GPU::ParticleOperandKind::Register, GPU::PARTICLE_REG_CONTEXT},
              ParticleCompiler::swizzle_mask(2, 2, 2, 2)
            );
          } break;
          case ParticleNodeType::Curve   :
          case ParticleNodeType::Gradient: {
            compiler.emit(
              particle_arithmetic_opcode(node.type),
              reg,
              0b1111,
              operand_for(node, 0),
              ParticleCompiler::immediate(node.index)
            );
          } break;
          case ParticleNodeType::Random: {
            compiler.emit(
              GPU::ParticleOpCode::Random,
              reg,
              0b1111,
              operand_for(node, 0),
              operand_for(node, 1),
              ParticleCompiler::immediate(node.index)
            );
          } break;
          default: {
            const auto op = particle_arithmetic_opcode(node.type);
            if (op == GPU::ParticleOpCode::Nop) {
              return std::unexpected("particle graph contains a node the compiler does not know");
            }

            const auto input_count = particle_node_desc(node.type).input_count;
            compiler.emit(
              op,
              reg,
              0b1111,
              input_count > 0 ? operand_for(node, 0) : ParticleValue{},
              input_count > 1 ? operand_for(node, 1) : ParticleValue{},
              input_count > 2 ? operand_for(node, 2) : ParticleValue{}
            );
          } break;
        }
      }
    }

    for (const auto source : inputs[node_id]) {
      if (source == ParticleNodeID::Invalid) {
        continue;
      }

      const auto source_id = std::to_underlying(source);
      if (compiler.last_use[source_id] == i) {
        compiler.free_value(compiler.node_values[source_id]);
      }
    }
  }

  return std::move(compiler.program);
}

auto compile_particle_graphs(const ParticleGraph& spawn_graph, const ParticleGraph& update_graph)
  -> std::expected<ParticleCompiledPrograms, std::string> {
  ZoneScoped;

  auto spawn = compile_particle_graph(spawn_graph, ParticleProgramKind::Spawn);
  if (!spawn) {
    return std::unexpected("spawn graph: " + spawn.error());
  }

  auto update = compile_particle_graph(update_graph, ParticleProgramKind::Update);
  if (!update) {
    return std::unexpected("update graph: " + update.error());
  }

  auto result = ParticleCompiledPrograms{
    .instructions = std::move(spawn->instructions),
    .constants = std::move(spawn->constants),
    .spawn_offset = 0,
  };
  result.spawn_count = static_cast<u32>(result.instructions.size());
  result.update_offset = result.spawn_count;

  const auto constant_offset = static_cast<u32>(result.constants.size());
  for (auto instruction : update->instructions) {
    for (auto* src : {&instruction.src0, &instruction.src1, &instruction.src2}) {
      const auto kind = static_cast<GPU::ParticleOperandKind>(*src >> 30u);
      if (kind == GPU::ParticleOperandKind::Constant) {
        *src = GPU::ParticleInstruction::encode_operand(kind, (*src & 0x3FFFFFFFu) + constant_offset);
      }
    }

    result.instructions.push_back(instruction);
  }

  result.constants.insert(result.constants.end(), update->constants.begin(), update->constants.end());
  result.update_count = static_cast<u32>(result.instructions.size()) - result.update_offset;

  return result;
}
} // namespace ox
