#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "Asset/ParticleEmitterProgram.hpp"
#include "Asset/ParticleGraph.hpp"

using namespace ox;

namespace {
struct EmitterHarness {
  ParticleGraph graph = {};
  std::vector<GPU::ParticleInstruction> instructions = {};
  std::vector<glm::vec4> constants = {};
  std::array<glm::vec4, GPU::PARTICLE_EMITTER_STATE_COUNT> state = {};

  auto add(const ParticleNodeType type, const glm::vec4& value = {}) -> ParticleNodeID {
    const auto id = graph.add_node(type, {});
    auto* node = const_cast<ParticleNode*>(graph.find_node(id));
    if (!node->params.empty()) {
      node->params[0] = value;
    }
    return id;
  }

  // looked up fresh every time: add() pushes into graph.nodes and invalidates any held pointer
  auto set_param(const ParticleNodeID id, const usize index, const glm::vec4& value) -> void {
    auto* node = const_cast<ParticleNode*>(graph.find_node(id));
    ASSERT_NE(node, nullptr);
    ASSERT_LT(index, node->params.size());
    node->params[index] = value;
  }

  auto link(const ParticleNodeID from, const ParticleNodeID to, const u32 pin = 0) -> void {
    graph.add_link(from, to, pin);
  }

  auto compile() -> testing::AssertionResult {
    auto program = compile_particle_graph(graph, ParticleProgramKind::Emitter);
    if (!program) {
      return testing::AssertionFailure() << program.error();
    }

    instructions = std::move(program->instructions);
    constants = std::move(program->constants);

    return testing::AssertionSuccess();
  }

  auto tick(const f32 delta_time, const f32 time, const f32 spawn_rate = 0.0f) -> ParticleEmitterProgramOutput {
    return run_particle_emitter_program(
      instructions,
      constants,
      ParticleEmitterProgramInput{.time = time, .delta_time = delta_time, .spawn_rate = spawn_rate},
      state
    );
  }
};
} // namespace

TEST(ParticleEmitterProgram, AnEmptyProgramPassesTheSpawnRateThrough) {
  auto harness = EmitterHarness{};
  ASSERT_TRUE(harness.compile());

  const auto result = harness.tick(0.1f, 0.1f, 32.0f);
  EXPECT_FLOAT_EQ(result.spawn_rate, 32.0f);
  EXPECT_FLOAT_EQ(result.spawn, 0.0f);
}

TEST(ParticleEmitterProgram, IntervalFiresOncePerPeriod) {
  auto harness = EmitterHarness{};
  const auto interval = harness.add(ParticleNodeType::Interval, glm::vec4(1.0f));
  const auto spawn = harness.add(ParticleNodeType::SpawnParticles);
  harness.link(interval, spawn);
  ASSERT_TRUE(harness.compile());

  // four quarter-second steps land exactly on the period, so only the last one fires
  auto time = 0.0f;
  for (auto i = 0; i < 3; i++) {
    time += 0.25f;
    EXPECT_FLOAT_EQ(harness.tick(0.25f, time).spawn, 0.0f) << "step " << i;
  }

  time += 0.25f;
  EXPECT_FLOAT_EQ(harness.tick(0.25f, time).spawn, 1.0f);

  // and the accumulator carries the remainder rather than resetting
  for (auto i = 0; i < 3; i++) {
    time += 0.25f;
    EXPECT_FLOAT_EQ(harness.tick(0.25f, time).spawn, 0.0f);
  }
  time += 0.25f;
  EXPECT_FLOAT_EQ(harness.tick(0.25f, time).spawn, 1.0f);
}

TEST(ParticleEmitterProgram, IntervalCatchesUpOverALongFrame) {
  auto harness = EmitterHarness{};
  const auto interval = harness.add(ParticleNodeType::Interval, glm::vec4(0.1f));
  const auto spawn = harness.add(ParticleNodeType::SpawnParticles);
  harness.link(interval, spawn);
  ASSERT_TRUE(harness.compile());

  // a single half-second frame covers five periods and must not silently drop four of them
  EXPECT_FLOAT_EQ(harness.tick(0.5f, 0.5f).spawn, 5.0f);
}

TEST(ParticleEmitterProgram, IntervalWithNoPeriodNeverFires) {
  auto harness = EmitterHarness{};
  const auto interval = harness.add(ParticleNodeType::Interval, glm::vec4(0.0f));
  const auto spawn = harness.add(ParticleNodeType::SpawnParticles);
  harness.link(interval, spawn);
  ASSERT_TRUE(harness.compile());

  EXPECT_FLOAT_EQ(harness.tick(1.0f, 1.0f).spawn, 0.0f);
  EXPECT_FLOAT_EQ(harness.tick(1.0f, 2.0f).spawn, 0.0f);
}

