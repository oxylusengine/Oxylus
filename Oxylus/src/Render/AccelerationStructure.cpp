#include "Render/AccelerationStructure.hpp"

#include <vuk/RenderGraph.hpp>
#include <vuk/runtime/CommandBuffer.hpp>
#include <vuk/runtime/vk/AllocatorHelpers.hpp>

#include "Render/RenderContext.hpp"
#include "Render/Utils/VukCommon.hpp"
#include "Scene/SceneGPU.hpp"
#include "Utils/Log.hpp"

namespace ox {
constexpr static auto BLAS_VERTEX_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr static auto BLAS_VERTEX_STRIDE = 8_u64;
constexpr static auto AS_BUFFER_ALIGNMENT = 256_u64;

static auto create_acceleration_structure(RenderContext& render_context, VkAccelerationStructureTypeKHR type, u64 size)
  -> AccelerationStructure {
  ZoneScoped;

  auto result = AccelerationStructure{};
  result.buffer = render_context.allocate_buffer_super(vuk::MemoryUsage::eGPUonly, size, AS_BUFFER_ALIGNMENT);

  auto create_info = VkAccelerationStructureCreateInfoKHR{
    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
    .pNext = nullptr,
    .createFlags = 0,
    .buffer = result.buffer->buffer,
    .offset = result.buffer->offset,
    .size = size,
    .type = type,
    .deviceAddress = 0,
  };

  auto& allocator = render_context.superframe_allocator.value();
  result.handle = vuk::Unique<VkAccelerationStructureKHR>(allocator);
  if (!allocator.allocate_acceleration_structures({&*result.handle, 1}, {&create_info, 1})) {
    OX_LOG_ERROR("Failed to allocate acceleration structure.");
    return {};
  }

  result.device_address = render_context.get_accel_structure_device_address(*result.handle);

  return result;
}

auto build_mesh_blas(
  RenderContext& render_context,
  const BLASBuildInfo& info,
  vuk::Value<vuk::Buffer>&& mesh_buffer,
  AccelerationStructure& out_blas,
  vuk::Unique<vuk::Buffer>& out_scratch
) -> vuk::Value<vuk::Buffer> {
  ZoneScoped;

  out_blas = {};
  out_scratch.reset();
  if (!render_context.use_ray_tracing() || info.index_count < 3 || info.vertex_count == 0) {
    return std::move(mesh_buffer);
  }

  const auto primitive_count = info.index_count / 3;

  auto geometry = VkAccelerationStructureGeometryKHR{
    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
    .pNext = nullptr,
    .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
    .geometry =
      {.triangles =
         {
           .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
           .pNext = nullptr,
           .vertexFormat = BLAS_VERTEX_FORMAT,
           .vertexData = {.deviceAddress = info.vertex_positions},
           .vertexStride = BLAS_VERTEX_STRIDE,
           .maxVertex = info.vertex_count - 1,
           .indexType = VK_INDEX_TYPE_UINT32,
           .indexData = {.deviceAddress = info.indices},
           .transformData = {},
         }},
    .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
  };

  auto build_info = VkAccelerationStructureBuildGeometryInfoKHR{
    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
    .pNext = nullptr,
    .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
    .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
    .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
    .srcAccelerationStructure = VK_NULL_HANDLE,
    .dstAccelerationStructure = VK_NULL_HANDLE,
    .geometryCount = 1,
    .pGeometries = &geometry,
    .ppGeometries = nullptr,
    .scratchData = {},
  };

  auto size_info = VkAccelerationStructureBuildSizesInfoKHR{
    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
    .pNext = nullptr,
    .accelerationStructureSize = 0,
    .updateScratchSize = 0,
    .buildScratchSize = 0,
  };
  render_context.runtime->vkGetAccelerationStructureBuildSizesKHR(
    render_context.device,
    VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
    &build_info,
    &primitive_count,
    &size_info
  );

  out_blas = create_acceleration_structure(
    render_context,
    VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
    size_info.accelerationStructureSize
  );
  if (!out_blas) {
    return std::move(mesh_buffer);
  }

  out_scratch = render_context.allocate_buffer_super(
    vuk::MemoryUsage::eGPUonly,
    size_info.buildScratchSize,
    render_context.as_scratch_alignment()
  );

  build_info.dstAccelerationStructure = *out_blas.handle;
  build_info.scratchData.deviceAddress = out_scratch->device_address;

  auto blas_buffer = vuk::discard_buf("blas", *out_blas.buffer);
  auto build_pass = vuk::make_pass(
    "blas build",
    [build_info, geometry, primitive_count](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eAccelerationStructureBuildWrite) blas_ba,
      VUK_BA(vuk::eAccelerationStructureBuildRead) mesh_ba
    ) mutable {
      build_info.pGeometries = &geometry;

      const auto range = VkAccelerationStructureBuildRangeInfoKHR{
        .primitiveCount = primitive_count,
        .primitiveOffset = 0,
        .firstVertex = 0,
        .transformOffset = 0,
      };
      const auto* range_ptr = &range;
      cmd_list.build_acceleration_structures(1, &build_info, &range_ptr);

      return blas_ba;
    }
  );

  return build_pass(std::move(blas_buffer), std::move(mesh_buffer));
}

