#include "Render/Graph/Types.hpp"
#include "RenderGraphTest.hpp"

namespace ox {
static auto draw_into(Module& module, ValueID target) -> ValueID {
  return module.begin_rendering({{target, Access::ColorRW}})
    .bind_graphics_pipeline(pipeline(0))
    .draw(3_u32, 1_u32)
    .end_rendering<1>()[0];
}

template <typename T>
static auto positions(const Program& program) -> std::vector<usize> {
  auto result = std::vector<usize>{};
  for (usize index = 0; index < program.instructions.size(); index++) {
    if (std::holds_alternative<T>(program.instructions[index].ir)) {
      result.push_back(index);
    }
  }
  return result;
}

TEST(RenderGraphOpt, UnrelatedClearsGatherTheirBarriersBeforeTheFirstClear) {
  auto module = Module{};
  const auto output = module.transient_image(transient_info());
  const auto first = module.clear(module.transient_image(transient_info()), clear::BLACK);
  const auto second = module.clear(module.transient_image(transient_info()), clear::BLACK);
  const auto third = module.clear(module.transient_image(transient_info()), clear::BLACK);
  const auto end = module
                     .begin_rendering(
                       {{output, Access::ColorRW},
                        {first, Access::FragmentSampled},
                        {second, Access::FragmentSampled},
                        {third, Access::FragmentSampled}}
                     )
                     .bind_graphics_pipeline(pipeline(0))
                     .draw(3_u32, 1_u32)
                     .end_rendering<4>()[0];
  const auto compiled = compile(module, end);
  const auto clears = positions<ir::Clear>(compiled);
  const auto barriers = positions<ir::ImageBarrier>(compiled);
  ASSERT_EQ(clears.size(), 3u) << compiled.dump();
  ASSERT_GE(barriers.size(), 5u) << compiled.dump();
  EXPECT_EQ(clears[1], clears[0] + 1);
  EXPECT_EQ(clears[2], clears[0] + 2);
  for (usize index = 0; index < 4; index++) {
    EXPECT_EQ(barriers[index], clears[0] - 4 + index) << compiled.dump();
  }
  EXPECT_GT(barriers[4], clears[2]);
}

TEST(RenderGraphOpt, ABarrierDoesNotClimbAboveAPass) {
  auto module = Module{};
  const auto first = module.transient_image(transient_info());
  const auto second = module.transient_image(transient_info());
  const auto drawn = draw_into(module, first);
  const auto end = module.begin_rendering({{second, Access::ColorRW}, {drawn, Access::ColorRW}})
                     .bind_graphics_pipeline(pipeline(0))
                     .draw(3_u32, 1_u32)
                     .end_rendering<2>()[1];
  const auto compiled = compile(module, end);
  const auto first_end = position_of<ir::EndRendering>(compiled);
  auto found = false;
  for (usize index = 0; index < compiled.instructions.size(); index++) {
    for (const auto& barrier : barrier_entries<ir::ImageBarrier>(compiled.instructions[index].ir)) {
      if (module.resource_root(barrier.value) == second) {
        EXPECT_GT(index, first_end) << compiled.dump();
        found = true;
      }
    }
  }
  EXPECT_TRUE(found);
}

TEST(RenderGraphOpt, AMemoryBarrierDoesNotClimbAboveAnUnrelatedBufferUse) {
  auto module = Module{};
  const auto target = module.transient_image(transient_info());
  const auto staging = module.declare_buffer_var("staging", Access::HostWrite);
  const auto vertices = module.declare_buffer_var("vertices", Access::HostWrite);
  const auto copied = module.copy_buffer_to_image(staging, target);
  const auto end = module.begin_rendering({{copied, Access::ColorRW}})
                     .bind_graphics_pipeline(pipeline(0))
                     .bind_vertex_buffer(0, vertices)
                     .draw(3_u32, 1_u32)
                     .end_rendering<1>()[0];
  const auto compiled = compile(module, end);
  const auto waits = positions<ir::MemoryBarrier>(compiled);
  ASSERT_EQ(waits.size(), 2u) << compiled.dump();
  const auto copy = position_of<ir::CopyBufferToImage>(compiled);
  EXPECT_LT(waits[0], copy);
  EXPECT_LT(copy, waits[1]);
  EXPECT_EQ(
    memory_barriers(module, compiled),
    (std::vector<std::pair<Access, Access>>{
      {Access::HostWrite, Access::CopyRead},
      {Access::HostWrite, Access::AttributeRead}
    })
  );
}

TEST(RenderGraphOpt, FoldedColorClearBecomesALoadOpWithNoTransferUsage) {
  auto module = Module{};
  const auto target = module.transient_image(untyped_info());
  const auto end = draw_into(module, module.clear(target, clear::BLACK));
  const auto compiled = compile(module, end);
  EXPECT_TRUE(find_all<ir::Clear>(compiled).empty());
  const auto begins = find_all<ir::BeginRendering>(compiled);
  ASSERT_EQ(begins.size(), 1u);
  ASSERT_EQ(begins[0]->clears.size(), 1u);
  EXPECT_EQ(begins[0]->clears[0].first, target);
  EXPECT_EQ(image_usage(compiled, target), vir::ImageUsageFlags::ColorAttachment);
  const auto barriers = image_barriers(module, compiled);
  ASSERT_EQ(barriers.size(), 1u);
  EXPECT_EQ(barriers[0].old_layout, VK_IMAGE_LAYOUT_UNDEFINED);
  EXPECT_EQ(barriers[0].dst, Access::ColorRW);
}

TEST(RenderGraphOpt, FoldedDepthClearStillWaitsOnThePreviousWriter) {
  auto module = Module{};
  const auto color = module.transient_image(transient_info());
  const auto image = Image::imported(VK_NULL_HANDLE, DEPTH_FORMAT, {WIDTH, HEIGHT, 1}, vir::Samples::e1);
  const auto depth = module.import_image(image, VK_IMAGE_LAYOUT_UNDEFINED, Access::DepthStencilRW);
  const auto cleared = module.clear(depth, ClearValue::depth(1.0f));
  const auto end = module.begin_rendering({{color, Access::ColorRW}, {cleared, Access::DepthStencilRW}})
                     .bind_graphics_pipeline(pipeline(0))
                     .draw(3_u32, 1_u32)
                     .end_rendering<2>()[0];
  const auto compiled = compile(module, end);
  EXPECT_TRUE(find_all<ir::Clear>(compiled).empty());
  const auto barriers = image_barriers(module, compiled);
  const auto transition = std::ranges::find(barriers, depth, &Barrier::resource);
  ASSERT_NE(transition, barriers.end()) << compiled.dump();
  EXPECT_EQ(transition->src, Access::DepthStencilRW);
}

TEST(RenderGraphOpt, AClearBeforeBothSelectionArmsFoldsIntoBothLoadOps) {
  auto module = Module{};
  const auto target = module.transient_image(transient_info());
  const auto enabled = module.declare_bool_var("enabled", true);
  const auto cleared = module.clear(target, clear::BLACK);
  const auto end = module.set_condition(
    enabled,
    [&](Module& arm) -> ValueID { return draw_into(arm, cleared); },
    [&](Module& arm) -> ValueID { return draw_into(arm, cleared); }
  );
  const auto compiled = compile(module, end);
  EXPECT_TRUE(find_all<ir::Clear>(compiled).empty());
  const auto begins = find_all<ir::BeginRendering>(compiled);
  ASSERT_EQ(begins.size(), 2u);
  for (const auto* begin : begins) {
    ASSERT_EQ(begin->clears.size(), 1u);
    EXPECT_EQ(begin->clears[0].first, target);
  }
}

TEST(RenderGraphOpt, AClearChosenByASelectionStaysATransferCommand) {
  auto module = Module{};
  const auto target = module.transient_image(transient_info());
  const auto enabled = module.declare_bool_var("enabled", true);
  const auto maybe = module.set_condition(
    enabled,
    [&](Module& arm) -> ValueID { return arm.clear(target, clear::BLACK); },
    [&](Module&) -> ValueID { return target; }
  );
  const auto compiled = compile(module, draw_into(module, maybe));
  EXPECT_EQ(find_all<ir::Clear>(compiled).size(), 1u) << compiled.dump();
}

TEST(RenderGraphOpt, ClearsCoveringMoreThanThePassPreservesItsTransfer) {
  for (const auto scenario : {0, 1, 2, 3}) {
    SCOPED_TRACE(scenario);
    auto module = Module{};
    auto info = transient_info();
    if (scenario == 0)
      info.format = vir::Format::D24UnormS8Uint;
    if (scenario == 2)
      info.mip_levels = 3;
    if (scenario == 3)
      info.array_layers = 4;
    const auto target = module.transient_image(info);
    const auto access = scenario == 0 ? Access::DepthStencilRW : Access::ColorRW;
    const auto cleared = module.clear(target, scenario == 0 ? ClearValue::depth(1.0f) : clear::BLACK);
    const auto end = module
                       .begin_rendering_area(
                         {{cleared, access}},
                         scenario == 1 ? vir::Extent2D{WIDTH / 2, HEIGHT / 2} : extent_2d()
                       )
                       .bind_graphics_pipeline(pipeline(0))
                       .draw(3_u32, 1_u32)
                       .end_rendering<1>()[0];
    const auto compiled = compile(module, end);
    EXPECT_EQ(find_all<ir::Clear>(compiled).size(), 1u) << compiled.dump();
    EXPECT_TRUE(find_all<ir::BeginRendering>(compiled)[0]->clears.empty());
  }
}

TEST(RenderGraphOpt, AnEmptyArmIsThreadedToTheMergeAndItsPhiIsRewired) {
  auto module = Module{};
  const auto target = module.transient_image(transient_info());
  const auto enabled = module.declare_bool_var("enabled", true);
  const auto drawn = draw_into(module, target);
  const auto maybe = module.set_condition(
    enabled,
    [&](Module& arm) -> ValueID { return draw_into(arm, drawn); },
    [&](Module&) -> ValueID { return drawn; }
  );
  const auto compiled = compile(module, module.export_to(maybe, Access::BlitRead));
  const auto branches = find_all<ir::BranchConditional>(compiled);
  const auto labels = find_all<ir::Label>(compiled);
  const auto merges = find_all<ir::SelectionMerge>(compiled);
  ASSERT_EQ(branches.size(), 1u);
  ASSERT_EQ(merges.size(), 1u);
  ASSERT_EQ(labels.size(), 3u) << compiled.dump();
  EXPECT_EQ(branches[0]->false_label, merges[0]->merge);
  EXPECT_NE(branches[0]->true_label, merges[0]->merge);
  const auto phis = find_all<ir::Phi>(compiled);
  ASSERT_EQ(phis.size(), 1u);
  EXPECT_NE(std::ranges::find(phis[0]->incoming, std::pair{drawn, labels[0]->label}), phis[0]->incoming.end());
}

TEST(RenderGraphOpt, AnArmHoldingABarrierKeepsItsBlock) {
  auto module = Module{};
  const auto target = module.transient_image(transient_info());
  const auto enabled = module.declare_bool_var("enabled", true);
  const auto maybe = module.set_condition(
    enabled,
    [&](Module& arm) -> ValueID { return draw_into(arm, target); },
    [&](Module&) -> ValueID { return target; }
  );
  const auto compiled = compile(module, module.export_to(maybe, Access::BlitRead));
  const auto branches = find_all<ir::BranchConditional>(compiled);
  const auto merges = find_all<ir::SelectionMerge>(compiled);
  ASSERT_EQ(branches.size(), 1u);
  ASSERT_EQ(merges.size(), 1u);
  EXPECT_NE(branches[0]->true_label, branches[0]->false_label);
  EXPECT_NE(branches[0]->false_label, merges[0]->merge);
  EXPECT_EQ(find_all<ir::Label>(compiled).size(), 4u) << compiled.dump();
}

TEST(RenderGraphOpt, ASelectionWithTwoEmptyAgreeingArmsFoldsAway) {
  auto module = Module{};
  const auto target = module.transient_image(transient_info());
  const auto enabled = module.declare_bool_var("enabled", true);
  const auto drawn = draw_into(module, target);
  const auto maybe = module.set_condition(
    enabled,
    [&](Module&) -> ValueID { return drawn; },
    [&](Module&) -> ValueID { return drawn; }
  );
  const auto compiled = compile(module, module.export_to(maybe, Access::BlitRead));
  EXPECT_TRUE(find_all<ir::BranchConditional>(compiled).empty()) << compiled.dump();
  EXPECT_TRUE(find_all<ir::SelectionMerge>(compiled).empty());
  EXPECT_EQ(find_all<ir::Label>(compiled).size(), 2u);
  const auto phis = find_all<ir::Phi>(compiled);
  ASSERT_EQ(phis.size(), 1u);
  EXPECT_EQ(phis[0]->incoming.size(), 1u);
}

TEST(RenderGraphOpt, EmptyArmsCarryingDifferentValuesKeepSeparateEdges) {
  auto module = Module{};
  const auto first = module.transient_image(transient_info());
  const auto second = module.transient_image(transient_info());
  const auto enabled = module.declare_bool_var("enabled", true);
  const auto chosen = module.set_condition(
    enabled,
    [&](Module&) -> ValueID { return first; },
    [&](Module&) -> ValueID { return second; }
  );
  const auto compiled = compile(module, module.export_to(chosen, Access::BlitRead));
  const auto branches = find_all<ir::BranchConditional>(compiled);
  const auto phis = find_all<ir::Phi>(compiled);
  ASSERT_EQ(branches.size(), 1u);
  ASSERT_EQ(phis.size(), 1u);
  EXPECT_NE(branches[0]->true_label, branches[0]->false_label);
  EXPECT_EQ(phis[0]->incoming.size(), 2u);
}

TEST(RenderGraphOpt, BarriersSharingASourceMergeTheirDestinations) {
  auto module = Module{};
  const auto target = module.transient_image(transient_info());
  const auto vertices = module.declare_buffer_var("vertices", Access::HostWrite);
  const auto indices = module.declare_buffer_var("indices", Access::HostWrite);
  const auto end = module.begin_rendering({{target, Access::ColorRW}})
                     .bind_graphics_pipeline(pipeline(0))
                     .bind_vertex_buffer(0, vertices)
                     .bind_index_buffer(indices, VK_INDEX_TYPE_UINT32)
                     .draw_indexed(3_u32, 1_u32)
                     .end_rendering<1>()[0];
  const auto compiled = compile(module, end);
  EXPECT_EQ(
    memory_barriers(module, compiled),
    (std::vector<std::pair<Access, Access>>{{Access::HostWrite, Access::AttributeRead | Access::IndexRead}})
  ) << compiled.dump();
}

TEST(RenderGraphOpt, BarriersWithAPassBetweenThemStaySeparate) {
  auto module = Module{};
  const auto first = module.transient_buffer({.size = 128});
  const auto second = module.transient_buffer({.size = 128});
  const auto written = module.begin_compute({{first, Access::ComputeWrite}, {second, Access::ComputeWrite}})
                         .bind_compute_pipeline(pipeline(0))
                         .dispatch(1_u32, 1_u32, 1_u32)
                         .end_compute<2>();
  const auto read = module.begin_compute({{written[0], Access::ComputeRead}})
                      .bind_compute_pipeline(pipeline(0))
                      .dispatch(1_u32, 1_u32, 1_u32)
                      .end_compute<1>()[0];
  const auto end = module.begin_compute({{written[1], Access::ComputeRead}, {read, Access::ComputeRead}})
                     .bind_compute_pipeline(pipeline(0))
                     .dispatch(1_u32, 1_u32, 1_u32)
                     .end_compute<2>()[0];
  const auto compiled = compile(module, end);
  EXPECT_EQ(memory_barriers(module, compiled).size(), 2u) << compiled.dump();
}

TEST(RenderGraphOpt, BarriersWithDifferentSourcesAndDestinationsMergeBothMasks) {
  auto module = Module{};
  const auto target = module.transient_image(transient_info());
  const auto vertices = module.declare_buffer_var("vertices", Access::HostWrite);
  const auto indices = module.declare_buffer_var("indices", Access::CopyWrite);
  const auto end = module.begin_rendering({{target, Access::ColorRW}})
                     .bind_graphics_pipeline(pipeline(0))
                     .bind_vertex_buffer(0, vertices)
                     .bind_index_buffer(indices, VK_INDEX_TYPE_UINT32)
                     .draw_indexed(3_u32, 1_u32)
                     .end_rendering<1>()[0];
  const auto compiled = compile(module, end);
  EXPECT_EQ(
    memory_barriers(module, compiled),
    (std::vector<std::pair<Access, Access>>{
      {Access::HostWrite | Access::CopyWrite, Access::AttributeRead | Access::IndexRead}
    })
  ) << compiled.dump();
}
TEST(RenderGraphOpt, DisjointMipsHaveConsecutiveImageBarriers) {
  auto module = Module{};
  auto info = untyped_info();
  info.mip_levels = 4;
  const auto image = module.transient_image(info);
  const auto source = module.mip(image, 0);
  const auto target = module.mip(image, 1);
  const auto end = module.begin_compute({{source, Access::ComputeSampled}, {target, Access::ComputeWrite}})
                     .bind_compute_pipeline(pipeline(0))
                     .dispatch(1_u32, 1_u32, 1_u32)
                     .end_compute<2>()[1];
  const auto compiled = compile(module, end);
  const auto barriers = find_all<ir::ImageBarrier>(compiled);
  ASSERT_EQ(barriers.size(), 2u) << compiled.dump();
  EXPECT_EQ(barriers[0]->range, Subresource::mip(0));
  EXPECT_EQ(barriers[1]->range, Subresource::mip(1));
  const auto first = std::ranges::find_if(compiled.instructions, [](const Instr& instruction) -> bool {
    return std::holds_alternative<ir::ImageBarrier>(instruction.ir);
  });
  ASSERT_NE(first + 1, compiled.instructions.end());
  EXPECT_TRUE(std::holds_alternative<ir::ImageBarrier>((first + 1)->ir));
}

TEST(RenderGraphOpt, CompatibleNeighboursAndDuplicatesFoldInsideTheRun) {
  const auto read = ValueID{0};
  const auto write = ValueID{1};
  const auto image = ValueID{2};
  auto nodes = std::vector<Instr>{
    {.id = read, .ir = ir::Constant::from_access(Access::None)},
    {.id = write, .ir = ir::Constant::from_access(Access::ComputeWrite)},
  };
  for (const auto mip : {0_u32, 0_u32, 2_u32, 1_u32, 1_u32, 0_u32}) {
    nodes.push_back(
      {.id = static_cast<ValueID>(nodes.size() + 1),
       .ir = ir::ImageBarrier{
         .src_access = read,
         .dst_access = write,
         .new_layout = VK_IMAGE_LAYOUT_GENERAL,
         .value = image,
         .range = Subresource::mip(mip),
       }}
    );
  }
  const auto compiled = Program(normalize_barriers(std::move(nodes)), {}, {});
  const auto barriers = find_all<ir::ImageBarrier>(compiled);
  ASSERT_EQ(barriers.size(), 1u);
  EXPECT_EQ(barriers[0]->range, (Subresource{.level_count = 3}));
}

TEST(RenderGraphOpt, OverlappingTransitionsStayOrdered) {
  for (const auto range : {Subresource::mip(0), Subresource::whole()}) {
    const auto image = ValueID{0};
    const auto access = ValueID{1};
    auto nodes = std::vector<Instr>{
      {.id = access, .ir = ir::Constant::from_access(Access::ComputeWrite)},
      {.id = ValueID{2},
       .ir =
         ir::ImageBarrier{
           .src_access = access,
           .dst_access = access,
           .new_layout = VK_IMAGE_LAYOUT_GENERAL,
           .value = image,
           .range = range
         }},
      {.id = ValueID{3},
       .ir = ir::ImageBarrier{
         .src_access = access,
         .dst_access = access,
         .old_layout = VK_IMAGE_LAYOUT_GENERAL,
         .new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         .value = image,
         .range = Subresource::mip(0)
       }},
    };
    const auto compiled = Program(normalize_barriers(std::move(nodes)), {}, {});
    const auto barriers = find_all<ir::ImageBarrier>(compiled);
    ASSERT_EQ(barriers.size(), 2u) << compiled.dump();
    EXPECT_EQ(barriers[0]->range, range);
    EXPECT_EQ(barriers[0]->new_layout, VK_IMAGE_LAYOUT_GENERAL);
    EXPECT_EQ(barriers[1]->old_layout, VK_IMAGE_LAYOUT_GENERAL);
    EXPECT_EQ(barriers[1]->new_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }
}

TEST(RenderGraphOpt, MemoryDependenciesPrecedeConsecutiveImageBarriers) {
  const auto image = ValueID{0};
  const auto access = ValueID{1};
  auto nodes = std::vector<Instr>{
    {.id = access, .ir = ir::Constant::from_access(Access::ComputeWrite)},
    {.id = ValueID{3},
     .ir =
       ir::ImageBarrier{
         .src_access = access,
         .dst_access = access,
         .new_layout = VK_IMAGE_LAYOUT_GENERAL,
         .value = image,
         .range = Subresource::layer(0)
       }},
    {.id = ValueID{2}, .ir = ir::MemoryBarrier{.src_access = access, .dst_access = access}},
    {.id = ValueID{4},
     .ir = ir::ImageBarrier{
       .src_access = access,
       .dst_access = access,
       .new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
       .value = image,
       .range = Subresource::layer(1)
     }},
  };
  const auto compiled = Program(normalize_barriers(std::move(nodes)), {}, {});
  ASSERT_EQ(compiled.instructions.size(), 4u);
  EXPECT_TRUE(std::holds_alternative<ir::MemoryBarrier>(compiled.instructions[1].ir));
  EXPECT_TRUE(std::holds_alternative<ir::ImageBarrier>(compiled.instructions[2].ir));
  EXPECT_TRUE(std::holds_alternative<ir::ImageBarrier>(compiled.instructions[3].ir));
  EXPECT_NE(compiled.dump().find("3 barriers"), std::string::npos);
}

} // namespace ox