TEST(ParticleEmitterProgram, OnceFiresExactlyOnce) {
  auto harness = EmitterHarness{};
  const auto once = harness.add(ParticleNodeType::Once, glm::vec4(0.5f));
  const auto spawn = harness.add(ParticleNodeType::SpawnParticles);
  harness.link(once, spawn);
  ASSERT_TRUE(harness.compile());

  EXPECT_FLOAT_EQ(harness.tick(0.25f, 0.25f).spawn, 0.0f);
  EXPECT_FLOAT_EQ(harness.tick(0.25f, 0.50f).spawn, 1.0f);
  EXPECT_FLOAT_EQ(harness.tick(0.25f, 0.75f).spawn, 0.0f);
  EXPECT_FLOAT_EQ(harness.tick(0.25f, 1.00f).spawn, 0.0f);
}

TEST(ParticleEmitterProgram, OnceReArmsWhenTheStateIsCleared) {
  auto harness = EmitterHarness{};
  const auto once = harness.add(ParticleNodeType::Once, glm::vec4(0.0f));
  const auto spawn = harness.add(ParticleNodeType::SpawnParticles);
  harness.link(once, spawn);
  ASSERT_TRUE(harness.compile());

  EXPECT_FLOAT_EQ(harness.tick(0.1f, 0.1f).spawn, 1.0f);
  EXPECT_FLOAT_EQ(harness.tick(0.1f, 0.2f).spawn, 0.0f);

  // this is what Scene::restart_particles does
  harness.state = {};
  EXPECT_FLOAT_EQ(harness.tick(0.1f, 0.3f).spawn, 1.0f);
}

TEST(ParticleEmitterProgram, OnRisingFiresOnTheCrossingAndNotWhileHeld) {
  auto harness = EmitterHarness{};
  const auto trigger = harness.add(ParticleNodeType::OnRising, glm::vec4(0.0f));
  const auto spawn = harness.add(ParticleNodeType::SpawnParticles);
  harness.set_param(trigger, 1, glm::vec4(0.5f)); // threshold
  harness.link(trigger, spawn);
  ASSERT_TRUE(harness.compile());

  const auto drive = [&](const f32 value) {
    harness.set_param(trigger, 0, glm::vec4(value));
    // the literal is folded into the constant pool at compile time, so recompile to change it
    EXPECT_TRUE(harness.compile());
    return harness.tick(0.1f, 0.1f).spawn;
  };

  EXPECT_FLOAT_EQ(drive(0.0f), 0.0f);
  EXPECT_FLOAT_EQ(drive(1.0f), 1.0f); // crossed
  EXPECT_FLOAT_EQ(drive(1.0f), 0.0f); // still high, no second fire
  EXPECT_FLOAT_EQ(drive(0.0f), 0.0f); // dropped back
  EXPECT_FLOAT_EQ(drive(1.0f), 1.0f); // and re-arms
}

TEST(ParticleEmitterProgram, SetSpawnRateOverridesTheEmitterSetting) {
  auto harness = EmitterHarness{};
  const auto value = harness.add(ParticleNodeType::Constant, glm::vec4(7.0f));
  const auto rate = harness.add(ParticleNodeType::SetSpawnRate);
  harness.link(value, rate);
  ASSERT_TRUE(harness.compile());

  EXPECT_FLOAT_EQ(harness.tick(0.1f, 0.1f, 32.0f).spawn_rate, 7.0f);
}

TEST(ParticleEmitterProgram, SeveralSpawnNodesAccumulate) {
  auto harness = EmitterHarness{};
  const auto first_value = harness.add(ParticleNodeType::Constant, glm::vec4(3.0f));
  const auto first_spawn = harness.add(ParticleNodeType::SpawnParticles);
  harness.link(first_value, first_spawn);

  const auto second_value = harness.add(ParticleNodeType::Constant, glm::vec4(4.0f));
  const auto second_spawn = harness.add(ParticleNodeType::SpawnParticles);
  harness.link(second_value, second_spawn);
  ASSERT_TRUE(harness.compile());

  EXPECT_FLOAT_EQ(harness.tick(0.1f, 0.1f).spawn, 7.0f);
}

