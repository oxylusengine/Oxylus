#include <memory>

#include "Render/Graph/Allocator.hpp"
#include "Render/Graph/Types.hpp"
#include "RenderGraphTest.hpp"

namespace ox {
TEST(RenderGraphExtensions, ViewFormatListsKeepIncompatibleCachedImagesSeparate) {
  auto cache = FrameImageCache{};
  auto info = untyped_info();
  info.format = vir::Format::R32Sfloat;
  info.view_formats = {vir::Format::R32Sfloat};
  u32 created = 0;
  const auto create = [&]() -> std::expected<Image, VkResult> {
    created += 1;
    return Image::from_info(reinterpret_cast<VkImage>(u64(created)), info);
  };
  const auto original_key = ImageCacheKey::from(info);
  const auto original = cache.acquire(info, 1, create);
  ASSERT_TRUE(original.has_value());
  cache.retire(2);
  info.view_formats.push_back(vir::Format::R32Uint);
  EXPECT_FALSE(original_key == ImageCacheKey::from(info));
  const auto overridden = cache.acquire(info, 2, create);
  ASSERT_TRUE(overridden.has_value());
  EXPECT_NE(original->handle, overridden->handle);
  EXPECT_EQ(created, 2);
  cache.retire(3);
  const auto reused = cache.acquire(info, 3, create);
  ASSERT_TRUE(reused.has_value());
  EXPECT_EQ(reused->handle, overridden->handle);
  EXPECT_EQ(created, 2);
}

TEST(RenderGraphExtensions, DisjointLayersKeepIndependentStates) {
  auto module = Module{};
  auto info = untyped_info();
  info.array_layers = 3;
  const auto image = module.transient_image(info);
  const auto first = module.view(image, Subresource::layer(0));
  const auto written = module.begin_compute({{first, Access::ComputeWrite}})
                         .bind_compute_pipeline(pipeline(0))
                         .dispatch(1_u32, 1_u32, 1_u32)
                         .end_compute<1>()[0];
  const auto second = module.view(written, Subresource::layer(2));
  const auto end = module.begin_compute({{second, Access::ComputeRead}})
                     .bind_compute_pipeline(pipeline(0))
                     .dispatch(1_u32, 1_u32, 1_u32)
                     .end_compute<1>()[0];
  const auto compiled = compile(module, end);
  const auto barriers = image_barriers(module, compiled);
  ASSERT_EQ(barriers.size(), 2u) << compiled.dump();
  EXPECT_EQ(barriers[0].range.base_layer, 0u);
  EXPECT_EQ(barriers[0].range.layer_count, 1u);
  EXPECT_EQ(barriers[1].range.base_layer, 2u);
  EXPECT_EQ(barriers[1].range.layer_count, 1u);
  EXPECT_EQ(barriers[1].src, Access::None);
  EXPECT_EQ(barriers[1].old_layout, VK_IMAGE_LAYOUT_UNDEFINED);
}

TEST(RenderGraphExtensions, OverlappingMipLayerViewsWaitOnlyOnTheirIntersection) {
  auto module = Module{};
  auto info = untyped_info();
  info.mip_levels = 4;
  info.array_layers = 3;
  const auto image = module.transient_image(info);
  const auto first_range = Subresource{.base_level = 1, .level_count = 2, .base_layer = 1, .layer_count = 1};
  const auto first = module.view(image, first_range);
  const auto written = module.begin_compute({{first, Access::ComputeWrite}})
                         .bind_compute_pipeline(pipeline(0))
                         .dispatch(1_u32, 1_u32, 1_u32)
                         .end_compute<1>()[0];
  const auto second = module.view(written, {.base_level = 2, .level_count = 2, .base_layer = 1, .layer_count = 1});
  const auto end = module.begin_compute({{second, Access::ComputeRead}})
                     .bind_compute_pipeline(pipeline(0))
                     .dispatch(1_u32, 1_u32, 1_u32)
                     .end_compute<1>()[0];
  const auto compiled = compile(module, end);
  const auto barriers = image_barriers(module, compiled);
  auto waited = false;
  auto fresh = false;
  for (const auto& barrier : barriers) {
    if (barrier.dst != Access::ComputeRead)
      continue;
    EXPECT_EQ(barrier.range.base_layer, 1u);
    EXPECT_EQ(barrier.range.layer_count, 1u);
    EXPECT_EQ(barrier.range.level_count, 1u);
    if (barrier.range.base_level == 2) {
      EXPECT_EQ(barrier.src, Access::ComputeWrite);
      waited = true;
    } else if (barrier.range.base_level == 3) {
      EXPECT_EQ(barrier.src, Access::None);
      fresh = true;
    } else
      ADD_FAILURE() << compiled.dump();
  }
  EXPECT_TRUE(waited);
  EXPECT_TRUE(fresh);
}

TEST(RenderGraphExtensions, WholeImageExportRestoresEveryMipAndLayerAfterPartialWrites) {
  auto module = Module{};
  auto info = untyped_info();
  info.mip_levels = 3;
  info.array_layers = 2;
  const auto image = module.transient_image(info);
  const auto range = module.view(image, {.base_level = 1, .level_count = 1, .base_layer = 1, .layer_count = 1});
  const auto written = module.begin_compute({{range, Access::ComputeWrite}})
                         .bind_compute_pipeline(pipeline(0))
                         .dispatch(1_u32, 1_u32, 1_u32)
                         .end_compute<1>()[0];
  const auto end = module.export_to(module.whole(written), Access::FragmentSampled);
  const auto compiled = compile(module, end);
  const auto barriers = image_barriers(module, compiled);
  auto covered = std::array<std::array<bool, 2>, 3>{};
  for (const auto& barrier : barriers) {
    if (barrier.dst != Access::FragmentSampled)
      continue;
    EXPECT_EQ(barrier.new_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const auto range_applied = barrier.range.apply(info.subresource_range());
    for (u32 level = range_applied.baseMipLevel; level < range_applied.baseMipLevel + range_applied.levelCount;
         level++) {
      for (u32 layer = range_applied.baseArrayLayer; layer < range_applied.baseArrayLayer + range_applied.layerCount;
           layer++) {
        ASSERT_LT(level, covered.size());
        ASSERT_LT(layer, covered[level].size());
        EXPECT_FALSE(covered[level][layer]);
        covered[level][layer] = true;
      }
    }
  }
  for (const auto& level : covered)
    for (const auto layer : level)
      EXPECT_TRUE(layer) << compiled.dump();
}

TEST(RenderGraphExtensions, ExplicitImageViewFormatsResolvePerBindingButShareSynchronization) {
  auto module = Module{};
  auto info = untyped_info();
  info.format = vir::Format::R32Sfloat;
  info.flags = vir::ImageCreateFlags::MutableFormat;
  info.view_formats = {vir::Format::R32Uint};
  const auto image = module.transient_image(info);
  const auto floating = module.view(image, Subresource::whole(), vir::Format::R32Sfloat);
  const auto integer = module.view(image, Subresource::whole(), vir::Format::R32Uint);
  const auto end = module.begin_compute({{image, Access::ComputeRead}})
                     .bind_compute_pipeline(pipeline(0))
                     .bind_image(0, 0, floating)
                     .bind_image(0, 1, integer)
                     .dispatch(1_u32, 1_u32, 1_u32)
                     .end_compute<1>()[0];
  const auto declared = Declared::with_access(
    {{0, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}},
    vir::ShaderStage::Compute,
    Access::ComputeRead
  );
  const auto compiled = module.compile(declared.pipelines(), end);
  ASSERT_TRUE(compiled.has_value());
  const auto views = find_all<ir::View>(*compiled);
  ASSERT_EQ(views.size(), 2u);
  EXPECT_EQ(
    std::ranges::count_if(views, [](const ir::View* view) -> bool { return view->format == vir::Format::R32Sfloat; }),
    1
  );
  EXPECT_EQ(
    std::ranges::count_if(views, [](const ir::View* view) -> bool { return view->format == vir::Format::R32Uint; }),
    1
  );
  const auto writes = find_all<ir::WriteDescriptor>(*compiled);
  ASSERT_EQ(writes.size(), 2u);
  EXPECT_EQ(writes[0]->descriptor.image, floating);
  EXPECT_EQ(writes[1]->descriptor.image, integer);
  EXPECT_EQ(writes[0]->image_layout, VK_IMAGE_LAYOUT_GENERAL);
  EXPECT_EQ(writes[1]->image_layout, VK_IMAGE_LAYOUT_GENERAL);
  EXPECT_EQ(image_barriers(module, *compiled).size(), 1u) << compiled->dump();
}

TEST(RenderGraphExtensions, TransferRegionsPreserveOffsetsAndSyncTheNamedView) {
  for (const auto readback : {false, true}) {
    auto module = Module{};
    auto info = untyped_info();
    info.mip_levels = 3;
    info.array_layers = 2;
    const auto image = module.transient_image(info);
    const auto view = module.view(image, {.base_level = 1, .level_count = 1, .base_layer = 1, .layer_count = 1});
    const auto buffer = module.declare_buffer_var("staging", readback ? Access::None : Access::HostWrite);
    const auto region = BufferImageCopy{
      .buffer_offset = 128,
      .buffer_row_length = 32,
      .buffer_image_height = 16,
      .image_offset = {2, 3, 0},
      .image_extent = {8, 4, 1},
      .layer_count = 1
    };
    const auto end = readback ? module.copy_image_to_buffer_region(view, buffer, region)
                              : module.copy_buffer_to_image_region(buffer, view, region);
    const auto compiled = compile(module, end);
    const auto barriers = image_barriers(module, compiled);
    ASSERT_EQ(barriers.size(), 1u);
    EXPECT_EQ(barriers[0].range, module.resource_range(view));
    EXPECT_EQ(barriers[0].dst, readback ? Access::CopyRead : Access::CopyWrite);
    if (readback) {
      const auto copies = find_all<ir::CopyImageToBuffer>(compiled);
      ASSERT_EQ(copies.size(), 1u);
      EXPECT_EQ(copies[0]->region, region);
    } else {
      const auto copies = find_all<ir::CopyBufferToImage>(compiled);
      ASSERT_EQ(copies.size(), 1u);
      EXPECT_EQ(copies[0]->region, region);
    }
  }
}

TEST(RenderGraphExtensions, EmptyTransferRegionsAreDropped) {
  auto module = Module{};
  const auto image = module.transient_image(untyped_info());
  const auto buffer = module.transient_buffer({.size = 64});
  const auto before = module.instructions.size();
  EXPECT_EQ(module.copy_buffer_to_image_region(buffer, image, {}), image);
  EXPECT_EQ(module.copy_image_to_buffer_region(image, buffer, {}), buffer);
  EXPECT_EQ(module.instructions.size(), before);
}

TEST(RenderGraphExtensions, ImageCopiesInferBothTransferUsagesAndLayouts) {
  auto module = Module{};
  const auto source = module.transient_image(untyped_info());
  const auto destination = module.transient_image(untyped_info());
  const auto compiled = compile(module, module.copy_image(source, destination));
  EXPECT_EQ(image_usage(compiled, source), vir::ImageUsageFlags::TransferSrc);
  EXPECT_EQ(image_usage(compiled, destination), vir::ImageUsageFlags::TransferDst);
  const auto barriers = image_barriers(module, compiled);
  ASSERT_EQ(barriers.size(), 2u);
  EXPECT_EQ(barriers[0].new_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  EXPECT_EQ(barriers[1].new_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
}

TEST(RenderGraphExtensions, BufferCopyAndFillRegionsSurviveCompilation) {
  auto module = Module{};
  const auto source = module.declare_buffer_var("source", Access::HostWrite);
  const auto destination = module.transient_buffer({.size = 512});
  const auto filled = module.fill_buffer(destination, 0xaabbccdd, 16, 64);
  const auto copied = module.copy_buffer(source, filled, {.src_offset = 32, .dst_offset = 128, .size = 80});
  const auto compiled = compile(module, copied);
  const auto fills = find_all<ir::FillBuffer>(compiled);
  const auto copies = find_all<ir::CopyBuffer>(compiled);
  ASSERT_EQ(fills.size(), 1u);
  ASSERT_EQ(copies.size(), 1u);
  EXPECT_EQ(fills[0]->data, 0xaabbccddu);
  EXPECT_EQ(fills[0]->offset, 16u);
  EXPECT_EQ(fills[0]->size, 64u);
  EXPECT_EQ(copies[0]->region, (BufferCopy{.src_offset = 32, .dst_offset = 128, .size = 80}));
  EXPECT_EQ(buffer_usage(compiled, destination), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
}

TEST(RenderGraphExtensions, GeneratingMipsWithNoLowerLevelsAddsNoBlit) {
  auto module = Module{};
  const auto image = module.transient_image(untyped_info());
  const auto compiled = compile(module, module.generate_mips(image, 0, 1));
  EXPECT_TRUE(find_all<ir::Blit>(compiled).empty());
  EXPECT_EQ(module.resource_root(image), image);
}

TEST(RenderGraphExtensions, MeshIndirectCountDrawsWaitOnCommandsAndCounts) {
  auto module = Module{};
  const auto target = module.transient_image(transient_info());
  const auto commands = module.declare_buffer_var("commands", Access::HostWrite);
  const auto counts = module.declare_buffer_var("counts", Access::ComputeWrite);
  const auto end = module.begin_rendering({{target, Access::ColorRW}})
                     .bind_graphics_pipeline(pipeline(2))
                     .draw_mesh_tasks_indirect_count(commands, 32, counts, 8, 7_u32)
                     .end_rendering<1>()[0];
  const auto compiled = compile(module, end);
  const auto draws = find_all<ir::DrawMeshTasksIndirect>(compiled);
  ASSERT_EQ(draws.size(), 1u);
  EXPECT_EQ(draws[0]->pipeline, pipeline(2));
  EXPECT_EQ(draws[0]->count.buffer, counts);
  EXPECT_EQ(draws[0]->count.offset, 8u);
  EXPECT_EQ(draws[0]->offset, 32u);
  EXPECT_EQ(draws[0]->stride, sizeof(VkDrawMeshTasksIndirectCommandEXT));
  const auto barriers = memory_barriers(module, compiled);
  ASSERT_EQ(barriers.size(), 1u) << compiled.dump();
  EXPECT_EQ(barriers[0], (std::pair{Access::HostWrite | Access::ComputeWrite, Access::IndirectRead}));
}

TEST(RenderGraphExtensions, AccelerationStructureBuildsAreVariablesAndSynchronizeSymbolicBuffers) {
  auto module = Module{};
  const auto geometry = module.declare_buffer_var("geometry", Access::HostWrite);
  const auto backing = module.transient_buffer({.size = 4096});
  const auto builds = module.declare_acceleration_structure_builds_var("builds");
  const auto addresses = std::array{AccelerationStructureAddress{.buffer = geometry}};
  const auto resources = std::array{ResourceAccess{backing, Access::AccelerationStructureBuildWrite}};
  const auto built = module.build_acceleration_structures(builds, addresses, resources);
  const auto end = module.export_to(built[0], Access::ComputeAccelerationStructureRead);
  auto compiled = compile(module, end);
  const auto nodes = find_all<ir::BuildAccelerationStructures>(compiled);
  ASSERT_EQ(nodes.size(), 1u);
  EXPECT_EQ(nodes[0]->builds, builds);
  EXPECT_TRUE(compiled.has_variable(builds));
  const auto supplied = std::make_shared<const std::vector<AccelerationStructureBuild>>(
    std::vector<AccelerationStructureBuild>{AccelerationStructureBuild{}}
  );
  compiled.set(builds, supplied);
  EXPECT_EQ(std::get<AccelerationStructureBuilds>(compiled.variables[compiled.slots.at(builds)].value), supplied);
  const auto barriers = memory_barriers(module, compiled);
  EXPECT_NE(
    std::ranges::find(barriers, std::pair{Access::HostWrite, Access::AccelerationStructureBuildRead}),
    barriers.end()
  );
  EXPECT_NE(
    std::ranges::find(
      barriers,
      std::pair{Access::AccelerationStructureBuildWrite, Access::ComputeAccelerationStructureRead}
    ),
    barriers.end()
  );
}

TEST(RenderGraphExtensions, RasterizationStencilAndTessellationStateReachThePipelineKey) {
  auto module = Module{};
  auto info = untyped_info();
  info.format = vir::Format::D24UnormS8Uint;
  const auto target = module.transient_image(info);
  const auto rasterization = RasterizationState{
    .is_depth_clamp_enabled = true,
    .is_depth_bias_enabled = true,
    .depth_bias_constant = 1.25f,
    .depth_bias_clamp = 0.5f,
    .depth_bias_slope = 2.0f
  };
  const auto stencil = StencilState{
    .is_test_enabled = true,
    .front =
      {.failOp = VK_STENCIL_OP_KEEP,
       .passOp = VK_STENCIL_OP_REPLACE,
       .depthFailOp = VK_STENCIL_OP_KEEP,
       .compareOp = VK_COMPARE_OP_ALWAYS,
       .compareMask = 0xff,
       .writeMask = 0xff,
       .reference = 3},
    .back = {
      .failOp = VK_STENCIL_OP_KEEP,
      .passOp = VK_STENCIL_OP_KEEP,
      .depthFailOp = VK_STENCIL_OP_KEEP,
      .compareOp = VK_COMPARE_OP_EQUAL,
      .compareMask = 0xff,
      .writeMask = 0,
      .reference = 0
    }
  };
  const auto end = module.begin_rendering({{target, Access::DepthStencilRW}})
                     .bind_graphics_pipeline(pipeline(0))
                     .set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_PATCH_LIST)
                     .set_patch_control_points(4)
                     .set_rasterization(rasterization)
                     .set_stencil(stencil)
                     .draw(4_u32, 1_u32)
                     .end_rendering<1>()[0];
  const auto compiled = compile(module, end);
  const auto draws = find_all<ir::Draw>(compiled);
  ASSERT_EQ(draws.size(), 1u);
  EXPECT_EQ(draws[0]->state.patch_control_points, 4u);
  EXPECT_EQ(draws[0]->state.rasterization, rasterization);
  EXPECT_EQ(draws[0]->state.stencil, stencil);
  EXPECT_EQ(draws[0]->state.rendering.stencil_format, info.format);
  auto changed = draws[0]->state;
  changed.stencil.front.reference = 9;
  EXPECT_NE(changed, draws[0]->state);
  EXPECT_NE(changed.hash(), draws[0]->state.hash());
}

TEST(RenderGraphExtensions, MultipleVertexBuffersInferUsageAndShareTheirHostBarrier) {
  auto module = Module{};
  const auto target = module.transient_image(transient_info());
  const auto vertices = module.declare_buffer_var("vertices", Access::HostWrite);
  const auto instances = module.declare_buffer_var("instances", Access::HostWrite);
  const auto buffers = std::array{vertices, instances};
  const auto offsets = std::array<u64, 2>{16, 64};
  const auto end = module.begin_rendering({{target, Access::ColorRW}})
                     .bind_graphics_pipeline(pipeline(0))
                     .bind_vertex_buffers(0, buffers, offsets)
                     .draw(3_u32, 4_u32)
                     .end_rendering<1>()[0];
  const auto compiled = compile(module, end);
  EXPECT_EQ(find_all<ir::BindVertexBuffers>(compiled).size(), 1u);
  EXPECT_EQ(buffer_usage(compiled, vertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
  EXPECT_EQ(buffer_usage(compiled, instances), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
  EXPECT_EQ(memory_barriers(module, compiled).size(), 1u);
}
} // namespace ox
