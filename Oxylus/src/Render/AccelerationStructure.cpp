#include "Render/AccelerationStructure.hpp"

#include <vuk/RenderGraph.hpp>
#include <vuk/runtime/CommandBuffer.hpp>
#include <vuk/runtime/vk/AllocatorHelpers.hpp>

#include "Core/Base.hpp"
#include "Render/RenderContext.hpp"
#include "Scene/SceneGPU.hpp"
#include "Utils/Log.hpp"

namespace ox {
constexpr static auto BLAS_VERTEX_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr static auto BLAS_VERTEX_STRIDE = 8_u64;
constexpr static auto AS_BUFFER_ALIGNMENT = 256_u64;

// the size query and the build have to name identical flags, or the structure and scratch
// allocations do not match what the build actually writes, so both sites read this one constant.
// ALLOW_COMPACTION is what pays for the query pool round trip in `compact_blas`: these are built
// once at load, so the round trip costs nothing at runtime and the compacted tree is typically half
// the size. A structure that is rebuilt per frame must not borrow these flags
constexpr static auto STATIC_BLAS_BUILD_FLAGS = VkBuildAccelerationStructureFlagsKHR{
  VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR
};

// same pairing rule: SceneTLAS::reserve sizes with these and build_scene_tlas builds with them.
// Deliberately the slow builder even though this one is rebuilt every frame: FAST_BUILD measured as
// no wall clock win and a slightly worse wavefront profile, and unlike a bottom level structure
// every ray traverses this one
constexpr static auto TLAS_BUILD_FLAGS = VkBuildAccelerationStructureFlagsKHR{
  VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR
};

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

// replaces `blas` with a compacted copy of itself. The build it measures has to have run already,
// and the device address changes, so this belongs before the structure is handed to anyone
static auto compact_blas(RenderContext& render_context, AccelerationStructure& blas) -> void {
  ZoneScoped;

  const auto query_pool = render_context.create_query_pool(VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, 1);
  if (query_pool == VK_NULL_HANDLE) {
    return;
  }
  OX_DEFER(&) { render_context.destroy_query_pool(query_pool); };

  auto query_pass = vuk::make_pass(
    "blas compacted size",
    [&render_context,
     query_pool,
     handle = *blas.handle](vuk::CommandBuffer& cmd_list, VUK_BA(vuk::eAccelerationStructureBuildRead) blas_buffer) {
      render_context.cmd_write_accel_structure_compacted_size(cmd_list.get_underlying(), handle, query_pool, 0);

      return blas_buffer;
    }
  );

  // the size has to be back on the host before the copy can even be sized, so this pass is its own
  // submission rather than a link in the build's chain
  auto queried_buffer = vuk::acquire_buf("blas", *blas.buffer, vuk::Access::eAccelerationStructureBuildWrite);
  render_context.wait_on(query_pass(std::move(queried_buffer)));

  auto compacted_size = 0_u64;
  if (!render_context.read_query_pool(query_pool, {&compacted_size, 1})) {
    return;
  }
  if (compacted_size == 0 || compacted_size >= blas.buffer->size) {
    return;
  }

  auto compacted = create_acceleration_structure(
    render_context,
    VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
    compacted_size
  );
  if (!compacted) {
    return;
  }

  const auto copy_info = VkCopyAccelerationStructureInfoKHR{
    .sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR,
    .pNext = nullptr,
    .src = *blas.handle,
    .dst = *compacted.handle,
    .mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR,
  };

  auto copy_pass = vuk::make_pass(
    "blas compact",
    [&render_context, copy_info](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eAccelerationStructureBuildWrite) dst,
      VUK_BA(vuk::eAccelerationStructureBuildRead) src
    ) {
      render_context.cmd_copy_accel_structure(cmd_list.get_underlying(), copy_info);

      return dst;
    }
  );