TEST(ParticleEmitterProgram, ATriggerCanBeScaledIntoABurstCount) {
  auto harness = EmitterHarness{};
  const auto interval = harness.add(ParticleNodeType::Interval, glm::vec4(1.0f));
  const auto count = harness.add(ParticleNodeType::Multiply, glm::vec4(0.0f));
  const auto spawn = harness.add(ParticleNodeType::SpawnParticles);
  harness.set_param(count, 1, glm::vec4(20.0f));
  harness.link(interval, count, 0);
  harness.link(count, spawn);
  ASSERT_TRUE(harness.compile());

  EXPECT_FLOAT_EQ(harness.tick(0.5f, 0.5f).spawn, 0.0f);
  EXPECT_FLOAT_EQ(harness.tick(0.5f, 1.0f).spawn, 20.0f);
}

TEST(ParticleEmitterProgram, ParticleOnlyNodesAreRejectedInAnEmitterGraph) {
  auto graph = ParticleGraph{};
  const auto velocity = graph.add_node(ParticleNodeType::ReadVelocity, {});
  const auto spawn = graph.add_node(ParticleNodeType::SpawnParticles, {});
  graph.add_link(velocity, spawn, 0);

  const auto program = compile_particle_graph(graph, ParticleProgramKind::Emitter);
  ASSERT_FALSE(program.has_value());
  EXPECT_THAT(program.error(), testing::HasSubstr("Velocity"));
}

TEST(ParticleEmitterProgram, EmitterOnlyNodesAreRejectedInAParticleGraph) {
  auto graph = ParticleGraph{};
  const auto interval = graph.add_node(ParticleNodeType::Interval, {});
  const auto velocity = graph.add_node(ParticleNodeType::SetVelocity, {});
  graph.add_link(interval, velocity, 0);

  const auto program = compile_particle_graph(graph, ParticleProgramKind::Update);
  ASSERT_FALSE(program.has_value());
  EXPECT_THAT(program.error(), testing::HasSubstr("Interval"));
}

TEST(ParticleEmitterProgram, APulseNodeClaimsGameplayBursts) {
  auto emitter_graph = ParticleGraph{};
  const auto pulse = emitter_graph.add_node(ParticleNodeType::ReadPulse, {});
  const auto spawn = emitter_graph.add_node(ParticleNodeType::SpawnParticles, {});
  emitter_graph.add_link(pulse, spawn, 0);

  const auto compiled = compile_particle_graphs(emitter_graph, {}, {});
  ASSERT_TRUE(compiled.has_value());
  EXPECT_TRUE(compiled->emitter_consumes_pulse);
  EXPECT_FALSE(compiled->emitter_instructions.empty());
  // the emitter program must stay out of the pool that gets uploaded to the GPU
  EXPECT_TRUE(compiled->instructions.empty());
}

TEST(ParticleEmitterProgram, WithoutAPulseNodeTheGraphDoesNotClaimBursts) {
  const auto compiled = compile_particle_graphs({}, {}, {});
  ASSERT_TRUE(compiled.has_value());
  EXPECT_FALSE(compiled->emitter_consumes_pulse);
}

TEST(ParticleEmitterProgram, PulseReachesTheSpawnCount) {
  auto harness = EmitterHarness{};
  const auto pulse = harness.add(ParticleNodeType::ReadPulse);
  const auto spawn = harness.add(ParticleNodeType::SpawnParticles);
  harness.link(pulse, spawn);
  ASSERT_TRUE(harness.compile());

  const auto result = run_particle_emitter_program(
    harness.instructions,
    harness.constants,
    ParticleEmitterProgramInput{.delta_time = 0.1f, .pulse = 12.0f},
    harness.state
  );
  EXPECT_FLOAT_EQ(result.spawn, 12.0f);
}

TEST(ParticleEmitterProgram, RunningOutOfTriggerStateSlotsIsAnError) {
  auto graph = ParticleGraph{};
  const auto spawn = graph.add_node(ParticleNodeType::SpawnParticles, {});

  auto previous = ParticleNodeID::Invalid;
  for (auto i = 0_u32; i < GPU::PARTICLE_EMITTER_STATE_COUNT + 1; i++) {
    const auto interval = graph.add_node(ParticleNodeType::Interval, {});
    // each new trigger feeds the previous one, so the whole chain stays reachable from the output
    graph.add_link(interval, previous == ParticleNodeID::Invalid ? spawn : previous, 0);
    previous = interval;
  }

  const auto program = compile_particle_graph(graph, ParticleProgramKind::Emitter);
  ASSERT_FALSE(program.has_value());
  EXPECT_THAT(program.error(), testing::HasSubstr("state bank"));
}
