#pragma once

#include <ankerl/svector.h>
#include <expected>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <string>
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

  Count,
};

enum class ParticleProgramKind : u32 {
  Spawn = 0,
  Update = 1,
};

struct ParticleNodeDesc {
  std::string_view name = {};
  std::string_view category = {};
  u32 input_count = 0;
  bool has_output = true;
  // number of literal float4 parameters the node carries when a pin is unconnected, plus any
  // index parameters (curve row, random stream)
  u32 param_count = 0;
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
};

auto compile_particle_graph(const ParticleGraph& graph, ParticleProgramKind kind)
  -> std::expected<ParticleProgram, std::string>;

auto compile_particle_graphs(const ParticleGraph& spawn_graph, const ParticleGraph& update_graph)
  -> std::expected<ParticleCompiledPrograms, std::string>;
} // namespace ox