  // the source is read by the copy and dies at the assignment below
  auto compacted_buffer = vuk::discard_buf("blas compacted", *compacted.buffer);
  auto source_buffer = vuk::acquire_buf("blas", *blas.buffer, vuk::Access::eAccelerationStructureBuildWrite);
  render_context.wait_on(copy_pass(std::move(compacted_buffer), std::move(source_buffer)));

  blas = std::move(compacted);
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
    .flags = STATIC_BLAS_BUILD_FLAGS,
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

  // TODO: vuk needs more granular acces for the mesh param under the pass
  auto blas_buffer = vuk::discard_buf("blas", *out_blas.buffer);
  auto build_pass = vuk::make_pass(
    "blas build",
    [build_info, geometry, primitive_count](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eAccelerationStructureBuildWrite) blas,
      VUK_BA(vuk::eAccelerationStructureBuildRead) mesh
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

      return blas;
    }
  );

  // callers publish `out_blas.device_address` as soon as this returns and the TLAS build dereferences
  // it as a raw pointer, so the build has to have actually run by then. Nothing downstream waits on
  // the returned value first: the batched loader path only parks it in an `UploadBatch`
  auto built = build_pass(std::move(blas_buffer), std::move(mesh_buffer));
  render_context.wait_on(vuk::UntypedValue(built));

  compact_blas(render_context, out_blas);

  return built;
}

auto SceneTLAS::reserve(this SceneTLAS& self, RenderContext& render_context, u32 instance_count) -> bool {
  ZoneScoped;

  if (!render_context.use_ray_tracing() || instance_count == 0) {
    return false;
  }

  if (self.acceleration_structure && instance_count <= self.capacity) {
    return true;
  }

  // grow only, deliberately: this is reserved from the frame that needs it, so releasing on a dip
  // would hand the next spike a fresh structure, buffer and scratch allocation mid frame
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
    .flags = TLAS_BUILD_FLAGS,
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

auto build_scene_tlas(RenderContext& render_context, SceneTLAS& scene_tlas, TLASBuildInfo&& info)
  -> vuk::Value<vuk::Buffer> {
  ZoneScoped;

  if (!scene_tlas.reserve(render_context, info.instance_count)) {
    return {};
  }

  auto instances_buffer = vuk::discard_buf("tlas instances", *scene_tlas.instances_buffer);
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
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, instance_count)
        .dispatch_invocations(instance_count);

      return instances;
    }
  );

  instances_buffer = write_instances_pass(
    std::move(instances_buffer),
    std::move(info.mesh_instances_buffer),
    std::move(info.transforms_buffer),
    std::move(info.blas_addresses_buffer)
  );

  render_context.wait_on(std::move(instances_buffer));
  instances_buffer = vuk::acquire_buf("tlas instances", *scene_tlas.instances_buffer, vuk::Access::eMemoryWrite);

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
           .data = {.deviceAddress = scene_tlas.instances_buffer->device_address},
         }},
    .flags = 0,
  };

  auto build_info = VkAccelerationStructureBuildGeometryInfoKHR{
    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
    .pNext = nullptr,
    .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
    .flags = TLAS_BUILD_FLAGS,
    .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
    .srcAccelerationStructure = VK_NULL_HANDLE,
    .dstAccelerationStructure = *scene_tlas.acceleration_structure.handle,
    .geometryCount = 1,
    .pGeometries = &geometry,
    .ppGeometries = nullptr,
    .scratchData = {.deviceAddress = scene_tlas.scratch_buffer->device_address},
  };

  auto tlas_buffer = vuk::discard_buf("tlas", *scene_tlas.acceleration_structure.buffer);
  auto build_pass = vuk::make_pass(
    "tlas build",
    [build_info, geometry, instance_count = info.instance_count](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eAccelerationStructureBuildWrite) tlas,
      VUK_BA(vuk::eAccelerationStructureBuildRead) instances
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

      return tlas;
    }
  );

  return build_pass(std::move(tlas_buffer), std::move(instances_buffer));
}
} // namespace ox
