#include "Render/AccelerationStructure.hpp"

#include <algorithm>
#include <limits>
#include <vuk/RenderGraph.hpp>
#include <vuk/runtime/CommandBuffer.hpp>
#include <vuk/runtime/vk/AllocatorHelpers.hpp>

#include "Memory/Stack.hpp"
#include "Render/RenderContext.hpp"
#include "Render/Utils/VukCommon.hpp"
#include "Scene/SceneGPU.hpp"
#include "Utils/Log.hpp"

namespace ox {
constexpr static auto BLAS_VERTEX_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr static auto BLAS_VERTEX_STRIDE = 8_u64;
constexpr static auto AS_BUFFER_ALIGNMENT = 256_u64;
// the size query and the build have to name identical flags, or the structure and scratch
// allocations do not match what the build actually writes, so both sites read this one constant.
// ALLOW_UPDATE is what lets an unchanged topology refit rather than rebuild, and it is also why the
// tree is worth spending on: full builds are rare and staggered now, and every refit between them
// inherits whatever tree the last one produced, so the fast builder's cheaper tree would be paid for
// over and over by traversal instead of once by the build
constexpr static auto SKINNED_BLAS_BUILD_FLAGS = VkBuildAccelerationStructureFlagsKHR{
  VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR
};
// same pairing rule: SceneTLAS::reserve sizes with these and build_scene_tlas builds with them.
// Deliberately not the fast builder the skinned structures use: measured as no wall clock win and a
// slightly worse wavefront profile, and unlike a bottom level structure every ray traverses this one.
constexpr static auto TLAS_BUILD_FLAGS = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;

static auto make_triangle_geometry(const BLASBuildInfo& info) -> VkAccelerationStructureGeometryKHR {
  return {
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
}

static auto is_buildable(const BLASBuildInfo& info) -> bool {
  return info.vertex_positions != 0 && info.indices != 0 && info.vertex_count != 0 && info.index_count >= 3;
}

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

  auto geometry = make_triangle_geometry(info);

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

  return build_pass(std::move(blas_buffer), std::move(mesh_buffer));
}

auto SkinnedBLASPool::reset(this SkinnedBLASPool& self) -> void {
  self.handles.clear();
  self.device_addresses.clear();
  self.scratch_offsets.clear();
  self.builds.clear();
  self.build_modes.clear();
  self.refit_counts.clear();
  self.buffer.reset();
  self.scratch_buffer.reset();
  self.address_min = 0;
  self.address_max = 0;
  self.layout_key = 0;
}

auto SkinnedBLASPool::reserve(
  this SkinnedBLASPool& self, RenderContext& render_context, std::span<const BLASBuildInfo> infos
) -> bool {
  ZoneScoped;
  memory::ScopedStack stack;

  if (!render_context.use_ray_tracing() || infos.empty()) {
    self.reset();
    return false;
  }

  // the acceleration structure sizes depend on the geometry counts alone, so a frame that only
  // moved the vertex arena keeps its handles and just refreshes the build descriptors. The index
  // address is in here and the vertex address deliberately is not: a refit may be pointed at moved
  // vertices, which is what an arena resize does, but it may not be pointed at different indices,
  // so a model reloading under a structure has to invalidate it
  auto key = 0xcbf29ce484222325_u64 ^ infos.size();
  for (const auto& info : infos) {
    key = (key ^ info.vertex_count) * 0x100000001b3_u64;
    key = (key ^ info.index_count) * 0x100000001b3_u64;
    key = (key ^ info.indices) * 0x100000001b3_u64;
  }

  self.builds.assign(infos.begin(), infos.end());
  if (
    self.layout_key == key && self.handles.size() == infos.size() && self.build_modes.size() == infos.size() &&
    self.refit_counts.size() == infos.size()
  ) {
    return !self.handles.empty();
  }

  // a freshly minted structure holds no tree for a refit to start from, so this frame is a build for
  // everyone no matter what the poses did
  self.build_modes.assign(infos.size(), SkinnedBLASBuildMode::Rebuild);
  self.refit_counts.assign(infos.size(), 0);

  self.handles.clear();
  self.buffer.reset();
  self.scratch_buffer.reset();
  self.device_addresses.assign(infos.size(), 0);
  self.scratch_offsets.assign(infos.size(), 0);
  self.address_min = 0;
  self.address_max = 0;
  self.layout_key = key;

  auto as_offsets = stack.alloc<u64>(infos.size());
  auto as_sizes = stack.alloc<u64>(infos.size());
  auto as_total = 0_u64;
  auto scratch_total = 0_u64;
  const auto scratch_alignment = render_context.as_scratch_alignment();

  for (usize i = 0; i < infos.size(); i++) {
    as_offsets[i] = 0;
    as_sizes[i] = 0;

    const auto& info = infos[i];
    if (!is_buildable(info)) {
      continue;
    }

    const auto primitive_count = info.index_count / 3;
    const auto geometry = make_triangle_geometry(info);
    auto build_info = VkAccelerationStructureBuildGeometryInfoKHR{
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
      .pNext = nullptr,
      .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
      .flags = SKINNED_BLAS_BUILD_FLAGS,
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

    as_offsets[i] = as_total;
    as_sizes[i] = size_info.accelerationStructureSize;
    as_total += align_up(size_info.accelerationStructureSize, AS_BUFFER_ALIGNMENT);

    // one arena serves both modes, so the slice has to fit whichever is larger. In practice the
    // update size is far smaller, but nothing in the spec promises that
    self.scratch_offsets[i] = scratch_total;
    scratch_total += align_up(ox::max(size_info.buildScratchSize, size_info.updateScratchSize), scratch_alignment);
  }

  if (as_total == 0) {
    return false;
  }

  self.buffer = render_context.allocate_buffer_super(vuk::MemoryUsage::eGPUonly, as_total, AS_BUFFER_ALIGNMENT);
  self.scratch_buffer = render_context
                          .allocate_buffer_super(vuk::MemoryUsage::eGPUonly, scratch_total, scratch_alignment);

  auto& allocator = render_context.superframe_allocator.value();
  self.handles.resize(infos.size());
  for (usize i = 0; i < infos.size(); i++) {
    if (as_sizes[i] == 0) {
      continue;
    }

    auto create_info = VkAccelerationStructureCreateInfoKHR{
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
      .pNext = nullptr,
      .createFlags = 0,
      .buffer = self.buffer->buffer,
      .offset = self.buffer->offset + as_offsets[i],
      .size = as_sizes[i],
      .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
      .deviceAddress = 0,
    };

    self.handles[i] = vuk::Unique<VkAccelerationStructureKHR>(allocator);
    if (!allocator.allocate_acceleration_structures({&*self.handles[i], 1}, {&create_info, 1})) {
      OX_LOG_ERROR("Failed to allocate skinned bottom level acceleration structure.");
      continue;
    }

    const auto address = render_context.get_accel_structure_device_address(*self.handles[i]);
    self.device_addresses[i] = address;
    self.address_min = self.address_min == 0 ? address : ox::min(self.address_min, address);
    self.address_max = ox::max(self.address_max, address);
  }

  return true;
}

auto build_skinned_blases(
  RenderContext& render_context,
  SkinnedBLASPool& pool,
  const SkinnedBLASBudget& budget,
  vuk::Value<vuk::Buffer>& skinned_vertices
) -> vuk::Value<vuk::Buffer> {
  ZoneScoped;
  memory::ScopedStack selection_stack;

  if (
    !render_context.use_ray_tracing() || !pool.buffer || !pool.scratch_buffer || pool.builds.empty() ||
    pool.handles.size() != pool.builds.size() || pool.build_modes.size() != pool.builds.size() ||
    pool.refit_counts.size() != pool.builds.size() || skinned_vertices.node == nullptr
  ) {
    return {};
  }

  const auto entry_count = pool.builds.size();
  const auto primitives_of = [&pool](const u32 index) {
    return static_cast<u64>(pool.builds[index].index_count / 3);
  };

  auto workable = selection_stack.alloc<u32>(entry_count);
  auto workable_count = usize{0};
  for (usize i = 0; i < entry_count; i++) {
    if (pool.device_addresses[i] == 0 || !is_buildable(pool.builds[i])) {
      pool.build_modes[i] = SkinnedBLASBuildMode::None;
      continue;
    }

    // a character that stopped moving keeps whatever tree its last refit left behind, so it stays a
    // candidate with nothing asking for work: an idle frame is exactly when the budget is free
    if (pool.build_modes[i] == SkinnedBLASBuildMode::None && pool.refit_counts[i] == 0) {
      continue;
    }

    workable[workable_count++] = static_cast<u32>(i);
  }

  if (workable_count == 0) {
    return {};
  }

  auto remaining_budget = budget.rebuild_primitive_budget == 0 ? std::numeric_limits<u64>::max()
                                                               : static_cast<u64>(budget.rebuild_primitive_budget);

  // a structure with no tree cannot refit, so it builds whatever the budget says. It still spends
  // from it, so the discretionary promotions below do not pile onto a frame that is already full
  for (usize k = 0; k < workable_count; k++) {
    const auto index = workable[k];
    if (pool.build_modes[index] != SkinnedBLASBuildMode::Rebuild) {
      continue;
    }
    remaining_budget -= ox::min(remaining_budget, primitives_of(index));
  }

  const auto refit_limit = ox::max(budget.max_refits_before_rebuild, 1_u32);
  auto candidates = selection_stack.alloc<u32>(workable_count);
  auto candidate_count = usize{0};
  for (usize k = 0; k < workable_count; k++) {
    const auto index = workable[k];
    const auto mode = pool.build_modes[index];
    if (mode == SkinnedBLASBuildMode::Rebuild) {
      continue;
    }

    const auto due = mode == SkinnedBLASBuildMode::None ? pool.refit_counts[index] > 0
                                                        : pool.refit_counts[index] >= refit_limit;
    if (due) {
      candidates[candidate_count++] = index;
    }
  }

  // staleest first, so a frame that can only afford one full build spends it on the tree that has
  // drifted furthest from the pose it was built at
  std::sort(
    candidates.begin(),
    candidates.begin() + static_cast<std::ptrdiff_t>(candidate_count),
    [&pool](const u32 lhs, const u32 rhs) { return pool.refit_counts[lhs] > pool.refit_counts[rhs]; }
  );

  for (usize k = 0; k < candidate_count; k++) {
    const auto index = candidates[k];
    const auto primitives = primitives_of(index);
    // not a break: a cheaper structure further down the list still fits in what is left
    if (primitives > remaining_budget) {
      continue;
    }

    remaining_budget -= primitives;
    pool.build_modes[index] = SkinnedBLASBuildMode::Rebuild;
  }

  struct Build {
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    u64 vertex_positions = 0;
    u64 indices = 0;
    u64 scratch_address = 0;
    u32 vertex_count = 0;
    u32 primitive_count = 0;
    bool refit = false;
  };

  const auto scratch_address = pool.scratch_buffer->device_address;
  auto pending_builds = std::vector<Build>();
  pending_builds.reserve(workable_count);
  for (usize k = 0; k < workable_count; k++) {
    const auto index = workable[k];
    const auto mode = pool.build_modes[index];
    if (mode == SkinnedBLASBuildMode::None) {
      continue;
    }

    pool.build_modes[index] = SkinnedBLASBuildMode::None;
    const auto refit = mode == SkinnedBLASBuildMode::Refit;
    pool.refit_counts[index] = refit ? pool.refit_counts[index] + 1 : 0;

    const auto& info = pool.builds[index];
    pending_builds.emplace_back(
      Build{
        .handle = *pool.handles[index],
        .vertex_positions = info.vertex_positions,
        .indices = info.indices,
        .scratch_address = scratch_address + pool.scratch_offsets[index],
        .vertex_count = info.vertex_count,
        .primitive_count = info.index_count / 3,
        .refit = refit,
      }
    );
  }

  if (pending_builds.empty()) {
    return {};
  }

  auto blas_buffer = vuk::discard_buf("skinned blas", *pool.buffer);
  auto build_pass = vuk::make_pass(
    "skinned blas build",
    [builds = std::move(pending_builds)](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eAccelerationStructureBuildRW) blas,
      VUK_BA(vuk::eAccelerationStructureBuildRead) vertices
    ) {
      memory::ScopedStack stack;

      auto geometries = stack.alloc<VkAccelerationStructureGeometryKHR>(builds.size());
      auto build_infos = stack.alloc<VkAccelerationStructureBuildGeometryInfoKHR>(builds.size());
      auto ranges = stack.alloc<VkAccelerationStructureBuildRangeInfoKHR>(builds.size());
      auto range_ptrs = stack.alloc<const VkAccelerationStructureBuildRangeInfoKHR*>(builds.size());

      for (usize i = 0; i < builds.size(); i++) {
        const auto& build = builds[i];

        geometries[i] = make_triangle_geometry(
          BLASBuildInfo{
            .vertex_positions = build.vertex_positions,
            .indices = build.indices,
            .vertex_count = build.vertex_count,
            .index_count = build.primitive_count * 3,
          }
        );

        build_infos[i] = {
          .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
          .pNext = nullptr,
          .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
          .flags = SKINNED_BLAS_BUILD_FLAGS,
          .mode = build.refit ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                              : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
          // refits are in place, which is why the pass takes the storage read write: the tree being
          // rewritten is also the tree being read
          .srcAccelerationStructure = build.refit ? build.handle : VK_NULL_HANDLE,
          .dstAccelerationStructure = build.handle,
          .geometryCount = 1,
          .pGeometries = &geometries[i],
          .ppGeometries = nullptr,
          .scratchData = {.deviceAddress = build.scratch_address},
        };

        ranges[i] = {
          .primitiveCount = build.primitive_count,
          .primitiveOffset = 0,
          .firstVertex = 0,
          .transformOffset = 0,
        };
        range_ptrs[i] = &ranges[i];
      }

      cmd_list.build_acceleration_structures(static_cast<u32>(builds.size()), build_infos.data(), range_ptrs.data());

      return blas;
    }
  );

  return build_pass(std::move(blas_buffer), std::move(skinned_vertices));
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
    [instance_count = info.instance_count,
     blas_address_count = info.blas_address_count,
     address_min = info.skinned_blas_address_min,
     address_max = info.skinned_blas_address_max](
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
        .push_constants(
          vuk::ShaderStageFlagBits::eCompute,
          0,
          PushConstants(address_min, address_max, instance_count, blas_address_count)
        )
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

  auto record = [build_info, geometry, instance_count = info.instance_count](vuk::CommandBuffer& cmd_list) mutable {
    build_info.pGeometries = &geometry;

    const auto range = VkAccelerationStructureBuildRangeInfoKHR{
      .primitiveCount = instance_count,
      .primitiveOffset = 0,
      .firstVertex = 0,
      .transformOffset = 0,
    };
    const auto* range_ptr = &range;
    cmd_list.build_acceleration_structures(1, &build_info, &range_ptr);
  };

  auto tlas_buffer = vuk::discard_buf("tlas", *scene_tlas.acceleration_structure.buffer);

  // the skinned structures are built the same frame, so the top level build has to be ordered
  // against them, while a scene with nothing skinned only reads structures built at load time
  if (info.skinned_blas_buffer.node != nullptr) {
    auto build_pass = vuk::make_pass(
      "tlas build",
      [record](
        vuk::CommandBuffer& cmd_list,
        VUK_BA(vuk::eAccelerationStructureBuildWrite) tlas,
        VUK_BA(vuk::eAccelerationStructureBuildRead) instances,
        VUK_BA(vuk::eAccelerationStructureBuildRead) skinned_blas
      ) mutable {
        record(cmd_list);

        return tlas;
      }
    );

    return build_pass(std::move(tlas_buffer), std::move(instances_buffer), std::move(info.skinned_blas_buffer));
  }

  auto build_pass = vuk::make_pass(
    "tlas build",
    [record](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eAccelerationStructureBuildWrite) tlas,
      VUK_BA(vuk::eAccelerationStructureBuildRead) instances
    ) mutable {
      record(cmd_list);

      return tlas;
    }
  );

  return build_pass(std::move(tlas_buffer), std::move(instances_buffer));
}
} // namespace ox
