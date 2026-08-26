#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "Asset/ParticleGraph.hpp"

using namespace ox;

static auto opcode_of(const GPU::ParticleInstruction& instruction) -> GPU::ParticleOpCode {
  return static_cast<GPU::ParticleOpCode>(instruction.op_dst & 0xFFu);
}

static auto dst_of(const GPU::ParticleInstruction& instruction) -> u32 { return (instruction.op_dst >> 8u) & 0xFu; }

static auto write_mask_of(const GPU::ParticleInstruction& instruction) -> u32 {
  return (instruction.op_dst >> 12u) & 0xFu;
}

static auto operand_kind(u32 operand) -> GPU::ParticleOperandKind {
  return static_cast<GPU::ParticleOperandKind>(operand >> 30u);
}

static auto operand_payload(u32 operand) -> u32 { return operand & 0x3FFFFFFFu; }

TEST(ParticleGraph, EmptyGraphCompilesToNothing) {
  const auto program = compile_particle_graph({}, ParticleProgramKind::Update);
  ASSERT_TRUE(program.has_value());
  EXPECT_TRUE(program->instructions.empty());
  EXPECT_TRUE(program->constants.empty());
}

TEST(ParticleGraph, OnlyOutputReachableNodesAreEmitted) {
  auto graph = ParticleGraph{};
  const auto used = graph.add_node(ParticleNodeType::Constant, {});
  const auto orphan = graph.add_node(ParticleNodeType::Sine, {});
  const auto output = graph.add_node(ParticleNodeType::SetVelocity, {});

  graph.add_link(used, output, 0);
  std::ignore = orphan;

  const auto program = compile_particle_graph(graph, ParticleProgramKind::Spawn);
  ASSERT_TRUE(program.has_value());

  // Constant folds into an operand, so the only instruction is the attribute write.
  ASSERT_EQ(program->instructions.size(), 1u);
  EXPECT_EQ(opcode_of(program->instructions[0]), GPU::ParticleOpCode::Mov);
  EXPECT_EQ(dst_of(program->instructions[0]), GPU::PARTICLE_REG_VELOCITY_TOTAL);
  EXPECT_EQ(write_mask_of(program->instructions[0]), 0b0111u);
}

TEST(ParticleGraph, ProducersAreEmittedBeforeConsumers) {
  auto graph = ParticleGraph{};
  // Declared consumer-first on purpose: only the topological sort can put these in the right order.
  const auto output = graph.add_node(ParticleNodeType::SetPosition, {});
  const auto outer = graph.add_node(ParticleNodeType::Sine, {});
  const auto inner = graph.add_node(ParticleNodeType::Cosine, {});
  const auto source = graph.add_node(ParticleNodeType::ReadTime, {});

  graph.add_link(source, inner, 0);
  graph.add_link(inner, outer, 0);
  graph.add_link(outer, output, 0);

  const auto program = compile_particle_graph(graph, ParticleProgramKind::Update);
  ASSERT_TRUE(program.has_value());

  ASSERT_EQ(program->instructions.size(), 4u);
  EXPECT_EQ(opcode_of(program->instructions[0]), GPU::ParticleOpCode::Time);
  EXPECT_EQ(opcode_of(program->instructions[1]), GPU::ParticleOpCode::Cos);
  EXPECT_EQ(opcode_of(program->instructions[2]), GPU::ParticleOpCode::Sin);
  EXPECT_EQ(opcode_of(program->instructions[3]), GPU::ParticleOpCode::Mov);
}

TEST(ParticleGraph, CyclesAreRejected) {
  auto graph = ParticleGraph{};
  const auto a = graph.add_node(ParticleNodeType::Add, {});
  const auto b = graph.add_node(ParticleNodeType::Add, {});
  const auto output = graph.add_node(ParticleNodeType::SetPosition, {});

  graph.add_link(a, b, 0);
  graph.add_link(b, a, 0);
  graph.add_link(b, output, 0);

  const auto program = compile_particle_graph(graph, ParticleProgramKind::Update);
  ASSERT_FALSE(program.has_value());
  EXPECT_THAT(program.error(), testing::HasSubstr("cycle"));
}

TEST(ParticleGraph, LinkToAMissingNodeIsRejected) {
  auto graph = ParticleGraph{};
  const auto source = graph.add_node(ParticleNodeType::Constant, {});
  const auto output = graph.add_node(ParticleNodeType::SetColor, {});
  graph.add_link(source, output, 0);
  graph.links[0].to_node = static_cast<ParticleNodeID>(4242);

  const auto program = compile_particle_graph(graph, ParticleProgramKind::Spawn);
  EXPECT_FALSE(program.has_value());
}

