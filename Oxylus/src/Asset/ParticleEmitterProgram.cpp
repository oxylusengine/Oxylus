#include "Asset/ParticleEmitterProgram.hpp"

#include <algorithm>
#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include "Asset/ParticleSystem.hpp"

namespace ox {
auto emitter_program_hash(const u32 value) -> u32 {
  auto x = value;
  x ^= x >> 17;
  x *= 0xed5ad4bb_u32;
  x ^= x >> 11;
  x *= 0xac4c1b51_u32;
  x ^= x >> 15;
  x *= 0x31848bab_u32;
  x ^= x >> 14;
  return x;
}

auto emitter_program_random(const u32 value) -> f32 {
  return static_cast<f32>(emitter_program_hash(value) & 0x00FFFFFF_u32) / static_cast<f32>(0x01000000_u32);
}

auto emitter_program_swizzle(const glm::vec4& value, const u32 mask) -> glm::vec4 {
  return {
    value[static_cast<i32>(mask & 3_u32)],
    value[static_cast<i32>((mask >> 2_u32) & 3_u32)],
    value[static_cast<i32>((mask >> 4_u32) & 3_u32)],
    value[static_cast<i32>((mask >> 6_u32) & 3_u32)],
  };
}

auto run_particle_emitter_program(
  const std::span<const GPU::ParticleInstruction> instructions,
  const std::span<const glm::vec4> constants,
  const ParticleEmitterProgramInput& input,
  const std::span<glm::vec4> state
) -> ParticleEmitterProgramOutput {
  ZoneScoped;

  auto output = ParticleEmitterProgramOutput{.spawn = 0.0f, .spawn_rate = input.spawn_rate};
  if (instructions.empty()) {
    return output;
  }

  auto registers = std::array<glm::vec4, GPU::PARTICLE_REGISTER_COUNT>{};
  registers[GPU::PARTICLE_EMITTER_REG_OUTPUT] = glm::vec4(0.0f, input.spawn_rate, 0.0f, 0.0f);
  registers[GPU::PARTICLE_EMITTER_REG_CONTEXT] = glm::vec4(input.time, input.delta_time, input.cycle_time, input.pulse);

  auto rng = emitter_program_hash(input.seed);

  const auto fetch = [&](const u32 operand) -> glm::vec4 {
    const auto kind = static_cast<GPU::ParticleOperandKind>(operand >> 30_u32);
    const auto payload = operand & 0x3FFFFFFF_u32;

    switch (kind) {
      case GPU::ParticleOperandKind::Register: return registers[payload & (GPU::PARTICLE_REGISTER_COUNT - 1_u32)];
      case GPU::ParticleOperandKind::Constant: return payload < constants.size() ? constants[payload] : glm::vec4(0.0f);
      default                                : return glm::vec4(static_cast<f32>(payload));
    }
  };

  const auto sample_atlas_row = [&](const u32 row, const f32 t) -> glm::vec4 {
    const auto clamped = std::clamp(t, 0.0f, 1.0f);
    if (row < input.curves.size()) {
      return glm::vec4(input.curves[row].sample(clamped));
    }

    const auto gradient_row = row - static_cast<u32>(input.curves.size());
    if (gradient_row < input.gradients.size()) {
      return input.gradients[gradient_row].sample(clamped);
    }

    return glm::vec4(1.0f);
  };

  for (const auto& instruction : instructions) {
    const auto opcode = static_cast<GPU::ParticleOpCode>(instruction.op_dst & 0xFF_u32);
    const auto destination = (instruction.op_dst >> 8_u32) & 0xF_u32;
    const auto write_mask = (instruction.op_dst >> 12_u32) & 0xF_u32;

    const auto a = fetch(instruction.src0);
    const auto b = fetch(instruction.src1);
    const auto c = fetch(instruction.src2);

    auto result = glm::vec4(0.0f);
    switch (opcode) {
      case GPU::ParticleOpCode::Mov    : result = a; break;
      case GPU::ParticleOpCode::Swizzle: result = emitter_program_swizzle(a, instruction.src1 & 0x3FFFFFFF_u32); break;
      case GPU::ParticleOpCode::Add    : result = a + b; break;
      case GPU::ParticleOpCode::Sub    : result = a - b; break;
      case GPU::ParticleOpCode::Mul    : result = a * b; break;
      case GPU::ParticleOpCode::Div    : result = a / glm::max(glm::abs(b), glm::vec4(1.0e-6f)) * glm::sign(b); break;
      case GPU::ParticleOpCode::Mad    : result = a * b + c; break;
      case GPU::ParticleOpCode::Min    : result = glm::min(a, b); break;
      case GPU::ParticleOpCode::Max    : result = glm::max(a, b); break;
      case GPU::ParticleOpCode::Clamp  : result = glm::clamp(a, b, c); break;
      case GPU::ParticleOpCode::Lerp   : result = glm::mix(a, b, c); break;
      case GPU::ParticleOpCode::Abs    : result = glm::abs(a); break;
      case GPU::ParticleOpCode::Floor  : result = glm::floor(a); break;
      case GPU::ParticleOpCode::Frac   : result = glm::fract(a); break;
      case GPU::ParticleOpCode::Pow    : result = glm::pow(glm::max(a, glm::vec4(0.0f)), b); break;
      case GPU::ParticleOpCode::Dot    : result = glm::vec4(glm::dot(glm::vec3(a), glm::vec3(b))); break;
      case GPU::ParticleOpCode::Cross  : result = glm::vec4(glm::cross(glm::vec3(a), glm::vec3(b)), 0.0f); break;
      case GPU::ParticleOpCode::Normalize:
        result = glm::vec4(glm::normalize(glm::vec3(a) + glm::vec3(1.0e-9f)), 0.0f);
        break;
      case GPU::ParticleOpCode::Length    : result = glm::vec4(glm::length(glm::vec3(a))); break;
      case GPU::ParticleOpCode::Sin       : result = glm::sin(a); break;
      case GPU::ParticleOpCode::Cos       : result = glm::cos(a); break;
      case GPU::ParticleOpCode::Step      : result = glm::step(a, b); break;
      case GPU::ParticleOpCode::Smoothstep: result = glm::smoothstep(a, b, c); break;
      case GPU::ParticleOpCode::Select    : result = glm::mix(a, b, glm::step(glm::vec4(0.5f), c)); break;
      case GPU::ParticleOpCode::Curve     : result = sample_atlas_row(instruction.src1 & 0x3FFFFFFF_u32, a.x); break;
      case GPU::ParticleOpCode::Gradient:
        result = sample_atlas_row(static_cast<u32>(input.curves.size()) + (instruction.src1 & 0x3FFFFFFF_u32), a.x);
        break;
      case GPU::ParticleOpCode::Random: {
        const auto stream = instruction.src2 & 0x3FFFFFFF_u32;
        const auto base = rng + stream * 0x9E3779B9_u32;
        const auto r = glm::vec4(
          emitter_program_random(base),
          emitter_program_random(base + 1_u32),
          emitter_program_random(base + 2_u32),
          emitter_program_random(base + 3_u32)
        );
        result = glm::mix(a, b, r);
      } break;
      case GPU::ParticleOpCode::Time   : result = glm::vec4(registers[GPU::PARTICLE_EMITTER_REG_CONTEXT].x); break;
      case GPU::ParticleOpCode::AgeNorm: result = glm::vec4(registers[GPU::PARTICLE_EMITTER_REG_CONTEXT].z); break;
      case GPU::ParticleOpCode::Param  : {
        const auto index = instruction.src1 & 0x3FFFFFFF_u32;
        result = index < input.user_params.size() ? input.user_params[index] : glm::vec4(0.0f);
      } break;
      case GPU::ParticleOpCode::LoadState: {
        const auto slot = instruction.src1 & 0x3FFFFFFF_u32;
        result = slot < state.size() ? state[slot] : glm::vec4(0.0f);
      } break;
      case GPU::ParticleOpCode::StoreState: {
        const auto slot = instruction.src1 & 0x3FFFFFFF_u32;
        if (slot < state.size()) {
          state[slot] = a;
        }
      } break;
      default: break;
    }

    auto current = registers[destination & (GPU::PARTICLE_REGISTER_COUNT - 1_u32)];
    if ((write_mask & 1_u32) != 0_u32) {
      current.x = result.x;
    }
    if ((write_mask & 2_u32) != 0_u32) {
      current.y = result.y;
    }
    if ((write_mask & 4_u32) != 0_u32) {
      current.z = result.z;
    }
    if ((write_mask & 8_u32) != 0_u32) {
      current.w = result.w;
    }
    registers[destination & (GPU::PARTICLE_REGISTER_COUNT - 1_u32)] = current;
  }

  const auto& emitted = registers[GPU::PARTICLE_EMITTER_REG_OUTPUT];
  output.spawn = std::max(emitted.x, 0.0f);
  output.spawn_rate = std::max(emitted.y, 0.0f);

  return output;
}
} // namespace ox
