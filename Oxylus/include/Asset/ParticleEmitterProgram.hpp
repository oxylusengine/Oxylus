#pragma once

#include <glm/vec4.hpp>
#include <span>

#include "Core/Types.hpp"
#include "Scene/SceneGPU.hpp"

namespace ox {
struct ParticleCurve;
struct ParticleGradient;

struct ParticleEmitterProgramInput {
  f32 time = 0.0f;
  f32 cycle_time = 0.0f;
  f32 delta_time = 0.0f;
  f32 pulse = 0.0f;
  f32 spawn_rate = 0.0f;
  u32 seed = 0;
  std::span<const glm::vec4> user_params = {};
  std::span<const ParticleCurve> curves = {};
  std::span<const ParticleGradient> gradients = {};
};

struct ParticleEmitterProgramOutput {
  f32 spawn = 0.0f;
  f32 spawn_rate = 0.0f;
};

auto run_particle_emitter_program(
  std::span<const GPU::ParticleInstruction> instructions,
  std::span<const glm::vec4> constants,
  const ParticleEmitterProgramInput& input,
  std::span<glm::vec4> state
) -> ParticleEmitterProgramOutput;
} // namespace ox