auto SceneTLAS::reserve(this SceneTLAS& self, RenderContext& render_context, u32 instance_count) -> bool {
  ZoneScoped;

  if (!render_context.use_ray_tracing() || instance_count == 0) {
    return false;
  }

  if (self.acceleration_structure && instance_count <= self.capacity) {
    return true;
  }

  const auto capacity = std::bit_ceil(instance_count);

  auto geometry = VkAccelerationStructureGeometryKHR{
    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
    .pNext = nullptr,
    .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
    .geometry =
      {.instances =
         {
           .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
           .pNext = nullptr,
           .arrayOfPointers = VK_FALSE,
           .data = {},
         }},
    .flags = 0,
  };

  auto build_info = VkAccelerationStructureBuildGeometryInfoKHR{
    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
    .pNext = nullptr,
    .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
    .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
    .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
    .srcAccelerationStructure = VK_NULL_HANDLE,
    .dstAccelerationStructure = VK_NULL_HANDLE,
    .geometryCount = 1,
    .pGeometries = &geometry,
    .ppGeometries = nullptr,
    .scratchData = {},
  };

  auto size_info = VkAccelerationStructureBuildSizesInfoKHR{
    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
    .pNext = nullptr,
    .accelerationStructureSize = 0,
    .updateScratchSize = 0,
    .buildScratchSize = 0,
  };
  render_context.runtime->vkGetAccelerationStructureBuildSizesKHR(
    render_context.device,
    VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
    &build_info,
    &capacity,
    &size_info
  );

  self.acceleration_structure = create_acceleration_structure(
    render_context,
    VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
    size_info.accelerationStructureSize
  );
  if (!self.acceleration_structure) {
    self.capacity = 0;
    return false;
  }

  self.instances_buffer = render_context.allocate_buffer_super(
    vuk::MemoryUsage::eGPUonly,
    static_cast<u64>(capacity) * sizeof(GPU::AccelerationStructureInstance),
    16
  );
  self.scratch_buffer = render_context.allocate_buffer_super(
    vuk::MemoryUsage::eGPUonly,
    size_info.buildScratchSize,
    render_context.as_scratch_alignment()
  );
  self.capacity = capacity;

  return true;
}

auto build_scene_tlas(RenderContext& render_context, SceneTLAS& tlas, TLASBuildInfo&& info) -> vuk::Value<vuk::Buffer> {
  ZoneScoped;

  if (!tlas.reserve(render_context, info.instance_count)) {
    return {};
  }

  auto instances_buffer = vuk::discard_buf("tlas instances", *tlas.instances_buffer);
  auto write_instances_pass = vuk::make_pass(
    "tlas write instances",
    [instance_count = info.instance_count](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eComputeWrite) instances,
      VUK_BA(vuk::eComputeRead) mesh_instances,
      VUK_BA(vuk::eComputeRead) transforms,
      VUK_BA(vuk::eComputeRead) blas_addresses
    ) {
      cmd_list //
        .bind_compute_pipeline("tlas_write_instances")
        .bind_buffer(0, 0, mesh_instances)
        .bind_buffer(0, 1, transforms)
        .bind_buffer(0, 2, blas_addresses)
        .bind_buffer(0, 3, instances)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(instance_count))
        .dispatch((instance_count + 63) / 64);

      return instances;
    }
  );

  instances_buffer = write_instances_pass(
    std::move(instances_buffer),
    std::move(info.mesh_instances_buffer),
    std::move(info.transforms_buffer),
    std::move(info.blas_addresses_buffer)
  );

  auto geometry = VkAccelerationStructureGeometryKHR{
    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
    .pNext = nullptr,
    .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
    .geometry =
      {.instances =
         {
           .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
           .pNext = nullptr,
           .arrayOfPointers = VK_FALSE,
           .data = {.deviceAddress = tlas.instances_buffer->device_address},
         }},
    .flags = 0,
  };

  auto build_info = VkAccelerationStructureBuildGeometryInfoKHR{
    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
    .pNext = nullptr,
    .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
    .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
    .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
    .srcAccelerationStructure = VK_NULL_HANDLE,
    .dstAccelerationStructure = *tlas.acceleration_structure.handle,
    .geometryCount = 1,
    .pGeometries = &geometry,
    .ppGeometries = nullptr,
    .scratchData = {.deviceAddress = tlas.scratch_buffer->device_address},
  };

  auto tlas_buffer = vuk::discard_buf("tlas", *tlas.acceleration_structure.buffer);
  auto build_pass = vuk::make_pass(
    "tlas build",
    [build_info, geometry, instance_count = info.instance_count](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eAccelerationStructureBuildWrite) tlas_ba,
      VUK_BA(vuk::eAccelerationStructureBuildRead) instances_ba
    ) mutable {
      build_info.pGeometries = &geometry;

      const auto range = VkAccelerationStructureBuildRangeInfoKHR{
        .primitiveCount = instance_count,
        .primitiveOffset = 0,
        .firstVertex = 0,
        .transformOffset = 0,
      };
      const auto* range_ptr = &range;
      cmd_list.build_acceleration_structures(1, &build_info, &range_ptr);

      return tlas_ba;
    }
  );

  return build_pass(std::move(tlas_buffer), std::move(instances_buffer));
}
} // namespace ox
