#include <atomic>

#include "Render/Graph/Types.hpp"
#include "RenderGraphTest.hpp"

namespace ox {
TEST(Access, SampledAccessKeepsEachGraphicsStageDistinct) {
  const auto stages = vir::ShaderStage::Vertex | vir::ShaderStage::TessellationControl |
                      vir::ShaderStage::TessellationEvaluation | vir::ShaderStage::Fragment;
  EXPECT_EQ(
    sampled_by(stages),
    Access::VertexSampled | Access::TessellationControlSampled | Access::TessellationEvaluationSampled |
      Access::FragmentSampled
  );
}

TEST(Access, MeshAndTaskStagesMapToTheirOwnPipelineStages) {
  EXPECT_EQ(to_stage_flags(Access::MeshRead), VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT);
  EXPECT_EQ(to_stage_flags(Access::TaskSampled), VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT);
  EXPECT_EQ(shader_read_by(vir::ShaderStage::Mesh), Access::MeshRead);
  EXPECT_TRUE(writes(Access::TaskWrite));
}

TEST(Access, ComputeUniformReadsConvertToTheMatchingVulkanAccessAndStage) {
  EXPECT_EQ(to_access_flags(Access::ComputeUniformRead), VK_ACCESS_2_UNIFORM_READ_BIT);
  EXPECT_EQ(to_stage_flags(Access::ComputeUniformRead), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
}

TEST(Access, InputAttachmentAccessSelectsItsLayoutAndUsage) {
  EXPECT_EQ(to_image_layout(Access::InputAttachmentRead), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  EXPECT_EQ(to_image_usage(Access::InputAttachmentRead), vir::ImageUsageFlags::InputAttachment);
}

TEST(Access, AStorageWriteNextToASampleLandsInGeneral) {
  EXPECT_EQ(to_image_layout(Access::ComputeSampled | Access::ComputeWrite), VK_IMAGE_LAYOUT_GENERAL);
}

TEST(Subresource, IntersectionAndContainmentHonourRemainingCounts) {
  const auto whole = Subresource::whole();
  const auto mip = Subresource::mip(3);
  EXPECT_TRUE(whole.contains(mip));
  EXPECT_FALSE(mip.contains(whole));
  EXPECT_TRUE(whole.overlaps(mip));
  EXPECT_FALSE(Subresource::mip(2).overlaps(mip));
  EXPECT_EQ(whole.intersect(mip), mip);

  const auto range = VkImageSubresourceRange{
    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
    .baseMipLevel = 1,
    .levelCount = 4,
    .baseArrayLayer = 0,
    .layerCount = 2,
  };
  const auto applied = Subresource::mip(2).apply(range);
  EXPECT_EQ(applied.baseMipLevel, 3u);
  EXPECT_EQ(applied.levelCount, 1u);
  EXPECT_EQ(applied.layerCount, 2u);
  EXPECT_EQ(Subresource::mip(9).apply(range).levelCount, 0u);
}

TEST(RenderGraphModule, AnOpaqueBodyIsCompiledWithTheStateInForceWhereItIsCalled) {
  auto module = Module{};
  const auto target = module.transient_image(transient_info());
  const auto body = module.declare_callback_var("draws");

  const auto end = module.begin_rendering({{target, Access::ColorRW}})
                     .bind_graphics_pipeline(pipeline(3))
                     .set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)
                     .broadcast_color_blend(BlendPreset::AlphaBlend)
                     .record_from(body)
                     .end_rendering<1>()[0];

  const auto compiled = compile(module, end);
  const auto calls = find_all<ir::CallOpaque>(compiled);
  ASSERT_EQ(calls.size(), 1u) << compiled.dump();
  EXPECT_EQ(calls[0]->pipeline, pipeline(3));
  EXPECT_EQ(calls[0]->state.topology, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
  EXPECT_EQ(calls[0]->state.rendering.color_formats, std::vector{FORMAT});
}

TEST(RenderGraphModule, ARegionWithAnOpaqueBodyStillBarriersWhatItDeclared) {
  auto module = Module{};
  const auto swapchain = module_with_attachment(module);
  const auto sampled = module.transient_image(transient_info());
  const auto body = module.declare_callback_var("draws");

  const auto cleared = module.clear(sampled, clear::BLACK);
  const auto drawn = module.begin_rendering({{swapchain, Access::ColorRW}, {cleared, Access::FragmentSampled}})
                       .bind_graphics_pipeline(pipeline(0))
                       .record_from(body)
                       .end_rendering<2>()[0];
  const auto end = module.export_to(drawn, Access::FragmentSampled);

  const auto compiled = compile(module, end);
  auto transitions = std::vector<std::tuple<VkImageLayout, VkImageLayout, Access>>{};
  for (const auto& barrier : image_barriers(module, compiled)) {
    if (barrier.resource == sampled) {
      transitions.emplace_back(barrier.old_layout, barrier.new_layout, barrier.dst);
    }
  }

  const auto expected = std::vector<std::tuple<VkImageLayout, VkImageLayout, Access>>{
    {VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, Access::Clear},
    {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, Access::FragmentSampled},
  };
  EXPECT_EQ(transitions, expected) << compiled.dump();
}

TEST(RenderGraphModule, TheBodyOfAnOpaquePassIsAVariableOfTheProgram) {
  auto module = Module{};
  const auto target = module.transient_image(transient_info());
  const auto body = module.declare_callback_var("draws");

  const auto end = module.begin_rendering({{target, Access::ColorRW}})
                     .bind_graphics_pipeline(pipeline(0))
                     .record_from(body)
                     .end_rendering<1>()[0];

  auto compiled = compile(module, end);
  EXPECT_TRUE(compiled.has_variable(body));

  auto called = std::atomic_bool{false};
  compiled.set_callback(body, [&](Recorder&) { called = true; });
  EXPECT_FALSE(called.load()) << "binding is not calling";
}

TEST(RenderGraphModule, ATransientImageIsCreatedWithTheUsageTheGraphImplies) {
  auto module = Module{};
  const auto swapchain = module_with_attachment(module);
  const auto target = module.transient_image(transient_info());

  const auto rendered = module.begin_rendering({{target, Access::ColorRW}})
                          .bind_graphics_pipeline(pipeline(0))
                          .draw(3_u32, 1_u32)
                          .end_rendering<1>()[0];
  const auto blitted = module.blit(rendered, swapchain);
  const auto end = module.export_to(blitted, Access::FragmentSampled);

  EXPECT_EQ(
    image_usage(compile(module, end), target),
    static_cast<vir::ImageUsageFlags>(vir::ImageUsageFlags::ColorAttachment | vir::ImageUsageFlags::TransferSrc)
  );
}

TEST(RenderGraphModule, ABlitPutsEachSideInItsOwnTransferLayout) {
  auto module = Module{};
  const auto destination = module_with_attachment(module);
  const auto source = module.transient_image(transient_info());
  const auto end = module.blit(source, destination);

  const auto compiled = compile(module, end);
  const auto barriers = image_barriers(module, compiled);
  ASSERT_EQ(barriers.size(), 2u) << compiled.dump();
  EXPECT_EQ(barriers[0].resource, source);
  EXPECT_EQ(barriers[0].src, Access::None);
  EXPECT_EQ(barriers[0].dst, Access::BlitRead);
  EXPECT_EQ(barriers[0].old_layout, VK_IMAGE_LAYOUT_UNDEFINED);
  EXPECT_EQ(barriers[0].new_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  EXPECT_EQ(barriers[1].resource, destination);
  EXPECT_EQ(barriers[1].dst, Access::BlitWrite);
  EXPECT_EQ(barriers[1].new_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
}

TEST(RenderGraphModule, AnImportedImageStartsInTheLayoutItsOwnerLeftItIn) {
  auto module = Module{};
  const auto image = Image::imported(VK_NULL_HANDLE, FORMAT, {WIDTH, HEIGHT, 1}, vir::Samples::e1);
  const auto imported = module.import_image(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, Access::None);
  const auto end = module.clear(imported, clear::BLACK);

  const auto barriers = image_barriers(module, compile(module, end));
  ASSERT_EQ(barriers.size(), 1u);
  EXPECT_EQ(barriers[0].old_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  EXPECT_EQ(barriers[0].new_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
}

TEST(RenderGraphModule, AnImportedImageWaitsOnTheAccessItsOwnerLeftItIn) {
  auto module = Module{};
  const auto image = Image::imported(VK_NULL_HANDLE, DEPTH_FORMAT, {WIDTH, HEIGHT, 1}, vir::Samples::e1);
  const auto imported = module.import_image(image, VK_IMAGE_LAYOUT_UNDEFINED, Access::DepthStencilRW);
  const auto end = module.clear(imported, ClearValue::depth(1.0f));

  const auto barriers = image_barriers(module, compile(module, end));
  ASSERT_EQ(barriers.size(), 1u);
  EXPECT_EQ(barriers[0].src, Access::DepthStencilRW);
  EXPECT_EQ(barriers[0].dst, Access::Clear);
  EXPECT_EQ(barriers[0].old_layout, VK_IMAGE_LAYOUT_UNDEFINED);
}

TEST(RenderGraphModule, AReadFromANewStageAtTheSameLayoutStillWaits) {
  auto module = Module{};
  const auto target = module.transient_image(transient_info());
  const auto other = module.transient_image(transient_info());

  const auto drawn = module.begin_rendering({{target, Access::ColorRW}})
                       .bind_graphics_pipeline(pipeline(0))
                       .draw(3_u32, 1_u32)
                       .end_rendering<1>()[0];
  const auto sampled = module.begin_rendering({{other, Access::ColorRW}, {drawn, Access::FragmentSampled}})
                         .bind_graphics_pipeline(pipeline(0))
                         .draw(3_u32, 1_u32)
                         .end_rendering<2>()[1];
  const auto read = module.begin_compute({{sampled, Access::ComputeSampled}})
                      .bind_compute_pipeline(pipeline(1))
                      .dispatch(1_u32, 1_u32, 1_u32)
                      .end_compute<1>()[0];
  const auto end = module.export_to(read, Access::ComputeSampled);

  const auto compiled = compile(module, end);
  const auto barriers = image_barriers(module, compiled);
  const auto widening = std::ranges::find_if(barriers, [&](const Barrier& barrier) {
    return barrier.resource == target && barrier.dst == Access::ComputeSampled;
  });
  ASSERT_NE(widening, barriers.end()) << compiled.dump();
  EXPECT_EQ(widening->src, Access::FragmentSampled);
  EXPECT_EQ(widening->old_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  EXPECT_EQ(widening->new_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

TEST(RenderGraphModule, ASecondReadAtTheSameLayoutDoesNotRepeatTheBarrier) {
  auto module = Module{};
  const auto first = module_with_attachment(module);
  const auto source = module.transient_image(transient_info());

  const auto once = module.blit(source, first);
  const auto end = module.blit(source, once);

  const auto barriers = image_barriers(module, compile(module, end));
  EXPECT_EQ(std::ranges::count(barriers, source, &Barrier::resource), 1);
  // the destination is written twice, and a write always waits
  EXPECT_EQ(barriers.size(), 3u);
}

TEST(RenderGraphModule, ExportingAnImportedBufferAfterAWriteEmitsAMemoryBarrier) {
  auto module = Module{};
  const auto buffer = module.import_buffer(Buffer{}, Access::HostWrite);
  const auto written = module.begin_compute({{buffer, Access::ComputeWrite}})
                         .bind_compute_pipeline(pipeline(0))
                         .dispatch(1_u32, 1_u32, 1_u32)
                         .end_compute<1>()[0];
  const auto exported = module.export_to(written, Access::HostRead);

  const auto compiled = compile(module, exported);
  EXPECT_EQ(module.resource_root(exported), buffer);
  const auto expected = std::vector<std::pair<Access, Access>>{
    {Access::HostWrite, Access::ComputeWrite},
    {Access::ComputeWrite, Access::HostRead},
  };
  EXPECT_EQ(memory_barriers(module, compiled), expected) << compiled.dump();
  EXPECT_TRUE(image_barriers(module, compiled).empty());
}

TEST(RenderGraphModule, ExportingATransientBufferInfersTheExternalAccessUsage) {
  auto module = Module{};
  const auto buffer = module.transient_buffer({.size = 64, .usage = 0, .location = MemoryLocation::GpuOnly});
  const auto exported = module.export_to(buffer, Access::CopyRead, DomainFlag::Transfer);
  EXPECT_TRUE(buffer_usage(compile(module, exported), buffer) & VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
}

TEST(RenderGraphModule, ResourceLookupsResolveALongExportChain) {
  auto module = Module{};
  const auto buffer = module.import_buffer(Buffer{}, Access::ComputeRead);
  auto exported = buffer;
  for (auto index = 0; index < 300; index++) {
    exported = module.export_to(exported, Access::ComputeRead, DomainFlag::Compute);
  }

  EXPECT_EQ(module.resource_root(exported), buffer);
  const auto compiled = compile(module, exported);
  EXPECT_TRUE(memory_barriers(module, compiled).empty());
}

TEST(RenderGraphModule, AnExportToTheLayoutAnImageAlreadyRestsInCostsNothing) {
  auto module = Module{};
  const auto destination = module_with_attachment(module);
  const auto source = module.transient_image(transient_info());
  const auto blit = module.blit(source, destination);

  const auto shown = module.export_to(blit, Access::FragmentSampled);
  const auto resting = module.export_to(source, Access::BlitRead);
  const ValueID roots[] = {shown, resting};
  const auto compiled = module.compile_all(PipelineBindings::unchecked(), roots);
  ASSERT_TRUE(compiled.has_value());

  const auto barriers = image_barriers(module, *compiled);
  EXPECT_EQ(std::ranges::count(barriers, source, &Barrier::resource), 1);
}

TEST(RenderGraphModule, BindingAVertexBufferMakesTheHostWriteVisibleToVertexInput) {
  auto module = Module{};
  const auto attachment = module_with_attachment(module);
  const auto buffer = module.import_buffer(Buffer{}, Access::HostWrite);

  const auto end = module.begin_rendering({{attachment, Access::ColorRW}})
                     .bind_graphics_pipeline(pipeline(0))
                     .bind_vertex_buffer(0, buffer)
                     .draw(3_u32, 1_u32)
                     .end_rendering<1>()[0];

  const auto compiled = compile(module, end);
  const auto expected = std::vector<std::pair<Access, Access>>{{Access::HostWrite, Access::AttributeRead}};
  EXPECT_EQ(memory_barriers(module, compiled), expected);
  EXPECT_LT(position_of<ir::MemoryBarrier>(compiled), position_of<ir::BeginRendering>(compiled));

  const auto draws = find_all<ir::Draw>(compiled);
  ASSERT_EQ(draws.size(), 1u);
  EXPECT_EQ(draws[0]->pipeline, pipeline(0));
  EXPECT_EQ(draws[0]->state.rendering.color_formats, std::vector{FORMAT});
}

TEST(RenderGraphModule, ATransientBufferIsCreatedWithTheUsageTheGraphImplies) {
  auto module = Module{};
  const auto attachment = module_with_attachment(module);
  const auto scratch = module.transient_buffer({.size = 256, .usage = 0, .location = MemoryLocation::GpuOnly});

  const auto written = module.begin_compute({{scratch, Access::ComputeWrite}})
                         .bind_compute_pipeline(pipeline(0))
                         .push_constant_address(0, scratch)
                         .dispatch(1_u32, 1_u32, 1_u32)
                         .end_compute<1>()[0];
  const auto end = module.begin_rendering({{attachment, Access::ColorRW}})
                     .bind_graphics_pipeline(pipeline(0))
                     .bind_vertex_buffer(0, written)
                     .draw(3_u32, 1_u32)
                     .end_rendering<1>()[0];

  const auto usage = buffer_usage(compile(module, end), scratch);
  EXPECT_TRUE(usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  EXPECT_TRUE(usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
  EXPECT_TRUE(usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
}

TEST(RenderGraphModule, TheFirstWriteToATransientBufferWaitsOnNothing) {
  auto module = Module{};
  const auto scratch = module.transient_buffer({.size = 256, .usage = 0, .location = MemoryLocation::GpuOnly});
  const auto written = module.begin_compute({{scratch, Access::ComputeWrite}})
                         .bind_compute_pipeline(pipeline(0))
                         .dispatch(1_u32, 1_u32, 1_u32)
                         .end_compute<1>()[0];
  const auto end = module.export_to(written, Access::ComputeRead);
  const auto expected = std::vector<std::pair<Access, Access>>{{Access::ComputeWrite, Access::ComputeRead}};
  EXPECT_EQ(memory_barriers(module, compile(module, end)), expected);
}

TEST(RenderGraphModule, ABoundBufferWaitsAtTheAccessItWasDeclaredToRestAt) {
  auto module = Module{};
  const auto bound = module.declare_buffer_var("instances", Access::ComputeRead);
  const auto written = module.begin_compute({{bound, Access::ComputeWrite}})
                         .bind_compute_pipeline(pipeline(0))
                         .dispatch(1_u32, 1_u32, 1_u32)
                         .end_compute<1>()[0];
  const auto end = module.export_to(written, Access::ComputeRead);

  const auto expected = std::vector<std::pair<Access, Access>>{
    {Access::ComputeRead, Access::ComputeWrite},
    {Access::ComputeWrite, Access::ComputeRead},
  };
  const auto compiled = compile(module, end);
  EXPECT_EQ(memory_barriers(module, compiled), expected) << compiled.dump();
  EXPECT_TRUE(compiled.bound.contains(bound));
}

TEST(RenderGraphModule, ABoundImageRestsAtItsDeclaredAccessAndLayout) {
  auto module = Module{};
  const auto history = module.declare_image_var("history", FORMAT, Access::FragmentSampled);
  const auto written = module.begin_compute({{history, Access::ComputeWrite}})
                         .bind_compute_pipeline(pipeline(0))
                         .dispatch(1_u32, 1_u32, 1_u32)
                         .end_compute<1>()[0];
  const auto end = module.export_to(written, Access::FragmentSampled);

  const auto compiled = compile(module, end);
  const auto barriers = image_barriers(module, compiled);
  ASSERT_EQ(barriers.size(), 2u) << compiled.dump();
  EXPECT_EQ(barriers[0].src, Access::FragmentSampled);
  EXPECT_EQ(barriers[0].old_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  EXPECT_EQ(barriers[0].new_layout, VK_IMAGE_LAYOUT_GENERAL);
  EXPECT_EQ(barriers[1].dst, Access::FragmentSampled);
  EXPECT_EQ(barriers[1].new_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

TEST(RenderGraphModule, BindingTheWrongKindToAResourceSlotIsRejected) {
  auto module = Module{};
  const auto bound = module.declare_buffer_var("instances", Access::ComputeRead);
  const auto written = module.begin_compute({{bound, Access::ComputeWrite}})
                         .bind_compute_pipeline(pipeline(0))
                         .dispatch(1_u32, 1_u32, 1_u32)
                         .end_compute<1>()[0];
  auto compiled = compile(module, written);
  EXPECT_EQ(kind_of(Value(Buffer{}))->tag, ir::VariableKindTag::Buffer);
  compiled.set(bound, Buffer{});
  EXPECT_TRUE(std::holds_alternative<Buffer>(compiled.variables[compiled.slots.at(bound)].value));
}

TEST(RenderGraphModule, BuffersWaitingOnTheSamePairShareOneBarrier) {
  auto module = Module{};
  const auto first = module.import_buffer(Buffer{.size = 16, .allocation_id = 1}, Access::HostWrite);
  const auto second = module.import_buffer(Buffer{.size = 16, .allocation_id = 2}, Access::HostWrite);
  const auto read = module.begin_compute({{first, Access::ComputeRead}, {second, Access::ComputeRead}})
                      .bind_compute_pipeline(pipeline(0))
                      .dispatch(1_u32, 1_u32, 1_u32)
                      .end_compute<2>()[0];
  const auto expected = std::vector<std::pair<Access, Access>>{{Access::HostWrite, Access::ComputeRead}};
  EXPECT_EQ(memory_barriers(module, compile(module, read)), expected);
}

TEST(RenderGraphModule, ADispatchAndADrawCanReadTheirCountsFromVariables) {
  auto module = Module{};
  const auto attachment = module_with_attachment(module);
  const auto groups = module.declare_u32_var("groups", 4);
  const auto vertices = module.declare_u32_var("vertices", 3);
  const auto scratch = module.transient_buffer({.size = 64, .location = MemoryLocation::GpuOnly});

  const auto written = module.begin_compute({{scratch, Access::ComputeWrite}})
                         .bind_compute_pipeline(pipeline(0))
                         .dispatch(groups, 1_u32, 1_u32)
                         .end_compute<1>()[0];
  const auto end = module.begin_rendering({{attachment, Access::ColorRW}, {written, Access::VertexRead}})
                     .bind_graphics_pipeline(pipeline(1))
                     .draw(vertices, 1_u32)
                     .end_rendering<2>()[0];

  const auto compiled = compile(module, end);
  ASSERT_EQ(find_all<ir::Dispatch>(compiled).size(), 1u);
  EXPECT_EQ(find_all<ir::Dispatch>(compiled)[0]->size.x, groups);
  ASSERT_EQ(find_all<ir::Draw>(compiled).size(), 1u);
  EXPECT_EQ(find_all<ir::Draw>(compiled)[0]->vertex_count, vertices);
}

TEST(RenderGraphModule, ARegionOpenedWithDepthInfersTheDepthFormat) {
  auto module = Module{};
  const auto color = module.transient_image(transient_info());
  const auto depth = module.transient_image(ImageInfo::depth_target(extent_2d(), DEPTH_FORMAT));
  const auto end = module.begin_rendering({{color, Access::ColorRW}, {depth, Access::DepthStencilRW}})
                     .bind_graphics_pipeline(pipeline(0))
                     .set_depth(DepthState::less())
                     .draw(3_u32, 1_u32)
                     .end_rendering<2>()[0];

  const auto compiled = compile(module, end);
  const auto draws = find_all<ir::Draw>(compiled);
  ASSERT_EQ(draws.size(), 1u);
  EXPECT_EQ(draws[0]->state.rendering.depth_format, DEPTH_FORMAT);
  EXPECT_TRUE(draws[0]->state.depth.is_test_enabled);
  const auto barriers = image_barriers(module, compiled);
  const auto depth_barrier = std::ranges::find(barriers, depth, &Barrier::resource);
  ASSERT_NE(depth_barrier, barriers.end());
  EXPECT_EQ(depth_barrier->new_layout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
  EXPECT_TRUE(vir::intersects(image_usage(compiled, depth), vir::ImageUsageFlags::DepthStencilAttachment));
}

TEST(RenderGraphModule, DepthStateWithoutADepthAttachmentIsDropped) {
  auto module = Module{};
  const auto color = module.transient_image(transient_info());
  const auto end = module.begin_rendering({{color, Access::ColorRW}})
                     .bind_graphics_pipeline(pipeline(0))
                     .set_depth(DepthState::less())
                     .draw(3_u32, 1_u32)
                     .end_rendering<1>()[0];
  const auto compiled = compile(module, end);
  const auto draws = find_all<ir::Draw>(compiled);
  ASSERT_EQ(draws.size(), 1u);
  EXPECT_FALSE(draws[0]->state.depth.is_test_enabled);
}

TEST(RenderGraphModule, AStateChangeBetweenDrawsSplitsThemIntoTwoPermutations) {
  auto module = Module{};
  const auto color = module.transient_image(transient_info());
  const auto end = module.begin_rendering({{color, Access::ColorRW}})
                     .bind_graphics_pipeline(pipeline(0))
                     .draw(3_u32, 1_u32)
                     .set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_LINE_LIST)
                     .draw(3_u32, 1_u32)
                     .end_rendering<1>()[0];
  const auto compiled = compile(module, end);
  const auto draws = find_all<ir::Draw>(compiled);
  ASSERT_EQ(draws.size(), 2u);
  EXPECT_FALSE(draws[0]->state == draws[1]->state);
  EXPECT_EQ(draws[1]->state.topology, VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
}

TEST(RenderGraphModule, DrawsThatAgreeOnStateShareOnePermutation) {
  auto module = Module{};
  const auto color = module.transient_image(transient_info());
  const auto end = module.begin_rendering({{color, Access::ColorRW}})
                     .bind_graphics_pipeline(pipeline(0))
                     .draw(3_u32, 1_u32)
                     .draw(6_u32, 1_u32)
                     .end_rendering<1>()[0];
  const auto compiled = compile(module, end);
  const auto draws = find_all<ir::Draw>(compiled);
  ASSERT_EQ(draws.size(), 2u);
  EXPECT_TRUE(draws[0]->state == draws[1]->state);
  EXPECT_EQ(draws[0]->state.hash(), draws[1]->state.hash());
}

TEST(RenderGraphModule, PushConstantsReachTheDrawsThatFollowThem) {
  auto module = Module{};
  const auto color = module.transient_image(transient_info());
  const auto end = module.begin_rendering({{color, Access::ColorRW}})
                     .bind_graphics_pipeline(pipeline(0))
                     .draw(3_u32, 1_u32)
                     .push_constants(42_u32)
                     .draw(3_u32, 1_u32)
                     .end_rendering<1>()[0];
  const auto compiled = compile(module, end);
  const auto draws = find_all<ir::Draw>(compiled);
  ASSERT_EQ(draws.size(), 2u);
  EXPECT_TRUE(draws[0]->dynamic.push_constants.is_empty());
  ASSERT_EQ(draws[1]->dynamic.push_constants.size(), 4u);
  EXPECT_EQ(draws[1]->dynamic.push_constants.data[0], 42);
}

TEST(RenderGraphModule, PushConstantsFromAVariableReachTheDrawAsAVariable) {
  auto module = Module{};
  const auto color = module.transient_image(transient_info());
  const auto push = module.declare_bytes_var("push", 16);
  const auto end = module.begin_rendering({{color, Access::ColorRW}})
                     .bind_graphics_pipeline(pipeline(0))
                     .push_constants_from(push)
                     .draw(3_u32, 1_u32)
                     .end_rendering<1>()[0];
  auto compiled = compile(module, end);
  const auto draws = find_all<ir::Draw>(compiled);
  ASSERT_EQ(draws.size(), 1u);
  EXPECT_EQ(
    draws[0]->dynamic.push_constants.variables,
    (std::vector<PushConstants::Variable>{{.offset = 0, .size = 16, .source = push}})
  );
  EXPECT_EQ(draws[0]->dynamic.push_constants.size(), 16u);

  struct Block {
    u32 a, b, c, d;
  };
  compiled.set_bytes(push, Block{1, 2, 3, 4});
  EXPECT_EQ(std::get<Bytes>(compiled.variables[compiled.slots.at(push)].value).data[4], 2);
}

TEST(RenderGraphModule, ADispatchPicksUpTheComputePipelineAndTheConstantsInForce) {
  auto module = Module{};
  const auto scratch = module.transient_buffer({.size = 64, .location = MemoryLocation::GpuOnly});
  const auto end = module.begin_compute({{scratch, Access::ComputeWrite}})
                     .specialize_constant(0, true)
                     .bind_compute_pipeline(pipeline(7))
                     .specialize_constant(1, 5_u32)
                     .dispatch(1_u32, 1_u32, 1_u32)
                     .end_compute<1>()[0];
  const auto compiled = compile(module, end);
  const auto dispatches = find_all<ir::Dispatch>(compiled);
  ASSERT_EQ(dispatches.size(), 1u);
  EXPECT_EQ(dispatches[0]->pipeline, pipeline(7));
  EXPECT_EQ(dispatches[0]->specialization.get(0), SpecValue::from(true));
  EXPECT_EQ(dispatches[0]->specialization.get(1), SpecValue::from(5_u32));
}

TEST(RenderGraphModule, ADispatchPerPixelCoversTheImageItWasSizedFrom) {
  auto module = Module{};
  const auto image = module.transient_image(transient_info());
  const auto end = module.begin_compute({{image, Access::ComputeWrite}})
                     .bind_compute_pipeline(pipeline(0))
                     .dispatch_invocations_per_pixel(image)
                     .end_compute<1>()[0];
  const auto compiled = compile(module, end);
  const auto dispatches = find_all<ir::Dispatch>(compiled);
  ASSERT_EQ(dispatches.size(), 1u);
  EXPECT_EQ(dispatches[0]->size.kind, ir::DispatchSize::Kind::Invocations);
  EXPECT_EQ(std::get<ir::Constant>(module.get(dispatches[0]->size.x)).as_u32(), WIDTH);
  EXPECT_EQ(std::get<ir::Constant>(module.get(dispatches[0]->size.y)).as_u32(), HEIGHT);
}

TEST(RenderGraphModule, AnIndirectDispatchWaitsForTheBufferOfCounts) {
  auto module = Module{};
  const auto counts = module.transient_buffer({.size = 12, .location = MemoryLocation::GpuOnly});
  const auto written = module.begin_compute({{counts, Access::ComputeWrite}})
                         .bind_compute_pipeline(pipeline(0))
                         .dispatch(1_u32, 1_u32, 1_u32)
                         .end_compute<1>()[0];
  const auto end = module.begin_compute({{written, Access::IndirectRead}})
                     .bind_compute_pipeline(pipeline(1))
                     .dispatch_indirect(written)
                     .end_compute<1>()[0];
  const auto compiled = compile(module, end);
  const auto barriers = memory_barriers(module, compiled);
  EXPECT_TRUE(std::ranges::find(barriers, std::pair{Access::ComputeWrite, Access::IndirectRead}) != barriers.end())
    << compiled.dump();
  EXPECT_TRUE(buffer_usage(compiled, counts) & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
}

TEST(RenderGraphModule, AnIndirectDrawWaitsForTheDispatchThatFilledIt) {
  auto module = Module{};
  const auto attachment = module_with_attachment(module);
  const auto commands = module.transient_buffer({.size = 64, .location = MemoryLocation::GpuOnly});
  const auto written = module.begin_compute({{commands, Access::ComputeWrite}})
                         .bind_compute_pipeline(pipeline(0))
                         .dispatch(1_u32, 1_u32, 1_u32)
                         .end_compute<1>()[0];
  const auto end = module.begin_rendering({{attachment, Access::ColorRW}})
                     .bind_graphics_pipeline(pipeline(1))
                     .draw_indirect(written, 4_u32)
                     .end_rendering<1>()[0];
  const auto compiled = compile(module, end);
  const auto barriers = memory_barriers(module, compiled);
  EXPECT_TRUE(std::ranges::find(barriers, std::pair{Access::ComputeWrite, Access::IndirectRead}) != barriers.end())
    << compiled.dump();
}

TEST(RenderGraphModule, MeshTaskDrawsInheritThePipelineAndWaitOnTheirCommands) {
  auto module = Module{};
  const auto attachment = module_with_attachment(module);
  const auto commands = module.transient_buffer({.size = 64, .location = MemoryLocation::GpuOnly});
  const auto counts = module.transient_buffer({.size = 4, .location = MemoryLocation::GpuOnly});
  const auto [written_commands, written_counts] = module
                                                    .begin_compute(
                                                      {{commands, Access::ComputeWrite}, {counts, Access::ComputeWrite}}
                                                    )
                                                    .bind_compute_pipeline(pipeline(0))
                                                    .dispatch(1_u32, 1_u32, 1_u32)
                                                    .end_compute<2>();
  const auto end = module.begin_rendering({{attachment, Access::ColorRW}})
                     .bind_graphics_pipeline(pipeline(2))
                     .draw_mesh_tasks(1_u32, 1_u32, 1_u32)
                     .draw_mesh_tasks_indirect_count(written_commands, 0, written_counts, 0, 16_u32)
                     .end_rendering<1>()[0];
  const auto compiled = compile(module, end);
  ASSERT_EQ(find_all<ir::DrawMeshTasks>(compiled).size(), 1u);
  ASSERT_EQ(find_all<ir::DrawMeshTasksIndirect>(compiled).size(), 1u);
  EXPECT_EQ(find_all<ir::DrawMeshTasksIndirect>(compiled)[0]->pipeline, pipeline(2));
  EXPECT_TRUE(buffer_usage(compiled, counts) & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
  const auto barriers = memory_barriers(module, compiled);
  EXPECT_TRUE(std::ranges::find(barriers, std::pair{Access::ComputeWrite, Access::IndirectRead}) != barriers.end());
}

TEST(RenderGraphModule, TracedRaysTakeTheRayTracingPipelineAndItsPushes) {
  auto module = Module{};
  const auto image = module.transient_image(untyped_info());
  const auto end = module.begin_compute({{image, Access::RayTracingWrite}})
                     .bind_ray_tracing_pipeline(pipeline(4))
                     .push_constants(7_u32)
                     .trace_rays(WIDTH, HEIGHT, 1_u32)
                     .end_compute<1>()[0];
  const auto compiled = compile(module, end);
  const auto traces = find_all<ir::TraceRays>(compiled);
  ASSERT_EQ(traces.size(), 1u);
  EXPECT_EQ(traces[0]->pipeline, pipeline(4));
  EXPECT_EQ(traces[0]->push_constants.size(), 4u);
  const auto binds = find_all<ir::BindPipeline>(compiled);
  ASSERT_EQ(binds.size(), 1u);
  EXPECT_EQ(binds[0]->bind_point, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);
  EXPECT_EQ(image_barriers(module, compiled).front().new_layout, VK_IMAGE_LAYOUT_GENERAL);
}

TEST(RenderGraphModule, ADispatchThatWritesAnImagePutsItInGeneralWithStorageUsage) {
  auto module = Module{};
  const auto image = module.transient_image(untyped_info());
  const auto end = module.begin_compute({{image, Access::ComputeWrite}})
                     .bind_compute_pipeline(pipeline(0))
                     .dispatch(1_u32, 1_u32, 1_u32)
                     .end_compute<1>()[0];
  const auto compiled = compile(module, end);
  const auto barriers = image_barriers(module, compiled);
  ASSERT_EQ(barriers.size(), 1u);
  EXPECT_EQ(barriers[0].new_layout, VK_IMAGE_LAYOUT_GENERAL);
  EXPECT_EQ(image_usage(compiled, image), static_cast<vir::ImageUsageFlags>(vir::ImageUsageFlags::Storage));
}

TEST(RenderGraphModule, EveryDeclaredResourceGetsAFreshResultAfterThePass) {
  auto module = Module{};
  const auto first = module.transient_image(untyped_info());
  const auto second = module.transient_image(untyped_info());
  const auto [a, b] = module.begin_compute({{first, Access::ComputeWrite}, {second, Access::ComputeRead}})
                        .bind_compute_pipeline(pipeline(0))
                        .dispatch(1_u32, 1_u32, 1_u32)
                        .end_compute<2>();
  EXPECT_NE(a, first);
  EXPECT_NE(b, second);
  EXPECT_EQ(module.resource_root(a), first);
  EXPECT_EQ(module.resource_root(b), second);
}

TEST(RenderGraphModule, AResourceDeclaredTwiceCarriesBothAccesses) {
  auto module = Module{};
  const auto image = module.transient_image(untyped_info());
  module.begin_compute({{image, Access::ComputeRead}, {image, Access::ComputeWrite}})
    .bind_compute_pipeline(pipeline(0))
    .dispatch(1_u32, 1_u32, 1_u32);
  const auto results = module.end_compute_values();
  ASSERT_EQ(results.size(), 2u);
  const auto compiled = compile(module, results[0]);
  const auto begins = find_all<ir::BeginCompute>(compiled);
  ASSERT_EQ(begins.size(), 1u);
  ASSERT_EQ(begins[0]->attachments.size(), 1u);
  EXPECT_EQ(module.resolve_access(begins[0]->attachments[0].second), Access::ComputeRW);
}

TEST(RenderGraphModule, ACopyWaitsForTheHostWriteAndPutsTheImageInTheTransferLayout) {
  auto module = Module{};
  const auto staging = module.import_buffer(Buffer{}, Access::HostWrite);
  const auto texture = module.transient_image(untyped_info());
  const auto end = module.copy_buffer_to_image(staging, texture);

  const auto compiled = compile(module, end);
  const auto expected = std::vector<std::pair<Access, Access>>{{Access::HostWrite, Access::CopyRead}};
  EXPECT_EQ(memory_barriers(module, compiled), expected);
  const auto barriers = image_barriers(module, compiled);
  ASSERT_EQ(barriers.size(), 1u);
  EXPECT_EQ(barriers[0].resource, texture);
  EXPECT_EQ(barriers[0].dst, Access::CopyWrite);
  EXPECT_EQ(barriers[0].new_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
}

TEST(RenderGraphModule, AReadbackPutsTheImageInTheTransferSourceLayoutAndTheBufferWaitsForTheHost) {
  auto module = Module{};
  const auto target = module.transient_image(transient_info());
  const auto drawn = module.begin_rendering({{target, Access::ColorRW}})
                       .bind_graphics_pipeline(pipeline(0))
                       .draw(3_u32, 1_u32)
                       .end_rendering<1>()[0];
  const auto readback = module.import_buffer(Buffer{.size = 64}, Access::None);
  const auto copied = module.copy_image_to_buffer(drawn, readback);
  const auto end = module.export_to(copied, Access::HostRead, DomainFlag::Host);

  const auto compiled = compile(module, end);
  const auto barriers = image_barriers(module, compiled);
  ASSERT_EQ(barriers.size(), 2u) << compiled.dump();
  EXPECT_EQ(barriers[1].new_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  EXPECT_EQ(barriers[1].src, Access::ColorRW);
  const auto memory = memory_barriers(module, compiled);
  EXPECT_TRUE(std::ranges::find(memory, std::pair{Access::CopyWrite, Access::HostRead}) != memory.end())
    << compiled.dump();
  EXPECT_TRUE(vir::intersects(image_usage(compiled, target), vir::ImageUsageFlags::TransferSrc));
}

TEST(RenderGraphModule, BufferCopiesAndFillsAreOrderedAgainstTheirReaders) {
  auto module = Module{};
  const auto staging = module.import_buffer(Buffer{.size = 64}, Access::HostWrite);
  const auto target = module.transient_buffer({.size = 64, .location = MemoryLocation::GpuOnly});
  const auto filled = module.fill_buffer(target, 0);
  const auto copied = module.copy_buffer(staging, filled, {.size = 32});
  const auto end = module.begin_compute({{copied, Access::ComputeRead}})
                     .bind_compute_pipeline(pipeline(0))
                     .dispatch(1_u32, 1_u32, 1_u32)
                     .end_compute<1>()[0];

  const auto compiled = compile(module, end);
  const auto memory = memory_barriers(module, compiled);
  EXPECT_EQ(
    memory,
    (std::vector<std::pair<Access, Access>>{
      {Access::Clear | Access::HostWrite, Access::CopyRead | Access::CopyWrite},
      {Access::CopyWrite, Access::ComputeRead}
    })
  ) << compiled.dump();
  EXPECT_LT(position_of<ir::FillBuffer>(compiled), position_of<ir::MemoryBarrier>(compiled));
  EXPECT_LT(position_of<ir::MemoryBarrier>(compiled), position_of<ir::CopyBuffer>(compiled));
  const auto usage = buffer_usage(compiled, target);
  EXPECT_TRUE(usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  EXPECT_TRUE(usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
}

TEST(RenderGraphModule, PerMipPassesBarrierOnlyTheMipsTheyTouch) {
  auto module = Module{};
  auto info = untyped_info();
  info.mip_levels = 4;
  const auto pyramid = module.transient_image(info);

  auto current = pyramid;
  for (u32 level = 1; level < 4; level++) {
    const auto src = module.mip(current, level - 1);
    const auto dst = module.mip(current, level);
    current = module.begin_compute({{src, Access::ComputeSampled}, {dst, Access::ComputeWrite}})
                .bind_compute_pipeline(pipeline(0))
                .dispatch(1_u32, 1_u32, 1_u32)
                .end_compute<2>()[1];
  }

  const auto end = module.export_to(module.whole(current), Access::FragmentSampled);
  const auto compiled = compile(module, end);
  const auto barriers = image_barriers(module, compiled);
  ASSERT_FALSE(barriers.empty()) << compiled.dump();
  for (const auto& barrier : barriers) {
    EXPECT_EQ(barrier.resource, pyramid);
  }

  // the first pass reads mip 0 and writes mip 1, each its own range
  EXPECT_EQ(barriers[0].range, Subresource::mip(0));
  EXPECT_EQ(barriers[0].new_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  EXPECT_EQ(barriers[1].range, Subresource::mip(1));
  EXPECT_EQ(barriers[1].new_layout, VK_IMAGE_LAYOUT_GENERAL);

  // the third pass waits on the second's write of mip 2 and nothing else of it
  const auto read_of_two = std::ranges::find_if(barriers, [](const Barrier& barrier) {
    return barrier.range == Subresource::mip(2) && barrier.dst == Access::ComputeSampled;
  });
  ASSERT_NE(read_of_two, barriers.end()) << compiled.dump();
  EXPECT_EQ(read_of_two->src, Access::ComputeWrite);
  EXPECT_EQ(read_of_two->old_layout, VK_IMAGE_LAYOUT_GENERAL);

  // and the export brings every mip to the sampled layout
  auto exported = std::vector<Subresource>{};
  for (const auto& barrier : barriers) {
    if (barrier.dst == Access::FragmentSampled) {
      exported.push_back(barrier.range);
    }
  }

  EXPECT_FALSE(exported.empty()) << compiled.dump();
  EXPECT_EQ(
    image_usage(compiled, pyramid),
    static_cast<vir::ImageUsageFlags>(vir::ImageUsageFlags::Sampled | vir::ImageUsageFlags::Storage)
  );
}

TEST(RenderGraphModule, GeneratedMipsChainBlitsAndHandBackTheWholeImage) {
  auto module = Module{};
  auto info = untyped_info();
  info.mip_levels = 3;
  const auto texture = module.transient_image(info);
  const auto staging = module.import_buffer(Buffer{.size = 1024}, Access::HostWrite);
  const auto uploaded = module.copy_buffer_to_image(staging, module.mip(texture, 0));
  const auto mipped = module.generate_mips(uploaded, 0, 3);
  const auto end = module.export_to(mipped, Access::FragmentSampled);

  const auto compiled = compile(module, end);
  EXPECT_EQ(find_all<ir::Blit>(compiled).size(), 2u) << compiled.dump();
  EXPECT_EQ(module.resource_root(mipped), texture);
  EXPECT_TRUE(module.resource_range(mipped).is_whole());
  const auto usage = image_usage(compiled, texture);
  EXPECT_TRUE(vir::intersects(usage, vir::ImageUsageFlags::TransferSrc));
  EXPECT_TRUE(vir::intersects(usage, vir::ImageUsageFlags::TransferDst));
  EXPECT_TRUE(vir::intersects(usage, vir::ImageUsageFlags::Sampled));
}

TEST(RenderGraphModule, AKeptPassIsCompiledWithoutARootReachingIt) {
  auto module = Module{};
  const auto scratch = module.transient_buffer({.size = 64, .location = MemoryLocation::GpuOnly});
  const auto image = module.transient_image(untyped_info());
  const auto written = module.begin_compute({{scratch, Access::ComputeWrite}})
                         .bind_compute_pipeline(pipeline(0))
                         .dispatch(1_u32, 1_u32, 1_u32)
                         .end_compute<1>()[0];
  module.keep(written);
  const auto end = module.clear(image, clear::BLACK);
  const auto compiled = compile(module, end);
  EXPECT_EQ(find_all<ir::Dispatch>(compiled).size(), 1u);
}

TEST(RenderGraphModule, ASampledImageReachesTheShaderReadLayoutBeforeTheRegionOpens) {
  auto module = Module{};
  const auto attachment = module_with_attachment(module);
  const auto texture = module.transient_image(untyped_info());
  const auto end = module.begin_rendering({{attachment, Access::ColorRW}, {texture, Access::FragmentSampled}})
                     .bind_graphics_pipeline(pipeline(0))
                     .draw(3_u32, 1_u32)
                     .end_rendering<2>()[0];

  const auto compiled = compile(module, end);
  const auto barriers = image_barriers(module, compiled);
  const auto sampled = std::ranges::find(barriers, texture, &Barrier::resource);
  ASSERT_NE(sampled, barriers.end());
  EXPECT_EQ(sampled->dst, Access::FragmentSampled);
  EXPECT_EQ(sampled->new_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  auto last_barrier = 0_sz;
  for (usize index = 0; index < compiled.instructions.size(); index++) {
    if (!barrier_entries<ir::ImageBarrier>(compiled.instructions[index].ir).empty()) {
      last_barrier = index;
    }
  }

  EXPECT_LT(last_barrier, position_of<ir::BeginRendering>(compiled));
}

TEST(RenderGraphModule, AWrittenDescriptorIsTransitionedBeforeItsRegionOpensAndRecordsItsLayout) {
  auto module = Module{};
  const auto attachment = module_with_attachment(module);
  const auto texture = module.transient_image(untyped_info());
  const auto end = module.begin_rendering({{attachment, Access::ColorRW}})
                     .bind_graphics_pipeline(pipeline(0))
                     .bind_texture(0, 1, texture, a_sampler())
                     .draw(3_u32, 1_u32)
                     .end_rendering<1>()[0];

  const auto declared = Declared::fragment({{0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER}});
  const auto compiled = module.compile(declared.pipelines(), end);
  ASSERT_TRUE(compiled.has_value());
  const auto barriers = image_barriers(module, *compiled);
  const auto sampled = std::ranges::find(barriers, texture, &Barrier::resource);
  ASSERT_NE(sampled, barriers.end());
  EXPECT_EQ(sampled->dst, Access::FragmentSampled);
  EXPECT_EQ(sampled->new_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  const auto writes = find_all<ir::WriteDescriptor>(*compiled);
  ASSERT_EQ(writes.size(), 1u);
  EXPECT_EQ(writes[0]->descriptor_type, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
  EXPECT_EQ(writes[0]->image_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  EXPECT_EQ(image_usage(*compiled, texture), static_cast<vir::ImageUsageFlags>(vir::ImageUsageFlags::Sampled));
}

TEST(RenderGraphModule, AStorageDescriptorLandsInGeneralWithStorageUsage) {
  auto module = Module{};
  const auto attachment = module_with_attachment(module);
  const auto image = module.transient_image(untyped_info());
  const auto end = module.begin_rendering({{attachment, Access::ColorRW}})
                     .bind_graphics_pipeline(pipeline(0))
                     .bind_image(0, 0, image)
                     .draw(3_u32, 1_u32)
                     .end_rendering<1>()[0];

  const auto declared = Declared::with_access(
    {{0, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}},
    vir::ShaderStage::Fragment,
    Access::FragmentWrite
  );
  const auto compiled = module.compile(declared.pipelines(), end);
  ASSERT_TRUE(compiled.has_value());
  const auto barriers = image_barriers(module, *compiled);
  const auto stored = std::ranges::find(barriers, image, &Barrier::resource);
  ASSERT_NE(stored, barriers.end());
  EXPECT_EQ(stored->dst, Access::FragmentWrite);
  EXPECT_EQ(stored->new_layout, VK_IMAGE_LAYOUT_GENERAL);
  EXPECT_EQ(image_usage(*compiled, image), static_cast<vir::ImageUsageFlags>(vir::ImageUsageFlags::Storage));
}

TEST(RenderGraphModule, ADescriptorAndAnAttachmentOverTheSameImageTransitionOnce) {
  auto module = Module{};
  const auto image = module.transient_image(untyped_info());
  const auto end = module.begin_compute({{image, Access::ComputeWrite}})
                     .bind_compute_pipeline(pipeline(0))
                     .bind_image(0, 0, image)
                     .dispatch(1_u32, 1_u32, 1_u32)
                     .end_compute<1>()[0];

  const auto declared = Declared::with_access(
    {{0, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}},
    vir::ShaderStage::Compute,
    Access::ComputeRW
  );
  const auto compiled = module.compile(declared.pipelines(), end);
  ASSERT_TRUE(compiled.has_value());
  const auto barriers = image_barriers(module, *compiled);
  ASSERT_EQ(barriers.size(), 1u) << compiled->dump();
  EXPECT_EQ(barriers[0].dst, Access::ComputeRW);
}

TEST(RenderGraphModule, ADescriptorWrittenWithoutAPipelineThatReadsItIsRejected) {
  auto module = Module{};
  const auto attachment = module_with_attachment(module);
  const auto end = module.begin_rendering({{attachment, Access::ColorRW}})
                     .bind_graphics_pipeline(pipeline(0))
                     .draw(3_u32, 1_u32)
                     .end_rendering<1>()[0];

  const auto declared = Declared::fragment({{0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE}});
  EXPECT_FALSE(module.compile(declared.pipelines(), end).has_value());
}

TEST(RenderGraphModule, BlendStateIsPerAttachmentAndKeysThePipeline) {
  auto module = Module{};
  const auto first = module.transient_image(transient_info());
  const auto second = module.transient_image(transient_info());
  const auto end = module.begin_rendering({{first, Access::ColorRW}, {second, Access::ColorRW}})
                     .bind_graphics_pipeline(pipeline(0))
                     .set_color_blend(1, BlendPreset::Additive)
                     .draw(3_u32, 1_u32)
                     .end_rendering<2>()[0];
  const auto compiled = compile(module, end);
  const auto draws = find_all<ir::Draw>(compiled);
  ASSERT_EQ(draws.size(), 1u);
  ASSERT_EQ(draws[0]->state.blend.size(), 2u);
  EXPECT_FALSE(draws[0]->state.blend[0].is_blend_enabled);
  EXPECT_TRUE(draws[0]->state.blend[1].is_blend_enabled);
}

TEST(RenderGraphModule, AnAttachmentlessPassTakesTheAreaAndLayersItNames) {
  auto module = Module{};
  const auto pages = module.transient_image(untyped_info());
  const auto end = module.begin_rendering_area({{pages, Access::FragmentWrite}}, {128, 128}, 4)
                     .bind_graphics_pipeline(pipeline(0))
                     .draw(3_u32, 1_u32)
                     .end_rendering<1>()[0];
  const auto compiled = compile(module, end);
  const auto begins = find_all<ir::BeginRendering>(compiled);
  ASSERT_EQ(begins.size(), 1u);
  EXPECT_EQ(begins[0]->layer_count, 4u);
  const auto draws = find_all<ir::Draw>(compiled);
  ASSERT_EQ(draws.size(), 1u);
  EXPECT_TRUE(draws[0]->state.rendering.color_formats.empty());
}

TEST(RenderGraphModule, AClearOnlyRenderedIntoBecomesTheLoadOp) {
  auto module = Module{};
  const auto target = module.transient_image(transient_info());
  const auto cleared = module.clear(target, clear::BLACK);
  const auto end = module.begin_rendering({{cleared, Access::ColorRW}})
                     .bind_graphics_pipeline(pipeline(0))
                     .draw(3_u32, 1_u32)
                     .end_rendering<1>()[0];
  const auto compiled = compile(module, end);
  EXPECT_TRUE(find_all<ir::Clear>(compiled).empty()) << compiled.dump();
  const auto begins = find_all<ir::BeginRendering>(compiled);
  ASSERT_EQ(begins.size(), 1u);
  EXPECT_EQ(begins[0]->clears.size(), 1u);
}

struct ConditionalClear {
  Module module = {};
  ValueID source = ValueID::Invalid;
  ValueID end = ValueID::Invalid;

  ConditionalClear() {
    const auto destination = module_with_attachment(module);
    source = module.transient_image(transient_info());
    const auto animate = module.declare_bool_var("animate", true);
    const auto cleared = module.set_condition(
      animate,
      [&](Module& m) { return m.clear(source, clear::WHITE); },
      [&](Module&) { return source; }
    );
    end = module.blit(cleared, destination);
  }
};

static auto label_position(const Program& program, LabelID label) -> usize {
  for (usize index = 0; index < program.instructions.size(); index++) {
    if (
      const auto* placed = std::get_if<ir::Label>(&program.instructions[index].ir); placed && placed->label == label
    ) {
      return index;
    }
  }

  return ~0_sz;
}

TEST(RenderGraphSelection, EveryBlockEndsInATerminator) {
  auto conditional = ConditionalClear{};
  const auto compiled = compile(conditional.module, conditional.end);
  auto is_open = false;
  for (const auto& instruction : compiled.instructions) {
    if (std::holds_alternative<ir::Label>(instruction.ir)) {
      EXPECT_FALSE(is_open) << compiled.dump();
      is_open = true;
    } else if (is_terminator(instruction.ir)) {
      is_open = false;
    }
  }

  EXPECT_FALSE(is_open);
}

TEST(RenderGraphSelection, BothArmsOfASelectionLeaveAResourceInTheSameLayout) {
  auto conditional = ConditionalClear{};
  const auto& module = conditional.module;
  const auto compiled = compile(module, conditional.end);
  const auto* branch = find_all<ir::BranchConditional>(compiled).front();
  const auto merge = find_all<ir::SelectionMerge>(compiled).front()->merge;
  const auto taken = label_position(compiled, branch->true_label);
  const auto skipped = label_position(compiled, branch->false_label);
  const auto joined = label_position(compiled, merge);
  ASSERT_LT(taken, skipped);
  ASSERT_LT(skipped, joined);

  const auto layout_leaving = [&](usize begin, usize end) -> option<VkImageLayout> {
    auto layout = option<VkImageLayout>{};
    for (auto index = begin; index < end; index++) {
      for (const auto& barrier : barrier_entries<ir::ImageBarrier>(compiled.instructions[index].ir)) {
        if (module.resource_root(barrier.value) == conditional.source) {
          layout = barrier.new_layout;
        }
      }
    }

    return layout;
  };

  const auto cleared = layout_leaving(taken, skipped);
  ASSERT_TRUE(cleared.has_value()) << compiled.dump();
  EXPECT_EQ(*cleared, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  const auto untouched = layout_leaving(skipped, joined);
  ASSERT_TRUE(untouched.has_value()) << compiled.dump();
  EXPECT_EQ(*untouched, *cleared);
}

TEST(RenderGraphSelection, AnArmThatNeverReachedABufferWaitsOnNothingAtTheJoin) {
  auto module = Module{};
  const auto buffer = module.transient_buffer({.size = 256, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT});
  const auto target = module.transient_image(transient_info());
  const auto enabled = module.declare_bool_var("enabled", true);
  const auto end = module.set_condition(
    enabled,
    [&](Module& m) {
      return m.begin_compute({{target, Access::ComputeWrite}, {buffer, Access::ComputeRead}})
        .bind_compute_pipeline(pipeline(0))
        .dispatch(1_u32, 1_u32, 1_u32)
        .end_compute<2>()[0];
    },
    [&](Module&) { return target; }
  );

  const auto compiled = compile(module, end);
  EXPECT_TRUE(memory_barriers(module, compiled).empty()) << compiled.dump();
}

TEST(RenderGraphSelection, ConditionalsMergePassResultArraysPositionByPosition) {
  auto module = Module{};
  const auto first = module.transient_image(untyped_info());
  const auto second = module.transient_image(untyped_info());
  const auto enabled = module.declare_bool_var("enabled", false);
  const auto merged = module.set_condition(
    enabled,
    [&](Module& m) {
      return m.begin_compute({{first, Access::ComputeWrite}, {second, Access::ComputeWrite}})
        .bind_compute_pipeline(pipeline(0))
        .dispatch(1_u32, 1_u32, 1_u32)
        .end_compute<2>();
    },
    [&](Module&) { return std::array{first, second}; }
  );
  EXPECT_EQ(module.resource_root(merged[0]), first);
  EXPECT_EQ(module.resource_root(merged[1]), second);
}

TEST(RenderGraphProgram, SettingAVariableLeavesTheCompiledInstructionsAlone) {
  auto module = Module{};
  const auto count = module.declare_u32_var("count", 1);
  const auto scratch = module.transient_buffer({.size = 64});
  const auto end = module.begin_compute({{scratch, Access::ComputeWrite}})
                     .bind_compute_pipeline(pipeline(0))
                     .dispatch(count, 1_u32, 1_u32)
                     .end_compute<1>()[0];
  auto compiled = compile(module, end);
  const auto before = compiled.dump();
  compiled.set(count, 9_u32);
  EXPECT_EQ(before, compiled.dump());
  EXPECT_EQ(std::get<u32>(compiled.variables[compiled.slots.at(count)].value), 9u);
}

TEST(RenderGraphProgram, TheDumpNamesEveryInstruction) {
  auto conditional = ConditionalClear{};
  const auto compiled = compile(conditional.module, conditional.end);
  const auto text = compiled.dump();
  EXPECT_NE(text.find("barrier.image"), std::string::npos);
  EXPECT_NE(text.find("branch_cond"), std::string::npos);
  EXPECT_NE(text.find("blit"), std::string::npos);
}

} // namespace ox
