#include <cstring>

#include "Render/Graph/Context.hpp"
#include "Render/Graph/Pipeline.hpp"
#include "Render/Graph/RenderGraph.hpp"
#include "Render/Graph/Types.hpp"
#include "RenderGraphTest.hpp"

namespace ox {
struct PipelineCapture {
  u32 created = 0;
  u32 destroyed = 0;
};

static VKAPI_ATTR auto VKAPI_CALL capture_graphics_pipeline(
  VkDevice device,
  VkPipelineCache,
  u32 count,
  const VkGraphicsPipelineCreateInfo* infos,
  const VkAllocationCallbacks*,
  VkPipeline* pipelines
) -> VkResult {
  auto& capture = *reinterpret_cast<PipelineCapture*>(device);
  capture.created += count;
  EXPECT_EQ(count, 1u);
  const auto& info = infos[0];
  const auto& vertex = *info.pVertexInputState;
  EXPECT_EQ(vertex.vertexBindingDescriptionCount, 2u);
  EXPECT_EQ(vertex.vertexAttributeDescriptionCount, 3u);
  if (vertex.vertexBindingDescriptionCount == 2 && vertex.vertexAttributeDescriptionCount == 3) {
    EXPECT_EQ(vertex.pVertexBindingDescriptions[1].binding, 1u);
    EXPECT_EQ(vertex.pVertexBindingDescriptions[1].inputRate, VK_VERTEX_INPUT_RATE_INSTANCE);
    EXPECT_EQ(vertex.pVertexBindingDescriptions[1].stride, 32u);
    EXPECT_EQ(vertex.pVertexAttributeDescriptions[1].format, static_cast<VkFormat>(vir::Format::RGBA8Unorm));
    EXPECT_EQ(vertex.pVertexAttributeDescriptions[1].offset, 16u);
  }
  EXPECT_EQ(info.pRasterizationState->depthClampEnable, VK_TRUE);
  EXPECT_EQ(info.pRasterizationState->depthBiasEnable, VK_TRUE);
  EXPECT_FLOAT_EQ(info.pRasterizationState->depthBiasSlopeFactor, 2.0f);
  EXPECT_EQ(info.pDepthStencilState->stencilTestEnable, VK_TRUE);
  EXPECT_EQ(info.pDepthStencilState->front.reference, 3u);
  EXPECT_NE(info.pTessellationState, nullptr);
  if (info.pTessellationState)
    EXPECT_EQ(info.pTessellationState->patchControlPoints, 4u);
  const auto* rendering = static_cast<const VkPipelineRenderingCreateInfo*>(info.pNext);
  EXPECT_EQ(rendering->viewMask, 5u);
  EXPECT_EQ(rendering->colorAttachmentCount, 2u);
  if (rendering->colorAttachmentCount == 2) {
    EXPECT_EQ(rendering->pColorAttachmentFormats[0], static_cast<VkFormat>(FORMAT));
    EXPECT_EQ(rendering->pColorAttachmentFormats[1], static_cast<VkFormat>(FORMAT));
  }
  EXPECT_EQ(rendering->depthAttachmentFormat, VK_FORMAT_D24_UNORM_S8_UINT);
  EXPECT_EQ(rendering->stencilAttachmentFormat, VK_FORMAT_D24_UNORM_S8_UINT);
  EXPECT_EQ(info.pMultisampleState->rasterizationSamples, VK_SAMPLE_COUNT_4_BIT);
  EXPECT_EQ(info.pColorBlendState->attachmentCount, 2u);
  if (info.pColorBlendState->attachmentCount == 2) {
    EXPECT_EQ(info.pColorBlendState->pAttachments[0].blendEnable, VK_FALSE);
    EXPECT_EQ(info.pColorBlendState->pAttachments[1].blendEnable, VK_TRUE);
  }
  pipelines[0] = reinterpret_cast<VkPipeline>(1_u64);
  return VK_SUCCESS;
}

TEST(RenderGraphPipeline, GraphicsCreationPreservesVertexOverridesAndExtendedState) {
  auto capture = PipelineCapture{};
  auto context = Context{};
  context.device = reinterpret_cast<VkDevice>(&capture);
  context.fn.vkCreateGraphicsPipelines = capture_graphics_pipeline;
  const auto vertex = VertexLayout{
    .bindings =
      {{.binding = 0, .stride = 20}, {.binding = 1, .stride = 32, .input_rate = VK_VERTEX_INPUT_RATE_INSTANCE}},
    .attributes = {
      {.location = 0, .binding = 0, .format = vir::Format::RG32Sfloat},
      {.location = 1, .binding = 0, .format = vir::Format::RGBA8Unorm, .offset = 16},
      {.location = 2, .binding = 1, .format = vir::Format::RGBA32Sfloat}
    }
  };
  const auto info = GraphicsPipelineInfo{.vertex_layout = vertex};
  auto state = PipelineState{};
  state.rendering = {
    .color_formats = {FORMAT, FORMAT},
    .depth_format = vir::Format::D24UnormS8Uint,
    .stencil_format = vir::Format::D24UnormS8Uint,
    .samples = vir::Samples::e4,
    .view_mask = 5
  };
  state.topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
  state.patch_control_points = 4;
  state.rasterization = {.is_depth_clamp_enabled = true, .is_depth_bias_enabled = true, .depth_bias_slope = 2.0f};
  state.stencil.is_test_enabled = true;
  state.stencil.front.reference = 3;
  state.blend = {blend_preset(BlendPreset::Off), blend_preset(BlendPreset::Additive)};
  const auto request = GraphicsPipelineRequest{.info = &info, .vertex = &*info.vertex_layout, .state = &state};
  const auto result = create_graphics_pipelines(context, std::span(&request, 1));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->size(), 1u);
  EXPECT_EQ(capture.created, 1u);
}