TEST(ParticleGraph, LinksToPinsANodeDoesNotHaveAreRejected) {
  auto graph = ParticleGraph{};
  const auto source = graph.add_node(ParticleNodeType::Constant, {});
  const auto sine = graph.add_node(ParticleNodeType::Sine, {});

  // Sine has exactly one input pin, and Constant has none.
  EXPECT_NE(graph.add_link(source, sine, 0), ParticleLinkID::Invalid);
  EXPECT_EQ(graph.add_link(source, sine, 1), ParticleLinkID::Invalid);
  EXPECT_EQ(graph.add_link(source, sine, ~0_u32), ParticleLinkID::Invalid);
  EXPECT_EQ(graph.add_link(sine, source, 0), ParticleLinkID::Invalid);
  EXPECT_EQ(graph.add_link(sine, sine, 0), ParticleLinkID::Invalid);
  EXPECT_EQ(graph.add_link(source, static_cast<ParticleNodeID>(99), 0), ParticleLinkID::Invalid);

  // A rejected link must not have disturbed the one that was already there.
  ASSERT_EQ(graph.links.size(), 1u);
  EXPECT_EQ(graph.links[0].from_node, source);
  EXPECT_EQ(graph.links[0].to_node, sine);
  EXPECT_EQ(graph.links[0].to_pin, 0u);
}

TEST(ParticleGraph, ScratchRegistersAreReusedAfterLastUse) {
  auto graph = ParticleGraph{};
  const auto time = graph.add_node(ParticleNodeType::ReadTime, {});
  const auto age = graph.add_node(ParticleNodeType::ReadAge, {});
  const auto add = graph.add_node(ParticleNodeType::Add, {});
  const auto multiply = graph.add_node(ParticleNodeType::Multiply, {});
  const auto output = graph.add_node(ParticleNodeType::SetPosition, {});

  graph.add_link(time, add, 0);
  graph.add_link(age, add, 1);
  graph.add_link(add, multiply, 0);
  graph.add_link(add, multiply, 1);
  graph.add_link(multiply, output, 0);

  const auto program = compile_particle_graph(graph, ParticleProgramKind::Update);
  ASSERT_TRUE(program.has_value());
  ASSERT_EQ(program->instructions.size(), 5u);

  EXPECT_EQ(opcode_of(program->instructions[0]), GPU::ParticleOpCode::Time);
  EXPECT_EQ(opcode_of(program->instructions[1]), GPU::ParticleOpCode::AgeNorm);
  EXPECT_EQ(opcode_of(program->instructions[2]), GPU::ParticleOpCode::Add);
  EXPECT_EQ(opcode_of(program->instructions[3]), GPU::ParticleOpCode::Mul);

  EXPECT_GE(dst_of(program->instructions[0]), GPU::PARTICLE_ATTRIBUTE_REGISTERS);

  // The Add was the last consumer of both reads, so the Multiply lands on a register they freed.
  const auto reused = dst_of(program->instructions[3]);
  EXPECT_TRUE(reused == dst_of(program->instructions[0]) || reused == dst_of(program->instructions[1]));
}

TEST(ParticleGraph, RunningOutOfRegistersIsAnError) {
  auto graph = ParticleGraph{};
  const auto output = graph.add_node(ParticleNodeType::SetPosition, {});
  auto previous = graph.add_node(ParticleNodeType::Add, {});
  graph.add_link(previous, output, 0);

  // Every ReadTime stays live until the final Add consumes it, so the scratch file runs dry.
  for (auto i = 0; i < 32; i++) {
    const auto value = graph.add_node(ParticleNodeType::ReadTime, {});
    const auto combine = graph.add_node(ParticleNodeType::Add, {});
    graph.add_link(value, combine, 0);
    graph.add_link(combine, previous, 1);
    previous = combine;
  }

  const auto program = compile_particle_graph(graph, ParticleProgramKind::Update);
  ASSERT_FALSE(program.has_value());
  EXPECT_THAT(program.error(), testing::HasSubstr("registers"));
}

TEST(ParticleGraph, IdenticalConstantsShareOnePoolSlot) {
  auto graph = ParticleGraph{};
  const auto a = graph.add_node(ParticleNodeType::Constant, {});
  const auto b = graph.add_node(ParticleNodeType::Constant, {});
  const auto c = graph.add_node(ParticleNodeType::Constant, {});
  const auto add = graph.add_node(ParticleNodeType::Add, {});
  const auto multiply = graph.add_node(ParticleNodeType::Multiply, {});
  const auto output = graph.add_node(ParticleNodeType::SetPosition, {});

  graph.nodes[0].params = {glm::vec4(1.0f, 2.0f, 3.0f, 4.0f)};
  graph.nodes[1].params = {glm::vec4(1.0f, 2.0f, 3.0f, 4.0f)};
  graph.nodes[2].params = {glm::vec4(9.0f)};

  graph.add_link(a, add, 0);
  graph.add_link(b, add, 1);
  graph.add_link(add, multiply, 0);
  graph.add_link(c, multiply, 1);
  graph.add_link(multiply, output, 0);

  const auto program = compile_particle_graph(graph, ParticleProgramKind::Spawn);
  ASSERT_TRUE(program.has_value());

  ASSERT_EQ(program->constants.size(), 2u);
  EXPECT_EQ(program->constants[0], glm::vec4(1.0f, 2.0f, 3.0f, 4.0f));
  EXPECT_EQ(program->constants[1], glm::vec4(9.0f));

  const auto& add_instruction = program->instructions[0];
  EXPECT_EQ(operand_kind(add_instruction.src0), GPU::ParticleOperandKind::Constant);
  EXPECT_EQ(operand_payload(add_instruction.src0), operand_payload(add_instruction.src1));
}

