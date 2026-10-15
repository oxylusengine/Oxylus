#include <array>
#include <gtest/gtest.h>
#include <tuple>
#include <type_traits>
#include <utility>

#include "Render/Graph/Access.hpp"
#include "Render/Graph/Context.hpp"
#include "Render/Graph/Types.hpp"

namespace ox {
using namespace vir;

static_assert(sizeof(vir::ShaderStage) == sizeof(VkShaderStageFlags));
static_assert(!std::is_convertible_v<vir::ShaderStage, VkShaderStageFlags>);
static_assert(!std::is_convertible_v<VkShaderStageFlags, vir::ShaderStage>);
static_assert(std::to_underlying(vir::ShaderStage::None) == 0);
static_assert(std::to_underlying(vir::ShaderStage::Vertex) == VK_SHADER_STAGE_VERTEX_BIT);
static_assert(std::to_underlying(vir::ShaderStage::TessellationControl) == VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT);
static_assert(
  std::to_underlying(vir::ShaderStage::TessellationEvaluation) == VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT
);
static_assert(std::to_underlying(vir::ShaderStage::Geometry) == VK_SHADER_STAGE_GEOMETRY_BIT);
static_assert(std::to_underlying(vir::ShaderStage::Fragment) == VK_SHADER_STAGE_FRAGMENT_BIT);
static_assert(std::to_underlying(vir::ShaderStage::Compute) == VK_SHADER_STAGE_COMPUTE_BIT);
static_assert(std::to_underlying(vir::ShaderStage::AllGraphics) == VK_SHADER_STAGE_ALL_GRAPHICS);
static_assert(std::to_underlying(vir::ShaderStage::All) == VK_SHADER_STAGE_ALL);
static_assert(std::to_underlying(vir::ShaderStage::RayGeneration) == VK_SHADER_STAGE_RAYGEN_BIT_KHR);
static_assert(std::to_underlying(vir::ShaderStage::AnyHit) == VK_SHADER_STAGE_ANY_HIT_BIT_KHR);
static_assert(std::to_underlying(vir::ShaderStage::ClosestHit) == VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR);
static_assert(std::to_underlying(vir::ShaderStage::Miss) == VK_SHADER_STAGE_MISS_BIT_KHR);
static_assert(std::to_underlying(vir::ShaderStage::Intersection) == VK_SHADER_STAGE_INTERSECTION_BIT_KHR);
static_assert(std::to_underlying(vir::ShaderStage::Callable) == VK_SHADER_STAGE_CALLABLE_BIT_KHR);
static_assert(std::to_underlying(vir::ShaderStage::Task) == VK_SHADER_STAGE_TASK_BIT_EXT);
static_assert(std::to_underlying(vir::ShaderStage::Mesh) == VK_SHADER_STAGE_MESH_BIT_EXT);
static_assert(std::to_underlying(vir::ShaderStage::SubpassShadingHUAWEI) == VK_SHADER_STAGE_SUBPASS_SHADING_BIT_HUAWEI);
static_assert(std::to_underlying(vir::ShaderStage::ClusterCullingHUAWEI) == VK_SHADER_STAGE_CLUSTER_CULLING_BIT_HUAWEI);
static_assert(std::to_underlying(vir::ShaderStage::RayGenerationNV) == VK_SHADER_STAGE_RAYGEN_BIT_NV);
static_assert(std::to_underlying(vir::ShaderStage::AnyHitNV) == VK_SHADER_STAGE_ANY_HIT_BIT_NV);
static_assert(std::to_underlying(vir::ShaderStage::ClosestHitNV) == VK_SHADER_STAGE_CLOSEST_HIT_BIT_NV);
static_assert(std::to_underlying(vir::ShaderStage::MissNV) == VK_SHADER_STAGE_MISS_BIT_NV);
static_assert(std::to_underlying(vir::ShaderStage::IntersectionNV) == VK_SHADER_STAGE_INTERSECTION_BIT_NV);
static_assert(std::to_underlying(vir::ShaderStage::CallableNV) == VK_SHADER_STAGE_CALLABLE_BIT_NV);
static_assert(std::to_underlying(vir::ShaderStage::TaskNV) == VK_SHADER_STAGE_TASK_BIT_NV);
static_assert(std::to_underlying(vir::ShaderStage::MeshNV) == VK_SHADER_STAGE_MESH_BIT_NV);
static_assert(
  ((vir::ShaderStage::Vertex | vir::ShaderStage::Fragment) & vir::ShaderStage::Fragment) == vir::ShaderStage::Fragment
);

TEST(AccessConversion, EveryShaderStageKeepsSampleStorageUniformAndAccelerationAccessDistinct) {
  const auto cases = std::array{
    std::tuple{
      vir::ShaderStage::Vertex,
      VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
      Access::VertexSampled,
      Access::VertexRead,
      Access::VertexWrite,
      Access::VertexUniformRead,
      Access::VertexAccelerationStructureRead
    },
    std::tuple{
      vir::ShaderStage::TessellationControl,
      VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT,
      Access::TessellationControlSampled,
      Access::TessellationControlRead,
      Access::TessellationControlWrite,
      Access::TessellationControlUniformRead,
      Access::TessellationControlAccelerationStructureRead
    },
    std::tuple{
      vir::ShaderStage::TessellationEvaluation,
      VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT,
      Access::TessellationEvaluationSampled,
      Access::TessellationEvaluationRead,
      Access::TessellationEvaluationWrite,
      Access::TessellationEvaluationUniformRead,
      Access::TessellationEvaluationAccelerationStructureRead
    },
    std::tuple{
      vir::ShaderStage::Fragment,
      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
      Access::FragmentSampled,
      Access::FragmentRead,
      Access::FragmentWrite,
      Access::FragmentUniformRead,
      Access::FragmentAccelerationStructureRead
    },
    std::tuple{
      vir::ShaderStage::Compute,
      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      Access::ComputeSampled,
      Access::ComputeRead,
      Access::ComputeWrite,
      Access::ComputeUniformRead,
      Access::ComputeAccelerationStructureRead
    },
    std::tuple{
      vir::ShaderStage::Task,
      VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT,
      Access::TaskSampled,
      Access::TaskRead,
      Access::TaskWrite,
      Access::TaskUniformRead,
      Access::TaskAccelerationStructureRead
    },
    std::tuple{
      vir::ShaderStage::Mesh,
      VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT,
      Access::MeshSampled,
      Access::MeshRead,
      Access::MeshWrite,
      Access::MeshUniformRead,
      Access::MeshAccelerationStructureRead
    },
  };
  for (const auto& [shader, stage, sampled, read, write, uniform, acceleration] : cases) {
    SCOPED_TRACE(std::to_underlying(shader));
    EXPECT_EQ(sampled_by(shader), sampled);
    EXPECT_EQ(shader_read_by(shader), read);
    EXPECT_EQ(shader_write_by(shader), write);
    EXPECT_EQ(uniform_by(shader), uniform);
    EXPECT_EQ(acceleration_structure_read_by(shader), acceleration);
    EXPECT_EQ(to_stage_flags(sampled | read | write | uniform | acceleration), stage);
    EXPECT_EQ(to_access_flags(sampled), VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    EXPECT_EQ(to_access_flags(read), VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    EXPECT_EQ(to_access_flags(write), VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    EXPECT_EQ(to_access_flags(uniform), VK_ACCESS_2_UNIFORM_READ_BIT);
    EXPECT_EQ(to_access_flags(acceleration), VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    EXPECT_TRUE(reads(read));
    EXPECT_FALSE(writes(read));
    EXPECT_TRUE(writes(write));
    EXPECT_FALSE(reads(write));
  }
}

TEST(AccessConversion, RayTracingStagesMapToRayTracingAccess) {
  for (const auto stage :
       {vir::ShaderStage::RayGeneration,
        vir::ShaderStage::AnyHit,
        vir::ShaderStage::ClosestHit,
        vir::ShaderStage::Miss,
        vir::ShaderStage::Intersection,
        vir::ShaderStage::Callable}) {
    EXPECT_EQ(sampled_by(stage), Access::RayTracingSampled);
    EXPECT_EQ(shader_read_by(stage), Access::RayTracingRead);
    EXPECT_EQ(shader_write_by(stage), Access::RayTracingWrite);
    EXPECT_EQ(uniform_by(stage), Access::RayTracingUniformRead);
    EXPECT_EQ(to_stage_flags(shader_read_by(stage)), VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);
  }
}

TEST(AccessConversion, TransferOperationsUseTheirOwnStagesAndMergeFlags) {
  const auto cases = std::array{
    std::tuple{Access::CopyRead, Access::CopyWrite, VK_PIPELINE_STAGE_2_COPY_BIT},
    std::tuple{Access::BlitRead, Access::BlitWrite, VK_PIPELINE_STAGE_2_BLIT_BIT},
    std::tuple{Access::ResolveRead, Access::ResolveWrite, VK_PIPELINE_STAGE_2_RESOLVE_BIT},
  };
  for (const auto& [read, write, stage] : cases) {
    EXPECT_EQ(to_stage_flags(read | write), stage);
    EXPECT_EQ(to_access_flags(read | write), VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    EXPECT_EQ(to_image_layout(read), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    EXPECT_EQ(to_image_layout(write), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    EXPECT_EQ(to_image_layout(read | write), VK_IMAGE_LAYOUT_GENERAL);
    EXPECT_EQ(to_image_usage(read | write), vir::ImageUsageFlags::TransferSrc | vir::ImageUsageFlags::TransferDst);
    EXPECT_EQ(to_buffer_usage(read | write), VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  }
  EXPECT_EQ(to_stage_flags(Access::Clear), VK_PIPELINE_STAGE_2_CLEAR_BIT);
  EXPECT_EQ(to_image_usage(Access::Clear), vir::ImageUsageFlags::TransferDst);
}

TEST(AccessConversion, DepthReadWithSamplingAndStorageWithSamplingKeepTheirRequiredLayouts) {
  EXPECT_EQ(
    to_image_layout(Access::DepthStencilRead | Access::FragmentSampled),
    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
  );
  EXPECT_EQ(
    to_image_usage(Access::DepthStencilRead | Access::FragmentSampled),
    vir::ImageUsageFlags::DepthStencilAttachment | vir::ImageUsageFlags::Sampled
  );
  EXPECT_EQ(to_image_layout(Access::FragmentSampled | Access::ComputeRW), VK_IMAGE_LAYOUT_GENERAL);
  EXPECT_EQ(
    to_image_usage(Access::FragmentSampled | Access::ComputeRW),
    vir::ImageUsageFlags::Sampled | vir::ImageUsageFlags::Storage
  );
}

TEST(AccessConversion, EmptyAccessHasNoBarrierAndPresentHasNoMemoryAccess) {
  for (const auto access : {Access::Empty, Access::None}) {
    EXPECT_EQ(to_access_flags(access), VK_ACCESS_2_NONE);
    EXPECT_EQ(to_stage_flags(access), VK_PIPELINE_STAGE_2_NONE);
    EXPECT_EQ(to_image_usage(access), vir::ImageUsageFlags::None);
    EXPECT_EQ(to_buffer_usage(access), 0u);
    EXPECT_FALSE(reads(access));
    EXPECT_FALSE(writes(access));
  }
  EXPECT_EQ(to_access_flags(Access::Present), VK_ACCESS_2_NONE);
  EXPECT_EQ(to_image_layout(Access::Present), VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}

TEST(AccessConversion, CommandBarriersPreserveStageMasksAndExactImageRanges) {
  const auto memory = CommandBuffer::memory_barrier_info(Access::HostWrite, Access::AttributeRead | Access::IndexRead);
  EXPECT_EQ(memory.srcStageMask, VK_PIPELINE_STAGE_2_HOST_BIT);
  EXPECT_EQ(memory.srcAccessMask, VK_ACCESS_2_HOST_WRITE_BIT);
  EXPECT_EQ(memory.dstStageMask, VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT);
  EXPECT_EQ(memory.dstAccessMask, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT);
  const auto range = VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 2, 3, 1, 2};
  const auto image = CommandBuffer::image_barrier_info(
    VK_NULL_HANDLE,
    Access::ComputeWrite,
    Access::FragmentSampled,
    VK_IMAGE_LAYOUT_GENERAL,
    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    range
  );
  EXPECT_EQ(image.srcStageMask, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
  EXPECT_EQ(image.dstStageMask, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
  EXPECT_EQ(image.subresourceRange.baseMipLevel, 2u);
  EXPECT_EQ(image.subresourceRange.levelCount, 3u);
  EXPECT_EQ(image.subresourceRange.baseArrayLayer, 1u);
  EXPECT_EQ(image.subresourceRange.layerCount, 2u);
  EXPECT_EQ(image.srcQueueFamilyIndex, VK_QUEUE_FAMILY_IGNORED);
  EXPECT_EQ(image.dstQueueFamilyIndex, VK_QUEUE_FAMILY_IGNORED);
}
} // namespace ox