TEST(RenderGraphPipeline, FailedBatchCreationDestroysPartialPipelines) {
  auto capture = PipelineCapture{};
  auto context = Context{};
  context.device = reinterpret_cast<VkDevice>(&capture);
  context.fn.vkCreateGraphicsPipelines = [](
                                           VkDevice,
                                           VkPipelineCache,
                                           u32 count,
                                           const VkGraphicsPipelineCreateInfo*,
                                           const VkAllocationCallbacks*,
                                           VkPipeline* outputs
                                         ) -> VkResult {
    EXPECT_EQ(count, 2u);
    outputs[0] = reinterpret_cast<VkPipeline>(1_u64);
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  };
  context.fn.vkDestroyPipeline = [](VkDevice device, VkPipeline pipeline_handle, const VkAllocationCallbacks*) -> void {
    auto& recorded = *reinterpret_cast<PipelineCapture*>(device);
    EXPECT_NE(pipeline_handle, VK_NULL_HANDLE);
    recorded.destroyed++;
  };
  const auto info = GraphicsPipelineInfo{};
  const auto vertex = VertexLayout{};
  const auto state = PipelineState{};
  const auto request = GraphicsPipelineRequest{.info = &info, .vertex = &vertex, .state = &state};
  const auto requests = std::array{request, request};
  const auto result = create_graphics_pipelines(context, requests);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), VK_ERROR_OUT_OF_DEVICE_MEMORY);
  EXPECT_EQ(capture.destroyed, 1u);
}

TEST(RenderGraphPipeline, ReflectionMergesBindingAccessAndStages) {
  const auto reflections = std::array{
    ShaderLayout{
      .stage = vir::ShaderStage::Vertex,
      .bindings =
        {{.descriptor_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .stages = vir::ShaderStage::Vertex,
          .access = Access::VertexRead}}
    },
    ShaderLayout{
      .stage = vir::ShaderStage::Fragment,
      .bindings = {
        {.descriptor_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .stages = vir::ShaderStage::Fragment,
         .access = Access::FragmentWrite}
      }
    },
  };
  ASSERT_TRUE(validate_descriptor_bindings(reflections, nullopt).has_value());
  const auto bindings = merged_bindings(reflections);
  ASSERT_EQ(bindings.size(), 1u);
  EXPECT_EQ(bindings[0].stages, vir::ShaderStage::Vertex | vir::ShaderStage::Fragment);
  EXPECT_EQ(bindings[0].access, Access::VertexRead | Access::FragmentWrite);
}