TEST(ParticleGraph, SetLifetimeWritesBothHalvesOfTheLifePair) {
  auto graph = ParticleGraph{};
  const auto source = graph.add_node(ParticleNodeType::Constant, {});
  const auto output = graph.add_node(ParticleNodeType::SetLifetime, {});
  graph.nodes[0].params = {glm::vec4(2.5f)};
  graph.add_link(source, output, 0);

  const auto program = compile_particle_graph(graph, ParticleProgramKind::Spawn);
  ASSERT_TRUE(program.has_value());

  ASSERT_EQ(program->instructions.size(), 2u);
  EXPECT_EQ(dst_of(program->instructions[0]), GPU::PARTICLE_REG_POSITION_LIFE);
  EXPECT_EQ(write_mask_of(program->instructions[0]), 0b1000u);
  EXPECT_EQ(dst_of(program->instructions[1]), GPU::PARTICLE_REG_VELOCITY_TOTAL);
  EXPECT_EQ(write_mask_of(program->instructions[1]), 0b1000u);
}

TEST(ParticleGraph, AddVelocityAccumulatesOntoTheAttributeRegister) {
  auto graph = ParticleGraph{};
  const auto source = graph.add_node(ParticleNodeType::Constant, {});
  const auto output = graph.add_node(ParticleNodeType::AddVelocity, {});
  graph.add_link(source, output, 0);

  const auto program = compile_particle_graph(graph, ParticleProgramKind::Update);
  ASSERT_TRUE(program.has_value());

  ASSERT_EQ(program->instructions.size(), 1u);
  const auto& instruction = program->instructions[0];
  EXPECT_EQ(opcode_of(instruction), GPU::ParticleOpCode::Add);
  EXPECT_EQ(dst_of(instruction), GPU::PARTICLE_REG_VELOCITY_TOTAL);
  EXPECT_EQ(write_mask_of(instruction), 0b0111u);
  EXPECT_EQ(operand_kind(instruction.src0), GPU::ParticleOperandKind::Register);
  EXPECT_EQ(operand_payload(instruction.src0), GPU::PARTICLE_REG_VELOCITY_TOTAL);
}

