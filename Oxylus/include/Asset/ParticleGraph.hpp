#pragma once

#include <ankerl/svector.h>
#include <expected>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <string>
#include <utility>
#include <vector>

#include "Core/Types.hpp"
#include "Scene/SceneGPU.hpp"

namespace ox {
enum class ParticleNodeID : u32 { Invalid = ~0_u32 };
enum class ParticleLinkID : u32 { Invalid = ~0_u32 };

enum class ParticleNodeType : u32 {
  Constant = 0,

  ReadPosition,
  ReadVelocity,
  ReadSize,
  ReadRotation,
  ReadColor,
  ReadLifeRemaining,
  ReadLifeTotal,
  ReadSeed,
  ReadAge,
  ReadTime,
  ReadDeltaTime,

  Random,
  Curve,
  Gradient,
  Noise,

  Add,
  Subtract,
  Multiply,
  Divide,
  MultiplyAdd,
  Minimum,
  Maximum,
  Clamp,
  Lerp,
  Absolute,
  Floor,
  Fract,
  Power,
  Dot,
  Cross,
  Normalize,
  Length,
  Sine,
  Cosine,
  Step,
  Smoothstep,
  Select,

  SetPosition,
  SetVelocity,
  SetSize,
  SetRotation,
  SetColor,
  SetLifetime,
  AddPosition,
  AddVelocity,

  ReadParameter,

  ReadCycleTime,
  ReadPulse,
  Interval,
  Once,
  OnRising,
  SpawnParticles,
  SetSpawnRate,

  Count,
};

enum class ParticleProgramKind : u32 {
  Spawn = 0,
  Update = 1,
  // Runs once per emitter per frame, on the CPU, and decides how many particles are born.
  Emitter = 2,
};

enum class ParticleKindMask : u32 {
  None = 0,
  Spawn = 1 << 0,
  Update = 1 << 1,
  Emitter = 1 << 2,
  Particle = Spawn | Update,
  All = Spawn | Update | Emitter,
};

constexpr auto operator|(ParticleKindMask a, ParticleKindMask b) -> ParticleKindMask {
  return static_cast<ParticleKindMask>(std::to_underlying(a) | std::to_underlying(b));
}

constexpr auto particle_kind_bit(ParticleProgramKind kind) -> ParticleKindMask {
  return static_cast<ParticleKindMask>(1u << std::to_underlying(kind));
}

constexpr auto particle_kind_allows(ParticleKindMask mask, ParticleProgramKind kind) -> bool {
  return (std::to_underlying(mask) & std::to_underlying(particle_kind_bit(kind))) != 0;
}

struct ParticleNodeDesc {
  std::string_view name = {};
  std::string_view category = {};
  u32 input_count = 0;
  bool has_output = true;
  // number of literal float4 parameters the node carries when a pin is unconnected, plus any
  // index parameters (curve row, random stream)
  u32 param_count = 0;
  // which programs this node is meaningful in, the compiler rejects it anywhere else
  ParticleKindMask kinds = ParticleKindMask::Particle;
  // nodes that keep a value between frames claim one emitter state slot
  bool stateful = false;
};

auto particle_node_desc(ParticleNodeType type) -> const ParticleNodeDesc&;
auto is_particle_output_node(ParticleNodeType type) -> bool;

struct ParticleNode {
  ParticleNodeID id = ParticleNodeID::Invalid;
  ParticleNodeType type = ParticleNodeType::Constant;
  glm::vec2 canvas_position = {};
  ankerl::svector<glm::vec4, 3> params = {};
  // curve/gradient row this node samples, or the random stream index
  u32 index = 0;
};

struct ParticleLink {
  ParticleLinkID id = ParticleLinkID::Invalid;
  ParticleNodeID from_node = ParticleNodeID::Invalid;
  ParticleNodeID to_node = ParticleNodeID::Invalid;
  u32 to_pin = 0;
};

struct ParticleGraph {
  std::vector<ParticleNode> nodes = {};
  std::vector<ParticleLink> links = {};

  auto find_node(this const ParticleGraph& self, ParticleNodeID id) -> const ParticleNode*;
  auto add_node(this ParticleGraph& self, ParticleNodeType type, glm::vec2 canvas_position) -> ParticleNodeID;
  auto remove_node(this ParticleGraph& self, ParticleNodeID id) -> void;
  auto add_link(this ParticleGraph& self, ParticleNodeID from, ParticleNodeID to, u32 to_pin) -> ParticleLinkID;
  auto remove_link(this ParticleGraph& self, ParticleLinkID id) -> void;
};

struct ParticleProgram {
  std::vector<GPU::ParticleInstruction> instructions = {};
  std::vector<glm::vec4> constants = {};
};

struct ParticleCompiledPrograms {
  std::vector<GPU::ParticleInstruction> instructions = {};
  std::vector<glm::vec4> constants = {};
  u32 spawn_offset = 0;
  u32 spawn_count = 0;
  u32 update_offset = 0;
  u32 update_count = 0;

  // kept out of the GPU pool. this one only ever runs on the CPU
  std::vector<GPU::ParticleInstruction> emitter_instructions = {};
  std::vector<glm::vec4> emitter_constants = {};
  // a graph holding a Pulse node takes ownership of queued gameplay bursts, without one they are
  // added to the spawn count directly
  bool emitter_consumes_pulse = false;
};

auto compile_particle_graph(const ParticleGraph& graph, ParticleProgramKind kind)
  -> std::expected<ParticleProgram, std::string>;

auto compile_particle_graphs(
  const ParticleGraph& emitter_graph, const ParticleGraph& spawn_graph, const ParticleGraph& update_graph
) -> std::expected<ParticleCompiledPrograms, std::string>;
} // namespace ox