TEST(RenderGraphPipeline, ConflictingDescriptorShapesAndNonBindlessArraysAreRejected) {
  for (const auto scenario : {0, 1, 2}) {
    auto reflections = std::array{
      ShaderLayout{
        .stage = vir::ShaderStage::Vertex,
        .bindings = {{.descriptor_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}}
      },
      ShaderLayout{
        .stage = vir::ShaderStage::Fragment,
        .bindings = {{.descriptor_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}}
      },
    };
    if (scenario == 0)
      reflections[1].bindings[0].descriptor_type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    if (scenario == 1)
      reflections[1].bindings[0].count = 2;
    if (scenario == 2)
      reflections[1].bindings[0].is_variable_count = true;
    EXPECT_FALSE(validate_descriptor_bindings(reflections, nullopt).has_value());
  }
  const auto array = ShaderLayout{
    .bindings = {{.set = 1, .descriptor_type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .is_variable_count = true}}
  };
  EXPECT_FALSE(validate_descriptor_bindings(std::span(&array, 1), nullopt).has_value());
  const auto bindless = option<BindlessDescriptorSet>{
    {.index = 1,
     .layout = reinterpret_cast<VkDescriptorSetLayout>(1_u64),
     .set = reinterpret_cast<VkDescriptorSet>(1_u64)}
  };
  EXPECT_TRUE(validate_descriptor_bindings(std::span(&array, 1), bindless).has_value());
}

TEST(RenderGraphPipeline, SpecializationPackingSharesBytesAndKeepsPerStageEntries) {
  const auto reflections = std::array{
    ShaderLayout{
      .stage = vir::ShaderStage::Vertex,
      .spec_constants = {{.id = 3, .size = 4, .stage = vir::ShaderStage::Vertex}}
    },
    ShaderLayout{
      .stage = vir::ShaderStage::Fragment,
      .spec_constants = {
        {.id = 3, .size = 4, .stage = vir::ShaderStage::Fragment},
        {.id = 7, .size = 8, .stage = vir::ShaderStage::Fragment}
      }
    },
  };
  auto specialization = Specialization{};
  specialization.set(7, SpecValue::from(2.5));
  specialization.set(3, SpecValue::from(8_u32));
  const auto packed = pack_specialization(reflections, specialization);
  ASSERT_TRUE(packed.has_value());
  EXPECT_EQ(packed->data.size(), 12u);
  ASSERT_EQ(packed->entries.size(), 2u);
  ASSERT_EQ(packed->entries[0].size(), 1u);
  ASSERT_EQ(packed->entries[1].size(), 2u);
  EXPECT_EQ(packed->entries[0][0].offset, packed->entries[1][0].offset);
  EXPECT_EQ(packed->entries[1][1].constantID, 7u);
  EXPECT_EQ(packed->entries[1][1].offset, 4u);
  EXPECT_EQ(packed->entries[1][1].size, 8u);
  auto x = 0_u32;
  std::memcpy(&x, packed->data.data(), sizeof(x));
  EXPECT_EQ(x, 8u);
  specialization.set(3, SpecValue::from(1.0));
  EXPECT_FALSE(pack_specialization(reflections, specialization).has_value());
}

TEST(RenderGraphPipeline, DefaultRayTracingGroupsOrderRaygenMissHitAndCallable) {
  const auto reflections = std::array{
    ShaderLayout{.stage = vir::ShaderStage::Callable},
    ShaderLayout{.stage = vir::ShaderStage::ClosestHit},
    ShaderLayout{.stage = vir::ShaderStage::Miss},
    ShaderLayout{.stage = vir::ShaderStage::RayGeneration},
    ShaderLayout{.stage = vir::ShaderStage::AnyHit},
    ShaderLayout{.stage = vir::ShaderStage::Intersection},
  };
  const auto groups = default_shader_groups(reflections);
  ASSERT_EQ(groups.size(), 4u);
  EXPECT_EQ(groups[0].general, 3u);
  EXPECT_EQ(groups[1].general, 2u);
  EXPECT_EQ(groups[2].type, VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR);
  EXPECT_EQ(groups[2].closest_hit, 1u);
  EXPECT_EQ(groups[2].any_hit, 4u);
  EXPECT_EQ(groups[2].intersection, 5u);
  EXPECT_EQ(groups[3].general, 0u);
  EXPECT_TRUE(validate_shader_groups(reflections, groups).has_value());
}