TEST(ParticleGraph, GoldenBytecodeForAGravityGraph) {
  auto graph = ParticleGraph{};
  const auto gravity = graph.add_node(ParticleNodeType::Constant, {});
  const auto delta = graph.add_node(ParticleNodeType::ReadDeltaTime, {});
  const auto step = graph.add_node(ParticleNodeType::Multiply, {});
  const auto output = graph.add_node(ParticleNodeType::AddVelocity, {});

  graph.nodes[0].params = {glm::vec4(0.0f, -9.81f, 0.0f, 0.0f)};

  graph.add_link(gravity, step, 0);
  graph.add_link(delta, step, 1);
  graph.add_link(step, output, 0);

  const auto program = compile_particle_graph(graph, ParticleProgramKind::Update);
  ASSERT_TRUE(program.has_value());

  ASSERT_EQ(program->constants.size(), 1u);
  EXPECT_EQ(program->constants[0], glm::vec4(0.0f, -9.81f, 0.0f, 0.0f));

  ASSERT_EQ(program->instructions.size(), 3u);

  // Swizzle r15 = r4.zzzz  (delta time out of the context register)
  EXPECT_EQ(opcode_of(program->instructions[0]), GPU::ParticleOpCode::Swizzle);
  EXPECT_EQ(write_mask_of(program->instructions[0]), 0b1111u);
  EXPECT_EQ(operand_kind(program->instructions[0].src0), GPU::ParticleOperandKind::Register);
  EXPECT_EQ(operand_payload(program->instructions[0].src0), GPU::PARTICLE_REG_CONTEXT);
  EXPECT_EQ(operand_kind(program->instructions[0].src1), GPU::ParticleOperandKind::Immediate);
  EXPECT_EQ(operand_payload(program->instructions[0].src1), 0b10'10'10'10u);

  // Mul rN = constant0 * rDelta
  EXPECT_EQ(opcode_of(program->instructions[1]), GPU::ParticleOpCode::Mul);
  EXPECT_EQ(operand_kind(program->instructions[1].src0), GPU::ParticleOperandKind::Constant);
  EXPECT_EQ(operand_payload(program->instructions[1].src0), 0u);
  EXPECT_EQ(operand_kind(program->instructions[1].src1), GPU::ParticleOperandKind::Register);
  EXPECT_EQ(operand_payload(program->instructions[1].src1), dst_of(program->instructions[0]));

  // Add r1.xyz = r1 + rN
  EXPECT_EQ(opcode_of(program->instructions[2]), GPU::ParticleOpCode::Add);
  EXPECT_EQ(dst_of(program->instructions[2]), GPU::PARTICLE_REG_VELOCITY_TOTAL);
  EXPECT_EQ(write_mask_of(program->instructions[2]), 0b0111u);
  EXPECT_EQ(operand_payload(program->instructions[2].src1), dst_of(program->instructions[1]));
}

TEST(ParticleGraph, MergedProgramsShareOneConstantPool) {
  auto spawn = ParticleGraph{};
  const auto spawn_source = spawn.add_node(ParticleNodeType::Constant, {});
  const auto spawn_output = spawn.add_node(ParticleNodeType::SetSize, {});
  spawn.nodes[0].params = {glm::vec4(0.5f)};
  spawn.add_link(spawn_source, spawn_output, 0);

  auto update = ParticleGraph{};
  const auto update_source = update.add_node(ParticleNodeType::Constant, {});
  const auto update_output = update.add_node(ParticleNodeType::AddPosition, {});
  update.nodes[0].params = {glm::vec4(1.0f, 0.0f, 0.0f, 0.0f)};
  update.add_link(update_source, update_output, 0);

  const auto merged = compile_particle_graphs(spawn, update);
  ASSERT_TRUE(merged.has_value());

  EXPECT_EQ(merged->spawn_offset, 0u);
  EXPECT_EQ(merged->spawn_count, 1u);
  EXPECT_EQ(merged->update_offset, 1u);
  EXPECT_EQ(merged->update_count, 1u);
  ASSERT_EQ(merged->constants.size(), 2u);
  EXPECT_EQ(merged->constants[0], glm::vec4(0.5f));
  EXPECT_EQ(merged->constants[1], glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));

  // The update half's constant operand must have been rebased onto the shared pool.
  const auto& update_instruction = merged->instructions[merged->update_offset];
  EXPECT_EQ(operand_kind(update_instruction.src1), GPU::ParticleOperandKind::Constant);
  EXPECT_EQ(operand_payload(update_instruction.src1), 1u);
}

TEST(ParticleGraph, CompilationIsDeterministic) {
  auto graph = ParticleGraph{};
  const auto a = graph.add_node(ParticleNodeType::ReadAge, {});
  const auto b = graph.add_node(ParticleNodeType::ReadTime, {});
  const auto add = graph.add_node(ParticleNodeType::Add, {});
  const auto output = graph.add_node(ParticleNodeType::SetPosition, {});

  graph.add_link(a, add, 0);
  graph.add_link(b, add, 1);
  graph.add_link(add, output, 0);

  const auto first = compile_particle_graph(graph, ParticleProgramKind::Update);
  const auto second = compile_particle_graph(graph, ParticleProgramKind::Update);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  ASSERT_EQ(first->instructions.size(), second->instructions.size());

  for (usize i = 0; i < first->instructions.size(); i++) {
    EXPECT_EQ(first->instructions[i].op_dst, second->instructions[i].op_dst);
    EXPECT_EQ(first->instructions[i].src0, second->instructions[i].src0);
    EXPECT_EQ(first->instructions[i].src1, second->instructions[i].src1);
    EXPECT_EQ(first->instructions[i].src2, second->instructions[i].src2);
  }
}

TEST(ParticleGraph, ParameterNodeCarriesItsSlotAsAnImmediate) {
  auto graph = ParticleGraph{};
  const auto parameter = graph.add_node(ParticleNodeType::ReadParameter, {});
  const auto output = graph.add_node(ParticleNodeType::AddVelocity, {});

  graph.nodes.front().index = 2;
  graph.add_link(parameter, output, 0);

  const auto program = compile_particle_graph(graph, ParticleProgramKind::Update);
  ASSERT_TRUE(program.has_value());
  ASSERT_GE(program->instructions.size(), 1u);

  const auto& instruction = program->instructions[0];
  EXPECT_EQ(opcode_of(instruction), GPU::ParticleOpCode::Param);
  EXPECT_EQ(operand_kind(instruction.src1), GPU::ParticleOperandKind::Immediate);
  EXPECT_EQ(operand_payload(instruction.src1), 2u);
}
