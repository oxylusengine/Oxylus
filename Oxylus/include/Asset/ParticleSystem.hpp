#pragma once

#include <filesystem>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "Asset/ParticleGraph.hpp"
#include "Asset/Texture.hpp"
#include "Core/Option.hpp"
#include "Core/Types.hpp"
#include "Core/UUID.hpp"

namespace ox {
enum class ParticleSystemID : u64 { Invalid = std::numeric_limits<u64>::max() };

enum class ParticleEmissionShape : u32 {
  Point = 0,
  Sphere,
  Hemisphere,
  Box,
  Circle,
  Cone,
  Count,
};

enum class ParticleSimulationSpace : u32 {
  World = 0,
  Local,
};

enum class ParticleRenderMode : u32 {
  Billboard = 0,
  Mesh,
};

enum class ParticleBillboardMode : u32 {
  FaceCamera = 0,
  VelocityStretched,
  HorizontalPlane,
  VerticalPlane,
};

enum class ParticleBlendMode : u32 {
  AlphaBlend = 0,
  Additive,
};

struct ParticleBurst {
  f32 time = 0.0f;
  u32 count = 10;
  u32 cycles = 1;
  f32 interval = 1.0f;
};

struct ParticleEmitterSettings {
  u32 capacity = 1024;
  f32 spawn_rate = 32.0f;
  f32 duration = 5.0f;
  f32 start_delay = 0.0f;
  bool looping = true;
  glm::vec2 lifetime = {1.0f, 2.0f};
  ParticleEmissionShape shape = ParticleEmissionShape::Sphere;
  glm::vec3 shape_size = {0.5f, 0.5f, 0.5f};
  f32 shape_angle = 25.0f;
  ParticleSimulationSpace simulation_space = ParticleSimulationSpace::World;
  u32 seed = 0;
  std::vector<ParticleBurst> bursts = {};
};

struct ParticleRenderSettings {
  UUID material = {};
  UUID mesh = {};
  ParticleRenderMode render_mode = ParticleRenderMode::Billboard;
  ParticleBillboardMode billboard = ParticleBillboardMode::FaceCamera;
  ParticleBlendMode blend = ParticleBlendMode::AlphaBlend;
  glm::uvec2 flipbook = {1u, 1u};
  f32 soft_particle_distance = 0.0f;
  f32 velocity_stretch = 1.0f;
  f32 restitution = 0.4f;
  bool sort = true;
  bool depth_collision = false;
};

struct ParticleCurve {
  std::string name = "Curve";
  std::vector<glm::vec2> points = {{0.0f, 0.0f}, {1.0f, 1.0f}};

  auto sample(this const ParticleCurve& self, f32 t) -> f32;
};

struct ParticleGradientKey {
  f32 t = 0.0f;
  glm::vec4 color = {1.0f, 1.0f, 1.0f, 1.0f};
};

struct ParticleParameter {
  std::string name = "Parameter";
  glm::vec4 default_value = {};
};

struct ParticleGradient {
  std::string name = "Gradient";
  std::vector<ParticleGradientKey> keys = {
    {0.0f, {1.0f, 1.0f, 1.0f, 1.0f}},
    {1.0f, {1.0f, 1.0f, 1.0f, 0.0f}},
  };

  auto sample(this const ParticleGradient& self, f32 t) -> glm::vec4;
};

struct ParticleSystem {
  constexpr static u32 CURVE_ATLAS_WIDTH = 64;

  ParticleEmitterSettings emitter = {};
  ParticleRenderSettings render = {};
  ParticleGraph spawn_graph = {};
  ParticleGraph update_graph = {};
  std::vector<ParticleCurve> curves = {};
  std::vector<ParticleGradient> gradients = {};
  std::vector<ParticleParameter> parameters = {};

  ParticleCompiledPrograms programs = {};
  std::string compile_error = {};
  Texture curve_atlas = {};

  ParticleSystem() = default;
  ~ParticleSystem() = default;
  ParticleSystem(const ParticleSystem&) = delete;
  ParticleSystem& operator=(const ParticleSystem&) = delete;
  ParticleSystem(ParticleSystem&&) = default;
  ParticleSystem& operator=(ParticleSystem&&) = default;

  static auto make_default() -> ParticleSystem;

  static auto read(const std::filesystem::path& path) -> option<ParticleSystem>;
  auto write(this const ParticleSystem& self, const std::filesystem::path& path) -> bool;

  auto recompile(this ParticleSystem& self) -> void;
  auto destroy(this ParticleSystem& self) -> void;

  auto curve_row_count(this const ParticleSystem& self) -> u32 { return static_cast<u32>(self.curves.size()); }
  auto atlas_row_count(this const ParticleSystem& self) -> u32 {
    return static_cast<u32>(self.curves.size() + self.gradients.size());
  }

  auto find_parameter(this const ParticleSystem& self, std::string_view name) -> option<u32>;
};

enum class ParticleEmitterID : u64 { Invalid = std::numeric_limits<u64>::max() };

// Per-entity runtime state. Lives on the Scene, not on the component, so nothing here reaches the
// scene JSON.
struct ParticleEmitterState {
  UUID asset = {};
  bool playing = false;
  f32 time = 0.0f;
  f32 spawn_accumulator = 0.0f;
  u32 pool_offset = 0;
  u32 capacity = 0;
  u32 pending_burst = 0;
  bool pool_valid = false;
  std::vector<u32> burst_cycles_fired = {};
};
} // namespace ox
