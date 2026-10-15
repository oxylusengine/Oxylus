#include <ankerl/svector.h>
#include <cstring>

#include "Render/Graph/RenderGraph.hpp"
#include "Render/Graph/Types.hpp"
#include "RenderGraphTest.hpp"

namespace ox {
inline auto constant_u32(const Module& module, const Program& program, ValueID id) -> u32 {
  const auto found = std::ranges::find(program.instructions, id, &Instr::id);
  const auto& node = found == program.instructions.end() ? module.get(id) : found->ir;
  return std::get<ir::Constant>(node).as_u32();
}

TEST(RenderGraphModulePort, ImportedBufferExportAtTheSameReadAccessNeedsNoBarrier) {
  auto module = Module{};
  const auto buffer = module.import_buffer({.size = 256}, Access::ComputeRead);
  const auto compiled = compile(module, module.export_to(buffer, Access::ComputeRead));
  EXPECT_TRUE(memory_barriers(module, compiled).empty());
}

TEST(RenderGraphModulePort, ExportChangingTheImageLayoutEmitsATransition) {
  auto module = Module{};
  const auto image = module.declare_image_var("history", FORMAT, Access::FragmentSampled);
  const auto compiled = compile(module, module.export_to(image, Access::CopyRead));
  const auto barriers = image_barriers(module, compiled);
  ASSERT_EQ(barriers.size(), 1u);
  EXPECT_EQ(barriers[0].old_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  EXPECT_EQ(barriers[0].new_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
}

TEST(RenderGraphModulePort, ATransientReachedOnlyByDeviceAddressIsBuiltWithAddressUsage) {
  auto module = Module{};
  const auto target = module.transient_image(untyped_info());
  const auto buffer = module.transient_buffer({.size = 64});
  const auto end = module.begin_compute({{target, Access::ComputeWrite}})
                     .bind_compute_pipeline(pipeline(0))
                     .push_constant_address(0, buffer)
                     .dispatch(1_u32, 1_u32, 1_u32)
                     .end_compute<1>()[0];
  const auto compiled = compile(module, end);
  EXPECT_EQ(find_all<ir::ConstructBuffer>(compiled).size(), 1u);
  EXPECT_EQ(buffer_usage(compiled, buffer), VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
  const auto dispatches = find_all<ir::Dispatch>(compiled);
  ASSERT_EQ(dispatches.size(), 1u);
  EXPECT_EQ(dispatches[0]->push_constants.addresses, (std::vector<std::pair<u32, ValueID>>{{0, buffer}}));
}

TEST(RenderGraphModulePort, ABufferVariableSharesOneResourceAndInfersItsUsage) {
  auto module = Module{};
  const auto buffer = module.declare_buffer_var("buffer", Access::HostWrite);
  const auto written = module.begin_compute({{buffer, Access::ComputeWrite}})
                         .bind_compute_pipeline(pipeline(0))
                         .dispatch(1_u32, 1_u32, 1_u32)
                         .end_compute<1>()[0];
  const auto end = module.begin_compute({{written, Access::ComputeRead}})
                     .bind_compute_pipeline(pipeline(0))
                     .dispatch(1_u32, 1_u32, 1_u32)
                     .end_compute<1>()[0];
  auto compiled = compile(module, end);
  EXPECT_EQ(find_all<ir::ConstructBuffer>(compiled).size(), 1u);
  EXPECT_EQ(buffer_usage(compiled, buffer), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  compiled.set(buffer, Buffer{.size = 128});
  const auto* bound = compiled.bound_variable(buffer);
  ASSERT_NE(bound, nullptr);
  EXPECT_EQ(std::get<Buffer>(bound->value).size, 128u);
  EXPECT_EQ(module.resource_root(end), buffer);
}

TEST(RenderGraphModulePort, BoundAndVariableSizedImagesUseDynamicViewportsAndRuntimeDispatchSizes) {
  for (const auto bound : {false, true}) {
    SCOPED_TRACE(bound);
    auto module = Module{};
    const auto extent = module.declare_extent_3d_var("extent", {WIDTH, HEIGHT, 1});
    const auto target = bound ? module.declare_image_var("image", FORMAT, Access::FragmentSampled)
                              : module.transient_image_sized(untyped_info(), extent);
    const auto written = module.begin_compute({{target, Access::ComputeWrite}})
                           .bind_compute_pipeline(pipeline(0))
                           .dispatch_invocations_per_pixel(target)
                           .end_compute<1>()[0];
    const auto end = module.begin_rendering({{written, Access::ColorRW}})
                       .bind_graphics_pipeline(pipeline(1))
                       .set_dynamic_state(DynamicStateFlags::None)
                       .draw(3_u32, 1_u32)
                       .end_rendering<1>()[0];
    const auto compiled = compile(module, end);
    const auto draws = find_all<ir::Draw>(compiled);
    const auto dispatches = find_all<ir::Dispatch>(compiled);
    ASSERT_EQ(draws.size(), 1u);
    ASSERT_EQ(dispatches.size(), 1u);
    EXPECT_EQ(draws[0]->state.rendering.color_formats, std::vector{FORMAT});
    EXPECT_TRUE(intersects(draws[0]->state.dynamic, DynamicStateFlags::Viewport | DynamicStateFlags::Scissor));
    EXPECT_EQ(dispatches[0]->size.kind, ir::DispatchSize::Kind::InvocationsPerPixel);
    EXPECT_EQ(dispatches[0]->size.resource, target);
  }
}

TEST(RenderGraphModulePort, DepthAttachmentTransitionsAndInfersItsUsage) {
  auto module = Module{};
  const auto color = module.transient_image(untyped_info());
  const auto depth = module.transient_image({.extent = {WIDTH, HEIGHT, 1}, .format = DEPTH_FORMAT});
  const auto end = module.begin_rendering({{color, Access::ColorRW}, {depth, Access::DepthStencilRW}})
                     .bind_graphics_pipeline(pipeline(0))
                     .set_depth(DepthState::less())
                     .draw(3_u32, 1_u32)
                     .end_rendering<2>()[0];
  const auto compiled = compile(module, end);
  EXPECT_EQ(image_usage(compiled, depth), vir::ImageUsageFlags::DepthStencilAttachment);
  const auto barriers = image_barriers(module, compiled);
  const auto into_depth = std::ranges::find(barriers, depth, &Barrier::resource);
  ASSERT_NE(into_depth, barriers.end());
  EXPECT_EQ(into_depth->dst, Access::DepthStencilRW);
  EXPECT_EQ(into_depth->new_layout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
}

TEST(RenderGraphModulePort, RebindingVertexAndIndexBuffersDoesNotRepeatTheirHostBarrier) {
  auto module = Module{};
  const auto target = module.transient_image(transient_info());
  const auto buffer = module.declare_buffer_var("geometry", Access::HostWrite);
  const auto end = module.begin_rendering({{target, Access::ColorRW}})
                     .bind_graphics_pipeline(pipeline(0))
                     .bind_vertex_buffer(0, buffer)
                     .bind_index_buffer(buffer, VK_INDEX_TYPE_UINT32)
                     .draw_indexed(3_u32, 1_u32)
                     .bind_vertex_buffer(0, buffer)
                     .bind_index_buffer(buffer, VK_INDEX_TYPE_UINT32)
                     .draw_indexed(3_u32, 1_u32)
                     .end_rendering<1>()[0];
  const auto compiled = compile(module, end);
  const auto barriers = memory_barriers(module, compiled);
  ASSERT_EQ(barriers.size(), 1u) << compiled.dump();
  EXPECT_EQ(barriers[0], (std::pair{Access::HostWrite, Access::AttributeRead | Access::IndexRead}));
}

TEST(RenderGraphModulePort, IndexedDrawLowersItsRangeAndSignedVertexOffset) {
  auto module = Module{};
  const auto target = module.transient_image(transient_info());
  const auto end = module.begin_rendering({{target, Access::ColorRW}})
                     .bind_graphics_pipeline(pipeline(4))
                     .set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)
                     .draw_indexed_range(12_u32, 2_u32, 3_u32, -7, 5_u32)
                     .end_rendering<1>()[0];
  const auto compiled = compile(module, end);
  const auto draws = find_all<ir::DrawIndexed>(compiled);
  ASSERT_EQ(draws.size(), 1u);
  EXPECT_EQ(draws[0]->pipeline, pipeline(4));
  EXPECT_EQ(draws[0]->state.topology, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
  EXPECT_EQ(constant_u32(module, compiled, draws[0]->index_count), 12u);
  EXPECT_EQ(constant_u32(module, compiled, draws[0]->first_index), 3u);
  EXPECT_EQ(std::get<ir::Constant>(module.get(draws[0]->vertex_offset)).as_i32(), -7);
  EXPECT_EQ(constant_u32(module, compiled, draws[0]->first_instance), 5u);
}

TEST(RenderGraphModulePort, RebindingPipelinesKeepsStateAndANewPassResetsIt) {
  auto module = Module{};
  const auto target = module.transient_image(transient_info());
  const auto first = module.begin_rendering({{target, Access::ColorRW}})
                       .set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_LINE_LIST)
                       .bind_graphics_pipeline(pipeline(0))
                       .draw(3_u32, 1_u32)
                       .bind_graphics_pipeline(pipeline(1))
                       .draw(3_u32, 1_u32)
                       .end_rendering<1>()[0];
  const auto end = module.begin_rendering({{first, Access::ColorRW}}).draw(3_u32, 1_u32).end_rendering<1>()[0];
  const auto compiled = compile(module, end);
  const auto draws = find_all<ir::Draw>(compiled);
  ASSERT_EQ(draws.size(), 3u);
  EXPECT_EQ(draws[0]->state.topology, VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
  EXPECT_EQ(draws[1]->state.topology, VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
  EXPECT_EQ(draws[1]->pipeline, pipeline(1));
  EXPECT_EQ(draws[2]->pipeline, PipelineID::Invalid);
  EXPECT_EQ(draws[2]->state.topology, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
}

TEST(RenderGraphModulePort, DynamicViewportValuesTravelWithDrawsAndStaticValuesKeyPipelines) {
  for (const auto dynamic : {DynamicStateFlags::Viewport | DynamicStateFlags::Scissor, DynamicStateFlags::None}) {
    auto module = Module{};
    const auto target = module.transient_image(transient_info());
    const auto half = Rect2D::relative(0.0f, 0.0f, 0.5f, 0.5f);
    const auto end = module.begin_rendering_area({{target, Access::ColorRW}}, {40, 20})
                       .bind_graphics_pipeline(pipeline(0))
                       .set_dynamic_state(dynamic)
                       .set_viewport(0, half)
                       .draw(3_u32, 1_u32)
                       .set_viewport(0, Rect2D::framebuffer())
                       .draw(3_u32, 1_u32)
                       .end_rendering<1>()[0];
    const auto compiled = compile(module, end);
    const auto draws = find_all<ir::Draw>(compiled);
    ASSERT_EQ(draws.size(), 2u);
    if (dynamic == DynamicStateFlags::None) {
      EXPECT_NE(draws[0]->state, draws[1]->state);
      ASSERT_EQ(draws[0]->state.viewports.size(), 1u);
      EXPECT_EQ(draws[0]->state.viewports[0].width, 20.0f);
      EXPECT_EQ(draws[0]->state.viewports[0].height, 10.0f);
    } else {
      EXPECT_EQ(draws[0]->state, draws[1]->state);
      EXPECT_EQ(draws[0]->state.hash(), draws[1]->state.hash());
      ASSERT_EQ(draws[0]->dynamic.viewports.size(), 1u);
      EXPECT_EQ(draws[0]->dynamic.viewports[0].rect, half);
    }
  }
}

TEST(RenderGraphModulePort, PushesBeforePipelineBindingReachOnlyFollowingDraws) {
  auto module = Module{};
  const auto target = module.transient_image(transient_info());
  const auto end = module.begin_rendering({{target, Access::ColorRW}})
                     .draw(3_u32, 1_u32)
                     .push_constants(17_u32)
                     .bind_graphics_pipeline(pipeline(2))
                     .draw(3_u32, 1_u32)
                     .end_rendering<1>()[0];
  const auto compiled = compile(module, end);
  const auto draws = find_all<ir::Draw>(compiled);
  ASSERT_EQ(draws.size(), 2u);
  EXPECT_TRUE(draws[0]->dynamic.push_constants.is_empty());
  ASSERT_EQ(draws[1]->dynamic.push_constants.data.size(), sizeof(u32));
  auto pushed = 0_u32;
  std::memcpy(&pushed, draws[1]->dynamic.push_constants.data.data(), sizeof(pushed));
  EXPECT_EQ(pushed, 17u);
}

TEST(RenderGraphModulePort, WritingBytesOverAVariablePushReplacesIt) {
  auto module = Module{};
  const auto target = module.transient_image(transient_info());
  const auto block = module.declare_bytes_var("uniform", 16);
  const auto end = module.begin_rendering({{target, Access::ColorRW}})
                     .bind_graphics_pipeline(pipeline(0))
                     .push_constants_from(block)
                     .draw(3_u32, 1_u32)
                     .push_constants(11_u32)
                     .draw(3_u32, 1_u32)
                     .end_rendering<1>()[0];
  const auto compiled = compile(module, end);
  const auto draws = find_all<ir::Draw>(compiled);
  ASSERT_EQ(draws.size(), 2u);
  EXPECT_EQ(
    draws[0]->dynamic.push_constants.variables,
    (std::vector<PushConstants::Variable>{{.offset = 0, .size = 16, .source = block}})
  );
  EXPECT_TRUE(draws[1]->dynamic.push_constants.variables.empty());
  EXPECT_TRUE(compiled.has_variable(block));
}

TEST(RenderGraphModulePort, ScaledDispatchSizesRoundUpAndPreserveRuntimeMeasurements) {
  auto module = Module{};
  const auto image = module.transient_image({.extent = {65, 33, 1}, .format = FORMAT});
  const auto buffer = module.transient_buffer({.size = 800});
  const auto image_end = module.begin_compute({{image, Access::ComputeWrite}})
                           .bind_compute_pipeline(pipeline(0))
                           .dispatch_invocations_per_pixel(image, {0.5f, 2.0f, 0.25f})
                           .end_compute<1>()[0];
  const auto buffer_end = module.begin_compute({{buffer, Access::ComputeWrite}})
                            .bind_compute_pipeline(pipeline(0))
                            .dispatch_invocations_per_element(buffer, 20, 3.0f)
                            .end_compute<1>()[0];
  const auto roots = std::array{image_end, buffer_end};
  const auto compiled = module.compile_all(PipelineBindings::unchecked(), roots);
  ASSERT_TRUE(compiled.has_value());
  const auto dispatches = find_all<ir::Dispatch>(*compiled);
  ASSERT_EQ(dispatches.size(), 2u);
  EXPECT_EQ(dispatches[0]->size.kind, ir::DispatchSize::Kind::Invocations);
  EXPECT_EQ(constant_u32(module, *compiled, dispatches[0]->size.x), 33u);
  EXPECT_EQ(constant_u32(module, *compiled, dispatches[0]->size.y), 66u);
  EXPECT_EQ(constant_u32(module, *compiled, dispatches[0]->size.z), 1u);
  EXPECT_EQ(constant_u32(module, *compiled, dispatches[1]->size.x), 120u);
  EXPECT_EQ(constant_u32(module, *compiled, dispatches[1]->size.y), 1u);
}

TEST(RenderGraphModulePort, CountedIndirectDrawsWaitOnBothBuffersAndInferTheirUsage) {
  for (const auto indexed : {false, true}) {
    auto module = Module{};
    const auto target = module.transient_image(transient_info());
    const auto commands = module.declare_buffer_var("commands", Access::HostWrite);
    const auto counts = module.declare_buffer_var("counts", Access::HostWrite);
    module.begin_rendering({{target, Access::ColorRW}}).bind_graphics_pipeline(pipeline(3));
    if (indexed)
      module.draw_indexed_indirect_count(commands, 32, counts, 4, 8_u32);
    else
      module.draw_indirect_count(commands, 32, counts, 4, 8_u32);
    const auto compiled = compile(module, module.end_rendering<1>()[0]);
    const auto draws = find_all<ir::DrawIndirect>(compiled);
    ASSERT_EQ(draws.size(), 1u);
    EXPECT_EQ(draws[0]->pipeline, pipeline(3));
    EXPECT_EQ(draws[0]->is_indexed, indexed);
    EXPECT_EQ(draws[0]->offset, 32u);
    EXPECT_EQ(draws[0]->count.buffer, counts);
    EXPECT_EQ(draws[0]->count.offset, 4u);
    EXPECT_EQ(constant_u32(module, compiled, draws[0]->count.max), 8u);
    EXPECT_EQ(buffer_usage(compiled, commands), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    EXPECT_EQ(buffer_usage(compiled, counts), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    EXPECT_EQ(memory_barriers(module, compiled).size(), 1u);
  }
}

TEST(RenderGraphModulePort, RepeatedIndirectDrawsAndDispatchesWaitOnce) {
  auto module = Module{};
  const auto target = module.transient_image(transient_info());
  const auto commands = module.declare_buffer_var("commands", Access::HostWrite);
  const auto dispatched = module.begin_compute({{target, Access::ComputeWrite}})
                            .bind_compute_pipeline(pipeline(0))
                            .dispatch_indirect(commands)
                            .dispatch_indirect(commands, 16)
                            .end_compute<1>()[0];
  const auto end = module.begin_rendering({{dispatched, Access::ColorRW}})
                     .bind_graphics_pipeline(pipeline(1))
                     .draw_indirect(commands, 1_u32)
                     .draw_indirect(commands, 1_u32)
                     .end_rendering<1>()[0];
  const auto compiled = compile(module, end);
  EXPECT_EQ(memory_barriers(module, compiled).size(), 1u) << compiled.dump();
  EXPECT_EQ(find_all<ir::Dispatch>(compiled).size(), 2u);
  EXPECT_EQ(find_all<ir::DrawIndirect>(compiled).size(), 2u);
}

TEST(RenderGraphModulePort, SpecializationsBeforeBindingAndBetweenDispatchesKeepSeparateValues) {
  auto module = Module{};
  const auto target = module.transient_buffer({.size = 64});
  const auto end = module.begin_compute({{target, Access::ComputeWrite}})
                     .specialize_constant(3, 8_u32)
                     .bind_compute_pipeline(pipeline(0))
                     .dispatch(1_u32, 1_u32, 1_u32)
                     .specialize_constant(3, 16_u32)
                     .dispatch(1_u32, 1_u32, 1_u32)
                     .end_compute<1>()[0];
  const auto compiled = compile(module, end);
  const auto dispatches = find_all<ir::Dispatch>(compiled);
  ASSERT_EQ(dispatches.size(), 2u);
  EXPECT_EQ(dispatches[0]->specialization.get(3), SpecValue::from(8_u32));
  EXPECT_EQ(dispatches[1]->specialization.get(3), SpecValue::from(16_u32));
  EXPECT_NE(dispatches[0]->specialization, dispatches[1]->specialization);
}

TEST(RenderGraphModulePort, ADispatchReadingAPreviousWriteWaitsOnTheRootAndConsumesTheWrittenVersion) {
  auto module = Module{};
  const auto image = module.transient_image(untyped_info());
  const auto written = module.begin_compute({{image, Access::ComputeWrite}})
                         .bind_compute_pipeline(pipeline(0))
                         .dispatch(1_u32, 1_u32, 1_u32)
                         .end_compute<1>()[0];
  const auto end = module.begin_compute({{written, Access::ComputeRead}})
                     .bind_compute_pipeline(pipeline(0))
                     .dispatch(1_u32, 1_u32, 1_u32)
                     .end_compute<1>()[0];
  const auto compiled = compile(module, end);
  const auto barriers = find_all<ir::ImageBarrier>(compiled);
  ASSERT_EQ(barriers.size(), 2u);
  EXPECT_EQ(barriers[1]->value, module.resource_root(written));
  const auto passes = find_all<ir::BeginCompute>(compiled);
  ASSERT_EQ(passes.size(), 2u);
  EXPECT_EQ(passes[1]->attachments[0].first, written);
  EXPECT_EQ(access_of(module, compiled, barriers[1]->src_access), Access::ComputeWrite);
  EXPECT_EQ(access_of(module, compiled, barriers[1]->dst_access), Access::ComputeRead);
}

TEST(RenderGraphModulePort, GlobalsAndBarrierConstantsPrecedeEveryBranchAndBlocksStayContiguous) {
  auto module = Module{};
  const auto target = module.transient_image(transient_info());
  const auto enabled = module.declare_bool_var("enabled", true);
  const auto chosen = module.set_condition(
    enabled,
    [&](Module& arm) -> ValueID { return arm.clear(target, clear::WHITE); },
    [&](Module&) -> ValueID { return target; }
  );
  const auto compiled = compile(module, module.export_to(chosen, Access::FragmentSampled));
  const auto first_label = position_of<ir::Label>(compiled);
  auto current = LabelID::Invalid;
  auto seen = ankerl::svector<LabelID, 8>{};
  for (usize index = 0; index < compiled.instructions.size(); index++) {
    const auto& instruction = compiled.instructions[index];
    if (const auto* label = std::get_if<ir::Label>(&instruction.ir)) {
      EXPECT_EQ(std::ranges::find(seen, label->label), seen.end());
      seen.push_back(label->label);
      current = label->label;
      const auto resolved = compiled.label_index(label->label);
      ASSERT_TRUE(resolved.has_value());
      EXPECT_EQ(*resolved, index);
    } else if (
      std::holds_alternative<ir::Type>(instruction.ir) || std::holds_alternative<ir::Constant>(instruction.ir) ||
      std::holds_alternative<ir::Variable>(instruction.ir) || std::holds_alternative<ir::ConstructImage>(instruction.ir)
    ) {
      EXPECT_LT(index, first_label) << compiled.dump();
    } else if (index_of(instruction.id) < module.instructions.size()) {
      EXPECT_EQ(module.block_of(instruction.id), current) << compiled.dump();
    }
    for (const auto& entry : barrier_entries<ir::ImageBarrier>(instruction.ir)) {
      const auto* barrier = &entry;
      for (const auto access : {barrier->src_access, barrier->dst_access}) {
        const auto definition = std::ranges::find(compiled.instructions, access, &Instr::id);
        ASSERT_NE(definition, compiled.instructions.end());
        EXPECT_LT(static_cast<usize>(definition - compiled.instructions.begin()), first_label);
      }
    }
  }
  EXPECT_FALSE(compiled.label_index(LabelID::Invalid).has_value());
}

TEST(RenderGraphModulePort, BufferWritesInBothArmsNeedNoExtraJoinBarrier) {
  auto module = Module{};
  const auto buffer = module.transient_buffer({.size = 64});
  const auto enabled = module.declare_bool_var("enabled", true);
  const auto written = module.set_condition(
    enabled,
    [&](Module& arm) -> ValueID {
      return arm.begin_compute({{buffer, Access::ComputeWrite}})
        .bind_compute_pipeline(pipeline(0))
        .dispatch(1_u32, 1_u32, 1_u32)
        .end_compute<1>()[0];
    },
    [&](Module& arm) -> ValueID {
      return arm.begin_compute({{buffer, Access::ComputeWrite}})
        .bind_compute_pipeline(pipeline(0))
        .dispatch(1_u32, 1_u32, 1_u32)
        .end_compute<1>()[0];
    }
  );
  const auto compiled = compile(module, module.export_to(written, Access::HostRead));
  const auto barriers = memory_barriers(module, compiled);
  ASSERT_EQ(barriers.size(), 1u) << compiled.dump();
  EXPECT_EQ(barriers[0], (std::pair{Access::ComputeWrite, Access::HostRead}));
}

TEST(RenderGraphModulePort, VariablesWithMatchingInitialValuesKeepSeparateSlots) {
  auto module = Module{};
  const auto first = module.declare_u32_var("first", 1);
  const auto second = module.declare_u32_var("second", 1);
  const auto target = module.transient_buffer({.size = 64});
  const auto end = module.begin_compute({{target, Access::ComputeWrite}})
                     .bind_compute_pipeline(pipeline(0))
                     .dispatch(first, second, 1_u32)
                     .end_compute<1>()[0];
  auto compiled = compile(module, end);
  EXPECT_NE(first, second);
  EXPECT_NE(compiled.slots.at(first), compiled.slots.at(second));
  compiled.set(first, 4_u32);
  EXPECT_EQ(std::get<u32>(compiled.variables[compiled.slots.at(second)].value), 1u);
}

TEST(RenderGraphModulePort, SettingAVariableTheProgramDoesNotHaveIsIgnored) {
  auto module = Module{};
  const auto count = module.declare_u32_var("count", 1);
  const auto unread = module.declare_bytes_var("unread", 8);
  const auto target = module.transient_buffer({.size = 64});
  const auto end = module.begin_compute({{target, Access::ComputeWrite}})
                     .bind_compute_pipeline(pipeline(0))
                     .dispatch(count, 1_u32, 1_u32)
                     .end_compute<1>()[0];
  auto compiled = compile(module, end);
  ASSERT_FALSE(compiled.has_variable(unread));
  compiled.set_bytes(unread, std::array<u8, 8>{});
  compiled.set(ValueID::Invalid, 4_u32);
  compiled.set(count, 4_u32);
  EXPECT_EQ(std::get<u32>(compiled.variables[compiled.slots.at(count)].value), 4u);
}

TEST(RenderGraphModulePort, ACallbackDeclaredWithItsBodyCarriesItIntoTheProgram) {
  auto module = Module{};
  auto is_called = false;
  const auto body = module.declare_callback_var("body", [&is_called](Recorder&) -> void { is_called = true; });
  const auto target = module.transient_buffer({.size = 64});
  const auto end = module.begin_compute({{target, Access::ComputeWrite}}).record_from(body).end_compute<1>()[0];
  const auto compiled = compile(module, end);
  const auto& held = std::get<PassCallback>(compiled.variables[compiled.slots.at(body)].value);
  ASSERT_TRUE(held);
  auto recorder = Recorder{};
  (*held)(recorder);
  EXPECT_TRUE(is_called);
}

TEST(RenderGraphModulePort, RecordingMisuseDropsOperationsAndLeavesAUsableModule) {
  auto module = Module{};
  module.draw(3_u32, 1_u32).dispatch(1_u32, 1_u32, 1_u32).bind_graphics_pipeline(pipeline(0));
  EXPECT_TRUE(std::ranges::none_of(module.instructions, [](const IR& node) -> bool {
    return std::holds_alternative<ir::Draw>(node) || std::holds_alternative<ir::Dispatch>(node) ||
           std::holds_alternative<ir::BindPipeline>(node);
  }));
  const auto target = module.transient_image(transient_info());
  const auto end = module.begin_rendering({{target, Access::ColorRW}})
                     .bind_graphics_pipeline(pipeline(0))
                     .dispatch(1_u32, 1_u32, 1_u32)
                     .draw(3_u32, 1_u32)
                     .end_rendering<1>()[0];
  const auto compiled = compile(module, end);
  EXPECT_TRUE(find_all<ir::Dispatch>(compiled).empty());
  EXPECT_EQ(find_all<ir::Draw>(compiled).size(), 1u);
}

TEST(RenderGraphModulePort, StructuralMisuseIsRejected) {
  EXPECT_DEATH(
    {
      Module module;
      module.begin_compute({});
    },
    "at least one resource"
  );
  EXPECT_DEATH(
    {
      Module module;
      const auto image = module.transient_image(transient_info());
      module.begin_compute({{image, Access::ComputeWrite}}).end_compute<2>();
    },
    "result"
  );
  EXPECT_DEATH(
    {
      Module module;
      const auto image = module.transient_image(transient_info());
      module.begin_compute({{image, Access::ComputeWrite}});
      module.compile(PipelineBindings::unchecked(), image);
    },
    "still open"
  );
}

TEST(RenderGraphModulePort, BindingWrongVariableKindOrImageFormatIsRejected) {
  auto module = Module{};
  const auto image = module.declare_image_var("image", FORMAT, Access::FragmentSampled);
  auto compiled = compile(module, module.clear(image, clear::BLACK));
  EXPECT_DEATH(compiled.set(image, Buffer{.size = 64}), "another kind");
  const auto wrong = Image::imported(VK_NULL_HANDLE, vir::Format::R32Uint, {WIDTH, HEIGHT, 1}, vir::Samples::e1);
  EXPECT_DEATH(
    compiled.set(image, ImageAttachment::from_image(wrong, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)),
    "format and sample count"
  );
}

TEST(RenderGraphModulePort, AResourceArrayIsOneAttachmentAndSynchronizesEveryElement) {
  auto module = Module{};
  const auto target = module.transient_image(transient_info());
  const auto first = module.transient_image(untyped_info());
  const auto second = module.transient_image(untyped_info());
  const auto type = module.lower_type({.kind = ir::TypeKind::Image, .format = FORMAT, .samples = vir::Samples::e1});
  const auto images = module.emit(ir::Array{.ty = type, .elements = {first, second}});
  const auto end = module.begin_rendering({{target, Access::ColorRW}, {images, Access::FragmentRead}})
                     .bind_graphics_pipeline(pipeline(0))
                     .draw(3_u32, 1_u32)
                     .end_rendering<2>()[0];
  const auto compiled = compile(module, end);
  const auto begins = find_all<ir::BeginRendering>(compiled);
  ASSERT_EQ(begins.size(), 1u);
  ASSERT_EQ(begins[0]->attachments.size(), 2u);
  EXPECT_EQ(begins[0]->attachments[1].first, images);
  const auto barriers = image_barriers(module, compiled);
  for (const auto image : {first, second}) {
    const auto found = std::ranges::find(barriers, image, &Barrier::resource);
    ASSERT_NE(found, barriers.end());
    EXPECT_EQ(found->dst, Access::FragmentRead);
    EXPECT_EQ(image_usage(compiled, image), vir::ImageUsageFlags::Storage);
  }
}

TEST(RenderGraphModulePort, MergedAttachmentAccessKeepsItsRenderingRole) {
  auto module = Module{};
  const auto color = module.transient_image(untyped_info());
  const auto sampled = module.transient_image(untyped_info());
  const auto end = module
                     .begin_rendering(
                       {{color, Access::ColorRW}, {color, Access::FragmentRead}, {sampled, Access::FragmentSampled}}
                     )
                     .bind_graphics_pipeline(pipeline(0))
                     .draw(3_u32, 1_u32)
                     .end_rendering<3>()[0];
  const auto compiled = compile(module, end);
  const auto begins = find_all<ir::BeginRendering>(compiled);
  const auto draws = find_all<ir::Draw>(compiled);
  ASSERT_EQ(begins.size(), 1u);
  ASSERT_EQ(draws.size(), 1u);
  ASSERT_EQ(begins[0]->attachments.size(), 2u);
  EXPECT_EQ(access_of(module, compiled, begins[0]->attachments[0].second), Access::ColorRW | Access::FragmentRead);
  EXPECT_EQ(draws[0]->state.rendering.color_formats, std::vector{FORMAT});
}

TEST(RenderGraphModulePort, AnUploadThenSampleInfersBothUsagesAndKeepsTheCopyOutsideRendering) {
  auto module = Module{};
  const auto target = module.transient_image(transient_info());
  const auto image = module.transient_image(untyped_info());
  const auto staging = module.declare_buffer_var("staging", Access::HostWrite);
  const auto copied = module.copy_buffer_to_image(staging, image);
  const auto end = module.begin_rendering({{target, Access::ColorRW}})
                     .bind_graphics_pipeline(pipeline(0))
                     .bind_texture(0, 0, copied, a_sampler())
                     .draw(3_u32, 1_u32)
                     .end_rendering<1>()[0];
  const auto declared = Declared::fragment({{0, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER}});
  const auto compiled = module.compile(declared.pipelines(), end);
  ASSERT_TRUE(compiled.has_value());
  EXPECT_LT(position_of<ir::CopyBufferToImage>(*compiled), position_of<ir::BeginRendering>(*compiled));
  EXPECT_EQ(image_usage(*compiled, image), vir::ImageUsageFlags::TransferDst | vir::ImageUsageFlags::Sampled);
  const auto barriers = image_barriers(module, *compiled);
  auto transitions = ankerl::svector<VkImageLayout, 4>{};
  for (const auto& barrier : barriers)
    if (barrier.resource == image)
      transitions.push_back(barrier.new_layout);
  EXPECT_TRUE(
    std::ranges::equal(
      transitions,
      std::array{VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}
    )
  );
}

TEST(RenderGraphModulePort, ABufferComputedThenBoundIsMadeVisibleBeforeRendering) {
  auto module = Module{};
  const auto target = module.transient_image(transient_info());
  const auto buffer = module.transient_buffer({.size = 256});
  const auto written = module.begin_compute({{buffer, Access::ComputeWrite}})
                         .bind_compute_pipeline(pipeline(0))
                         .dispatch(1_u32, 1_u32, 1_u32)
                         .end_compute<1>()[0];
  const auto end = module.begin_rendering({{target, Access::ColorRW}})
                     .bind_graphics_pipeline(pipeline(1))
                     .bind_vertex_buffer(0, written)
                     .draw(3_u32, 1_u32)
                     .end_rendering<1>()[0];
  const auto compiled = compile(module, end);
  EXPECT_LT(position_of<ir::EndCompute>(compiled), position_of<ir::BeginRendering>(compiled));
  EXPECT_LT(position_of<ir::MemoryBarrier>(compiled), position_of<ir::BeginRendering>(compiled));
  EXPECT_EQ(
    memory_barriers(module, compiled),
    (std::vector<std::pair<Access, Access>>{{Access::ComputeWrite, Access::AttributeRead}})
  );
}

TEST(RenderGraphModulePort, NestedSelectionsKeepEveryBranchAndPhiLabelResolvable) {
  auto module = Module{};
  const auto target = module.transient_image(transient_info());
  const auto first = module.declare_bool_var("first", true);
  const auto second = module.declare_bool_var("second", false);
  const auto nested = module.set_condition(
    first,
    [&](Module& outer) -> ValueID {
      return outer.set_condition(
        second,
        [&](Module& inner) -> ValueID { return inner.clear(target, clear::WHITE); },
        [&](Module& inner) -> ValueID { return inner.clear(target, clear::BLACK); }
      );
    },
    [&](Module&) -> ValueID { return target; }
  );
  const auto compiled = compile(module, module.export_to(nested, Access::FragmentSampled));
  EXPECT_EQ(find_all<ir::BranchConditional>(compiled).size(), 2u);
  for (const auto* branch : find_all<ir::BranchConditional>(compiled)) {
    EXPECT_TRUE(compiled.label_index(branch->true_label).has_value());
    EXPECT_TRUE(compiled.label_index(branch->false_label).has_value());
  }
  for (const auto* phi : find_all<ir::Phi>(compiled)) {
    for (const auto& [value, label] : phi->incoming) {
      EXPECT_TRUE(compiled.label_index(label).has_value());
      EXPECT_NE(std::ranges::find(compiled.instructions, value, &Instr::id), compiled.instructions.end());
    }
  }
  auto block = LabelID::Invalid;
  auto predecessors = ankerl::unordered_dense::map<LabelID, ankerl::svector<LabelID, 4>>{};
  for (const auto& instruction : compiled.instructions) {
    if (const auto* label = std::get_if<ir::Label>(&instruction.ir)) {
      block = label->label;
    } else if (const auto* branch = std::get_if<ir::Branch>(&instruction.ir)) {
      predecessors[branch->target].push_back(block);
    } else if (const auto* conditional = std::get_if<ir::BranchConditional>(&instruction.ir)) {
      predecessors[conditional->true_label].push_back(block);
      predecessors[conditional->false_label].push_back(block);
    }
  }
  for (const auto& instruction : compiled.instructions) {
    if (const auto* label = std::get_if<ir::Label>(&instruction.ir)) {
      block = label->label;
    } else if (const auto* phi = std::get_if<ir::Phi>(&instruction.ir)) {
      for (const auto& [value, incoming] : phi->incoming) {
        const auto& entries = predecessors[block];
        EXPECT_NE(std::ranges::find(entries, incoming), entries.end()) << compiled.dump();
      }
    }
  }
}

} // namespace ox
