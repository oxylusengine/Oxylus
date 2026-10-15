#include "RenderGraphTest.hpp"

namespace ox {
TEST(RenderGraphAnalysis, DescriptorKindsMustMatchTheReflectedType) {
  const auto cases = std::array{
    std::pair{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, true},
    std::pair{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, true},
    std::pair{VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, true},
    std::pair{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, false},
    std::pair{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, false},
  };
  for (const auto& [type, compatible] : cases) {
    SCOPED_TRACE(type);
    auto module = Module{};
    const auto target = module.transient_image(transient_info());
    const auto image = module.transient_image(untyped_info());
    const auto end = module.begin_rendering({{target, Access::ColorRW}})
                       .bind_graphics_pipeline(pipeline(0))
                       .bind_image(0, 0, image)
                       .draw(3_u32, 1_u32)
                       .end_rendering<1>()[0];
    const auto access = type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE      ? Access::FragmentRW
                        : type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT ? Access::InputAttachmentRead
                                                                      : Access::FragmentSampled;
    const auto declared = Declared::with_access({{0, 0, type}}, vir::ShaderStage::Fragment, access);
    EXPECT_EQ(module.compile(declared.pipelines(), end).has_value(), compatible);
  }
}

TEST(RenderGraphAnalysis, ImageAndBufferDescriptorsMustNameTheCorrectResourceKind) {
  for (const auto image_binding : {false, true}) {
    auto module = Module{};
    const auto target = module.transient_image(transient_info());
    const auto wrong = image_binding ? module.transient_buffer({.size = 64}) : module.transient_image(untyped_info());
    module.begin_rendering({{target, Access::ColorRW}}).bind_graphics_pipeline(pipeline(0));
    if (image_binding)
      module.bind_image(0, 0, wrong);
    else
      module.bind_buffer(0, 0, wrong);
    const auto end = module.draw(3_u32, 1_u32).end_rendering<1>()[0];
    const auto declared = Declared::with_access(
      {{0, 0, image_binding ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}},
      vir::ShaderStage::Fragment,
      Access::FragmentRead
    );
    EXPECT_FALSE(module.compile(declared.pipelines(), end).has_value());
  }
}

TEST(RenderGraphAnalysis, NullSamplersAndInvalidBufferRangesAreRejected) {
  for (const auto scenario : {0, 1, 2}) {
    SCOPED_TRACE(scenario);
    auto module = Module{};
    const auto target = module.transient_image(transient_info());
    const auto buffer = module.transient_buffer({.size = 64});
    module.begin_rendering({{target, Access::ColorRW}}).bind_graphics_pipeline(pipeline(0));
    if (scenario == 0)
      module.bind_sampler(0, 0, VK_NULL_HANDLE);
    else
      module.bind_buffer_range(0, 0, buffer, scenario == 1 ? 0 : ~0_u64 - 4, scenario == 1 ? 0 : 16);
    const auto end = module.draw(3_u32, 1_u32).end_rendering<1>()[0];
    const auto declared = Declared::with_access(
      {{0, 0, scenario == 0 ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}},
      vir::ShaderStage::Fragment,
      Access::FragmentRead
    );
    EXPECT_FALSE(module.compile(declared.pipelines(), end).has_value());
  }
}

TEST(RenderGraphAnalysis, UncheckedBindingsDoNotPermitUnreflectedDescriptors) {
  auto module = Module{};
  const auto target = module.transient_image(transient_info());
  const auto end = module.begin_rendering({{target, Access::ColorRW}})
                     .bind_graphics_pipeline(pipeline(0))
                     .bind_image(0, 0, target)
                     .draw(3_u32, 1_u32)
                     .end_rendering<1>()[0];
  EXPECT_FALSE(module.compile(PipelineBindings::unchecked(), end).has_value());
}

TEST(RenderGraphAnalysis, DescriptorWritesDoNotCarryIntoTheNextRegion) {
  auto module = Module{};
  const auto target = module.transient_image(transient_info());
  const auto image = module.transient_image(untyped_info());
  const auto first = module.begin_rendering({{target, Access::ColorRW}})
                       .bind_graphics_pipeline(pipeline(0))
                       .bind_image(0, 0, image)
                       .draw(3_u32, 1_u32)
                       .end_rendering<1>()[0];
  const auto end = module.begin_rendering({{first, Access::ColorRW}})
                     .bind_graphics_pipeline(pipeline(0))
                     .draw(3_u32, 1_u32)
                     .end_rendering<1>()[0];
  const auto declared = Declared::fragment({{0, 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE}});
  EXPECT_FALSE(module.compile(declared.pipelines(), end).has_value());
}

TEST(RenderGraphAnalysis, BindlessSetsNeedNoIndividualWritesAndRejectGraphWrites) {
  for (const auto write : {false, true}) {
    auto module = Module{};
    const auto target = module.transient_image(transient_info());
    module.begin_rendering({{target, Access::ColorRW}}).bind_graphics_pipeline(pipeline(0));
    if (write)
      module.bind_image(1, 0, target);
    const auto end = module.draw(3_u32, 1_u32).end_rendering<1>()[0];
    auto declared = Declared::fragment({{1, 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE}});
    declared.bindless = 1;
    EXPECT_EQ(module.compile(declared.pipelines(), end).has_value(), !write);
  }
}

TEST(RenderGraphAnalysis, BufferDescriptorsInferUsageAndWaitBeforeDispatch) {
  const auto cases = std::array{
    std::tuple{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, Access::ComputeUniformRead, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT},
    std::tuple{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, Access::ComputeRead, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT},
  };
  for (const auto& [type, access, usage] : cases) {
    auto module = Module{};
    const auto output = module.transient_image(untyped_info());
    const auto buffer = module.declare_buffer_var("buffer", Access::HostWrite);
    const auto end = module.begin_compute({{output, Access::ComputeWrite}})
                       .bind_compute_pipeline(pipeline(0))
                       .bind_buffer_range(0, 0, buffer, 16, 32)
                       .dispatch(1_u32, 1_u32, 1_u32)
                       .end_compute<1>()[0];
    const auto declared = Declared::with_access({{0, 0, type}}, vir::ShaderStage::Compute, access);
    const auto compiled = module.compile(declared.pipelines(), end);
    ASSERT_TRUE(compiled.has_value());
    const auto writes = find_all<ir::WriteDescriptor>(*compiled);
    ASSERT_EQ(writes.size(), 1u);
    EXPECT_EQ(writes[0]->descriptor_type, type);
    EXPECT_EQ(writes[0]->descriptor.offset, 16u);
    EXPECT_EQ(writes[0]->descriptor.range, 32u);
    EXPECT_EQ(buffer_usage(*compiled, buffer), usage);
    EXPECT_EQ(
      memory_barriers(module, *compiled),
      (std::vector<std::pair<Access, Access>>{{Access::HostWrite, access}})
    );
    EXPECT_LT(position_of<ir::MemoryBarrier>(*compiled), position_of<ir::Dispatch>(*compiled));
  }
}

TEST(RenderGraphAnalysis, SamplingTheSameImageTwiceDoesNotDuplicateItsBarrier) {
  auto module = Module{};
  const auto output = module.transient_image(transient_info());
  const auto image = module.transient_image(untyped_info());
  const auto end = module.begin_rendering({{output, Access::ColorRW}})
                     .bind_graphics_pipeline(pipeline(0))
                     .bind_image(0, 0, image)
                     .bind_image(0, 1, image)
                     .draw(3_u32, 1_u32)
                     .end_rendering<1>()[0];
  const auto declared = Declared::fragment(
    {{0, 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE}, {0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE}}
  );
  const auto compiled = module.compile(declared.pipelines(), end);
  ASSERT_TRUE(compiled.has_value());
  const auto barriers = image_barriers(module, *compiled);
  EXPECT_EQ(std::ranges::count(barriers, image, &Barrier::resource), 1);
}

struct ChangingBindings {
  std::array<DescriptorBinding, 2> bindings = {
    DescriptorBinding{
      .descriptor_type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      .stages = vir::ShaderStage::Compute,
      .access = Access::ComputeUniformRead
    },
    DescriptorBinding{
      .descriptor_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .stages = vir::ShaderStage::Compute,
      .access = Access::ComputeRead
    },
  };
  auto pipelines(this const ChangingBindings& self) -> PipelineBindings {
    return {
      .user = &self,
      .bindings_fn = [](const void* user, PipelineID pipeline_id) -> option<std::span<const DescriptorBinding>> {
        const auto* supplied = static_cast<const ChangingBindings*>(user);
        return std::span(supplied->bindings).subspan(std::to_underlying(pipeline_id), 1);
      }
    };
  }
};

TEST(RenderGraphAnalysis, ChangingDescriptorTypesBetweenPipelinesRequiresARebind) {
  for (const auto rebind : {false, true}) {
    auto module = Module{};
    const auto output = module.transient_image(untyped_info());
    const auto buffer = module.transient_buffer({.size = 64});
    module.begin_compute({{output, Access::ComputeWrite}})
      .bind_compute_pipeline(pipeline(0))
      .bind_buffer(0, 0, buffer)
      .dispatch(1_u32, 1_u32, 1_u32)
      .bind_compute_pipeline(pipeline(1));
    if (rebind)
      module.bind_buffer(0, 0, buffer);
    const auto end = module.dispatch(1_u32, 1_u32, 1_u32).end_compute<1>()[0];
    const auto declared = ChangingBindings{};
    EXPECT_EQ(module.compile(declared.pipelines(), end).has_value(), rebind);
  }
}
} // namespace ox