TEST(RenderGraphPipeline, ShaderGroupsRejectInvalidStageIndicesAndMissingIntersection) {
  const auto reflections = std::array{
    ShaderLayout{.stage = vir::ShaderStage::RayGeneration},
    ShaderLayout{.stage = vir::ShaderStage::ClosestHit},
    ShaderLayout{.stage = vir::ShaderStage::Intersection}
  };
  auto groups = std::array{
    RayTracingShaderGroup{.general = 0},
    RayTracingShaderGroup{
      .type = VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR,
      .closest_hit = 1,
      .intersection = 2
    }
  };
  EXPECT_TRUE(validate_shader_groups(reflections, groups).has_value());
  groups[0].general = 99;
  EXPECT_FALSE(validate_shader_groups(reflections, groups).has_value());
  groups[0].general = 1;
  EXPECT_FALSE(validate_shader_groups(reflections, groups).has_value());
  groups[0].general = 0;
  groups[1].intersection = VK_SHADER_UNUSED_KHR;
  EXPECT_FALSE(validate_shader_groups(reflections, groups).has_value());
  groups[1] = groups[0];
  EXPECT_FALSE(validate_shader_groups(reflections, groups).has_value());
}

TEST(RenderGraphPipeline, ShaderBindingTableAlignsRegionsWithoutAddingPhantomRecords) {
  const auto properties = RayTracingProperties{
    .shader_group_handle_size = 24,
    .shader_group_base_alignment = 64,
    .shader_group_handle_alignment = 32,
    .max_shader_group_stride = 4096
  };
  const auto layout = shader_binding_table_layout(properties, std::array<u32, 4>{1, 1, 3, 0});
  ASSERT_TRUE(layout.has_value());
  EXPECT_EQ(layout->regions[0].offset, 0);
  EXPECT_EQ(layout->regions[0].stride, layout->regions[0].size);
  EXPECT_EQ(layout->regions[1].offset, 64);
  EXPECT_EQ(layout->regions[1].size, 32);
  EXPECT_EQ(layout->regions[2].offset, 128);
  EXPECT_EQ(layout->regions[2].size, 96);
  EXPECT_EQ(layout->regions[3].size, 0);
  EXPECT_EQ(layout->size, 224);
  const auto sparse = shader_binding_table_layout(properties, std::array<u32, 4>{1, 0, 0, 1});
  ASSERT_TRUE(sparse.has_value());
  EXPECT_EQ(sparse->regions[3].offset, 64);
  EXPECT_EQ(sparse->regions[3].size, 32);
  EXPECT_EQ(sparse->size, 96);
}

TEST(RenderGraphPipeline, ShaderBindingTableRejectsUnsupportedPropertiesAndRaygenCounts) {
  auto properties = RayTracingProperties{
    .shader_group_handle_size = 32,
    .shader_group_base_alignment = 64,
    .shader_group_handle_alignment = 32,
    .max_shader_group_stride = 4096
  };
  EXPECT_FALSE(shader_binding_table_layout(properties, std::array<u32, 4>{0, 1, 1, 0}).has_value());
  EXPECT_FALSE(shader_binding_table_layout(properties, std::array<u32, 4>{2, 1, 1, 0}).has_value());
  properties.shader_group_base_alignment = 0;
  EXPECT_FALSE(shader_binding_table_layout(properties, std::array<u32, 4>{1, 1, 1, 0}).has_value());
  properties.shader_group_base_alignment = 64;
  properties.shader_group_handle_size = 4097;
  EXPECT_FALSE(shader_binding_table_layout(properties, std::array<u32, 4>{1, 1, 1, 0}).has_value());
}
} // namespace ox
