#include "Render/AccelerationStructure.hpp"

#include <algorithm>
#include <ankerl/svector.h>
#include <glm/glm.hpp>
#include <limits>
#include <vuk/RenderGraph.hpp>
#include <vuk/runtime/CommandBuffer.hpp>
#include <vuk/runtime/vk/AllocatorHelpers.hpp>

#include "Core/Base.hpp"
#include "Memory/Stack.hpp"
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

// same pairing rule as above. ALLOW_UPDATE is what lets an unchanged topology refit rather than
// rebuild. No ALLOW_COMPACTION: these are rewritten in place every few frames, so the query pool
// round trip that pays for itself on a load time structure would be paid over and over here. The
// slow builder is deliberate even though this is touched per frame: full builds are rare and
// staggered, and every refit between them inherits whatever tree the last one produced
constexpr static auto SKINNED_BLAS_BUILD_FLAGS = VkBuildAccelerationStructureFlagsKHR{
  VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR
};

// same pairing rule: SceneTLAS::reserve sizes with these and build_scene_tlas builds with them.
// Deliberately the slow builder even though this one is rebuilt every frame: FAST_BUILD measured as
// no wall clock win and a slightly worse wavefront profile, and unlike a bottom level structure
// every ray traverses this one
constexpr static auto TLAS_BUILD_FLAGS = VkBuildAccelerationStructureFlagsKHR{
  VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR
};

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

  auto geometry = make_triangle_geometry(info);

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

constexpr static auto SKINNED_ARENA_MIN_SIZE = 1_u64 << 20;

static auto query_skinned_blas_sizes(RenderContext& render_context, const BLASBuildInfo& info)
  -> VkAccelerationStructureBuildSizesInfoKHR {
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

  return size_info;
}

static auto take_free_block(std::vector<SkinnedBLASPool::Block>& free_blocks, u64 size) -> option<u64> {
  for (usize i = 0; i < free_blocks.size(); i++) {
    if (free_blocks[i].size < size) {
      continue;
    }

    const auto offset = free_blocks[i].offset;
    free_blocks[i].offset += size;
    free_blocks[i].size -= size;
    if (free_blocks[i].size == 0) {
      free_blocks.erase(free_blocks.begin() + static_cast<std::ptrdiff_t>(i));
    }

    return offset;
  }

  return nullopt;
}

static auto give_free_block(std::vector<SkinnedBLASPool::Block>& free_blocks, u64 offset, u64 size) -> void {
  if (size == 0) {
    return;
  }

  const auto it = std::ranges::lower_bound(free_blocks, offset, {}, &SkinnedBLASPool::Block::offset);
  const auto inserted = free_blocks.insert(it, SkinnedBLASPool::Block{.offset = offset, .size = size});

  // coalesce with the neighbour on each side so a churning crowd does not shred the arena
  auto index = static_cast<usize>(inserted - free_blocks.begin());
  if (
    index + 1 < free_blocks.size() &&
    free_blocks[index].offset + free_blocks[index].size == free_blocks[index + 1].offset
  ) {
    free_blocks[index].size += free_blocks[index + 1].size;
    free_blocks.erase(free_blocks.begin() + static_cast<std::ptrdiff_t>(index) + 1);
  }
  if (index > 0 && free_blocks[index - 1].offset + free_blocks[index - 1].size == free_blocks[index].offset) {
    free_blocks[index - 1].size += free_blocks[index].size;
    free_blocks.erase(free_blocks.begin() + static_cast<std::ptrdiff_t>(index));
  }
}

static auto create_pool_handle(RenderContext& render_context, SkinnedBLASPool& pool, SkinnedBLASPool::Entry& entry)
  -> bool {
  auto create_info = VkAccelerationStructureCreateInfoKHR{
    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
    .pNext = nullptr,
    .createFlags = 0,
    .buffer = pool.buffer->buffer,
    .offset = pool.buffer->offset + entry.offset,
    .size = entry.size,
    .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
    .deviceAddress = 0,
  };

  auto& allocator = render_context.superframe_allocator.value();
  entry.handle = vuk::Unique<VkAccelerationStructureKHR>(allocator);
  if (!allocator.allocate_acceleration_structures({&*entry.handle, 1}, {&create_info, 1})) {
    OX_LOG_ERROR("Failed to allocate skinned bottom level acceleration structure.");
    entry.handle.reset();
    entry.device_address = 0;
    return false;
  }

  entry.device_address = render_context.get_accel_structure_device_address(*entry.handle);
  // a fresh handle over fresh memory holds no tree, so nothing may refit from it and its address
  // stays out of the TLAS until a build has actually run
  entry.built_once = false;
  entry.refits_since_build = 0;

  return true;
}

// a handle a frame in flight may still be traversing cannot be destroyed on the spot, so every
// release goes through here and is collected a few frames later
static auto retire_handle(
  RenderContext& render_context, SkinnedBLASPool& pool, vuk::Unique<VkAccelerationStructureKHR>&& handle
) -> void {
  if (!handle) {
    return;
  }

  if (pool.retired.empty() || pool.retired.back().frame != render_context.num_frames) {
    pool.retired.emplace_back(SkinnedBLASPool::Retired{.frame = render_context.num_frames});
  }

  pool.retired.back().handles.emplace_back(std::move(handle));
}

// replaces the arena with a larger one and re-places every live entry in it. Everything loses its
// tree, which is the safe direction: an entry with no tree publishes no address
static auto grow_skinned_arena(RenderContext& render_context, SkinnedBLASPool& pool, u64 required) -> bool {
  ZoneScoped;

  auto live = 0_u64;
  for (const auto& entry : pool.entries) {
    if (entry.handle) {
      live += align_up(entry.size, AS_BUFFER_ALIGNMENT);
    }
  }

  const auto capacity = ox::max(ox::max(live + required, pool.capacity * 2), SKINNED_ARENA_MIN_SIZE);

  auto retired = SkinnedBLASPool::Retired{.frame = render_context.num_frames};
  retired.buffer = std::move(pool.buffer);
  for (auto& entry : pool.entries) {
    if (entry.handle) {
      retired.handles.emplace_back(std::move(entry.handle));
    }
  }
  if (retired.buffer || !retired.handles.empty()) {
    pool.retired.emplace_back(std::move(retired));
  }

  pool.buffer = render_context.allocate_buffer_super(vuk::MemoryUsage::eGPUonly, capacity, AS_BUFFER_ALIGNMENT);
  if (!pool.buffer) {
    pool.capacity = 0;
    return false;
  }

  pool.capacity = capacity;
  pool.bump = 0;
  pool.free_blocks.clear();

  for (u32 index = 0; index < static_cast<u32>(pool.entries.size()); index++) {
    auto& entry = pool.entries[index];
    if (entry.size == 0) {
      continue;
    }

    entry.offset = pool.bump;
    pool.bump += align_up(entry.size, AS_BUFFER_ALIGNMENT);
    if (!create_pool_handle(render_context, pool, entry)) {
      pool.slot_to_entry.erase(entry.mesh_instance_slot);
      entry = {};
      pool.free_entries.push_back(index);
    }
  }

  return true;
}

// retires rather than drops, because a frame still in flight may be reading both the arena and the
// handles that were created over it
auto SkinnedBLASPool::reset(this SkinnedBLASPool& self, RenderContext& render_context) -> void {
  auto retired = Retired{.frame = render_context.num_frames};
  retired.buffer = std::move(self.buffer);
  for (auto& entry : self.entries) {
    if (entry.handle) {
      retired.handles.emplace_back(std::move(entry.handle));
    }
  }
  if (retired.buffer || !retired.handles.empty()) {
    self.retired.emplace_back(std::move(retired));
  }
  if (self.scratch_buffer) {
    self.retired.emplace_back(Retired{.buffer = std::move(self.scratch_buffer), .frame = render_context.num_frames});
  }

  self.entries.clear();
  self.free_entries.clear();
  self.free_blocks.clear();
  self.slot_to_entry.clear();
  self.pending_rebuilt.clear();
  self.pending_refit.clear();
  self.capacity = 0;
  self.bump = 0;

  self.collect_retired(render_context);
}

auto SkinnedBLASPool::collect_retired(this SkinnedBLASPool& self, RenderContext& render_context) -> void {
  // a replaced arena has to outlive every frame that could still be recording or executing against
  // it, which is the in flight count plus the frame doing the replacing
  const auto window = static_cast<u64>(render_context.num_inflight_frames) + 1;
  std::erase_if(self.retired, [&render_context, window](const Retired& entry) {
    return render_context.num_frames - entry.frame >= window;
  });
}

auto SkinnedBLASPool::address_of(this const SkinnedBLASPool& self, const u32 mesh_instance_slot) -> u64 {
  const auto it = self.slot_to_entry.find(mesh_instance_slot);
  if (it == self.slot_to_entry.end()) {
    return 0;
  }

  const auto& entry = self.entries[it->second];
  return entry.built_once ? entry.device_address : 0;
}

auto SkinnedBLASPool::commit_builds(this SkinnedBLASPool& self) -> void {
  for (const auto index : self.pending_refit) {
    self.entries[index].refits_since_build += 1;
  }
  for (const auto index : self.pending_rebuilt) {
    self.entries[index].refits_since_build = 0;
    self.entries[index].built_once = true;
  }

  self.pending_refit.clear();
  self.pending_rebuilt.clear();
}

auto SkinnedBLASPool::sync(
  this SkinnedBLASPool& self,
  RenderContext& render_context,
  const std::span<const SkinnedMeshInstance> instances,
  const u64 arena_address
) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  if (!render_context.use_ray_tracing()) {
    self.reset(render_context);
    return;
  }

  self.collect_retired(render_context);

  // anything a build was staged for last frame but never committed is not built, and the flags below
  // are rebuilt from scratch anyway
  self.pending_rebuilt.clear();
  self.pending_refit.clear();

  for (auto& entry : self.entries) {
    entry.alive = false;
    entry.pose_advanced = false;
  }

  // matching first, releasing second, allocating third: a growth in the third phase only has to
  // re-place entries that are still live, and never one that is about to be dropped
  auto pending = stack.alloc<u32>(instances.size());
  auto pending_count = 0_sz;
  for (usize i = 0; i < instances.size(); i++) {
    const auto& skinned = instances[i];
    const auto info = BLASBuildInfo{
      .vertex_positions = arena_address + static_cast<u64>(skinned.vertex_offset) * BLAS_VERTEX_STRIDE,
      .indices = skinned.index_address,
      .vertex_count = skinned.vertex_count,
      .index_count = skinned.index_count,
    };
    // no skeleton yet means nothing wrote this instance's slice of the arena
    if (skinned.bone_count == 0 || !is_buildable(info)) {
      continue;
    }

    const auto it = self.slot_to_entry.find(skinned.mesh_instance_slot);
    if (it != self.slot_to_entry.end()) {
      auto& entry = self.entries[it->second];
      // a refit may be pointed at moved vertices, which is all an arena resize does, but it may not
      // be pointed at different geometry, so a model reloading under a slot invalidates the entry
      if (
        entry.handle && entry.indices == info.indices && entry.vertex_count == info.vertex_count &&
        entry.index_count == info.index_count
      ) {
        entry.alive = true;
        entry.pose_advanced = skinned.pose_advanced;
        entry.vertex_positions = info.vertex_positions;
        continue;
      }
    }

    pending[pending_count++] = static_cast<u32>(i);
  }

  for (u32 index = 0; index < static_cast<u32>(self.entries.size()); index++) {
    auto& entry = self.entries[index];
    if (entry.alive || entry.size == 0) {
      continue;
    }

    self.slot_to_entry.erase(entry.mesh_instance_slot);
    give_free_block(self.free_blocks, entry.offset, align_up(entry.size, AS_BUFFER_ALIGNMENT));
    retire_handle(render_context, self, std::move(entry.handle));
    entry = {};
    self.free_entries.push_back(index);
  }

  // ten characters sharing a model give ten identical answers, and the query is a driver round trip
  auto size_cache = ankerl::unordered_dense::map<u64, VkAccelerationStructureBuildSizesInfoKHR>();
  for (usize k = 0; k < pending_count; k++) {
    const auto& skinned = instances[pending[k]];
    const auto info = BLASBuildInfo{
      .vertex_positions = arena_address + static_cast<u64>(skinned.vertex_offset) * BLAS_VERTEX_STRIDE,
      .indices = skinned.index_address,
      .vertex_count = skinned.vertex_count,
      .index_count = skinned.index_count,
    };

    const auto size_key = (static_cast<u64>(info.vertex_count) << 32) | info.index_count;
    auto size_it = size_cache.find(size_key);
    if (size_it == size_cache.end()) {
      size_it = size_cache.emplace(size_key, query_skinned_blas_sizes(render_context, info)).first;
    }

    const auto& size_info = size_it->second;
    if (size_info.accelerationStructureSize == 0) {
      continue;
    }

    const auto block_size = align_up(size_info.accelerationStructureSize, AS_BUFFER_ALIGNMENT);
    auto offset = take_free_block(self.free_blocks, block_size);
    if (!offset && self.buffer && self.bump + block_size <= self.capacity) {
      offset = self.bump;
      self.bump += block_size;
    }
    if (!offset) {
      if (!grow_skinned_arena(render_context, self, block_size)) {
        continue;
      }

      offset = self.bump;
      self.bump += block_size;
    }

    auto index = 0_u32;
    if (self.free_entries.empty()) {
      index = static_cast<u32>(self.entries.size());
      self.entries.emplace_back();
    } else {
      index = self.free_entries.back();
      self.free_entries.pop_back();
    }

    auto& entry = self.entries[index];
    entry.offset = *offset;
    entry.size = size_info.accelerationStructureSize;
    entry.build_scratch_size = size_info.buildScratchSize;
    entry.update_scratch_size = size_info.updateScratchSize;
    entry.vertex_positions = info.vertex_positions;
    entry.indices = info.indices;
    entry.vertex_count = info.vertex_count;
    entry.index_count = info.index_count;
    entry.mesh_instance_slot = skinned.mesh_instance_slot;
    entry.alive = true;
    entry.pose_advanced = skinned.pose_advanced;

    if (!create_pool_handle(render_context, self, entry)) {
      give_free_block(self.free_blocks, entry.offset, block_size);
      retire_handle(render_context, self, std::move(entry.handle));
      entry = {};
      self.free_entries.push_back(index);
      continue;
    }

    self.slot_to_entry.insert_or_assign(skinned.mesh_instance_slot, index);
  }
}

auto build_skinned_blases(
  RenderContext& render_context,
  SkinnedBLASPool& pool,
  const SkinnedBLASBudget& budget,
  vuk::Value<vuk::Buffer>&& skinned_vertices
) -> vuk::Value<vuk::Buffer> {
  ZoneScoped;
  memory::ScopedStack stack;

  if (!render_context.use_ray_tracing() || !pool.buffer || pool.entries.empty() || skinned_vertices.node == nullptr) {
    return {};
  }

  const auto entry_count = pool.entries.size();
  const auto primitives_of = [&pool](const u32 index) {
    return static_cast<u64>(pool.entries[index].index_count / 3);
  };

  // a structure with no tree cannot refit, so it takes the budget first: until it lands its owner is
  // not in the TLAS at all, whereas an already built one is merely getting stale
  auto unbuilt = stack.alloc<u32>(entry_count);
  auto unbuilt_count = 0_sz;
  auto stale = stack.alloc<u32>(entry_count);
  auto stale_count = 0_sz;
  auto refits = stack.alloc<u32>(entry_count);
  auto refit_count = 0_sz;

  const auto refit_limit = ox::max(budget.max_refits_before_rebuild, 1_u32);
  for (u32 index = 0; index < static_cast<u32>(entry_count); index++) {
    const auto& entry = pool.entries[index];
    if (!entry.alive || !entry.handle || entry.device_address == 0) {
      continue;
    }

    if (!entry.built_once) {
      unbuilt[unbuilt_count++] = index;
    } else if (entry.refits_since_build >= refit_limit) {
      stale[stale_count++] = index;
    } else if (entry.pose_advanced) {
      refits[refit_count++] = index;
    }
  }

  if (unbuilt_count == 0 && stale_count == 0 && refit_count == 0) {
    return {};
  }

  // cheapest first among the unbuilt, so a crowd comes online in as few frames as possible, and
  // stalest first among the rest, so a frame that can only afford one spends it on the tree that has
  // drifted furthest from the pose it was built at
  const auto by_primitives = [&primitives_of](const u32 lhs, const u32 rhs) {
    return primitives_of(lhs) < primitives_of(rhs);
  };
  const auto by_staleness = [&pool](const u32 lhs, const u32 rhs) {
    return pool.entries[lhs].refits_since_build > pool.entries[rhs].refits_since_build;
  };
  std::sort(unbuilt.begin(), unbuilt.begin() + static_cast<std::ptrdiff_t>(unbuilt_count), by_primitives);
  std::sort(stale.begin(), stale.begin() + static_cast<std::ptrdiff_t>(stale_count), by_staleness);

  struct Build {
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    u64 vertex_positions = 0;
    u64 indices = 0;
    u64 scratch_offset = 0;
    u32 vertex_count = 0;
    u32 primitive_count = 0;
    bool refit = false;
  };

  auto pending = stack.alloc<Build>(entry_count);
  auto pending_count = 0_sz;
  auto scratch_total = 0_u64;
  const auto scratch_alignment = render_context.as_scratch_alignment();

  // the builds in one command run concurrently, so every one of them needs its own scratch region
  const auto stage = [&](const u32 index, const bool refit) {
    const auto& entry = pool.entries[index];
    const auto scratch_size = refit ? entry.update_scratch_size : entry.build_scratch_size;
    pending[pending_count++] = Build{
      .handle = *entry.handle,
      .vertex_positions = entry.vertex_positions,
      .indices = entry.indices,
      .scratch_offset = scratch_total,
      .vertex_count = entry.vertex_count,
      .primitive_count = entry.index_count / 3,
      .refit = refit,
    };
    scratch_total += align_up(scratch_size, scratch_alignment);

    if (refit) {
      pool.pending_refit.push_back(index);
    } else {
      pool.pending_rebuilt.push_back(index);
    }
  };

  auto remaining_budget = budget.rebuild_primitive_budget == 0 ? std::numeric_limits<u64>::max()
                                                               : static_cast<u64>(budget.rebuild_primitive_budget);
  for (usize k = 0; k < unbuilt_count; k++) {
    const auto index = unbuilt[k];
    const auto primitives = primitives_of(index);
    // not a break: a cheaper structure further down the list still fits in what is left
    if (primitives > remaining_budget) {
      continue;
    }

    remaining_budget -= primitives;
    stage(index, false);
  }
  for (usize k = 0; k < stale_count; k++) {
    const auto index = stale[k];
    const auto primitives = primitives_of(index);
    if (primitives > remaining_budget) {
      // over budget this frame, so it keeps refitting rather than sitting on a frozen tree
      if (pool.entries[index].pose_advanced) {
        stage(index, true);
      }
      continue;
    }

    remaining_budget -= primitives;
    stage(index, false);
  }
  for (usize k = 0; k < refit_count; k++) {
    stage(refits[k], true);
  }

  if (pending_count == 0) {
    return {};
  }

  // not resize_buffer: that allocates at the default alignment, and a scratch address the device
  // rejects is a driver crash rather than a validation message
  if (!pool.scratch_buffer || pool.scratch_buffer->size < scratch_total) {
    const auto scratch_capacity = ox::max(scratch_total, pool.scratch_buffer ? pool.scratch_buffer->size * 2 : 0_u64);
    pool.scratch_buffer.reset();
    pool.scratch_buffer = render_context
                            .allocate_buffer_super(vuk::MemoryUsage::eGPUonly, scratch_capacity, scratch_alignment);
  }
  if (!pool.scratch_buffer) {
    pool.pending_refit.clear();
    pool.pending_rebuilt.clear();
    return {};
  }

  const auto scratch_address = pool.scratch_buffer->device_address;
  for (usize i = 0; i < pending_count; i++) {
    pending[i].scratch_offset += scratch_address;
  }

  // acquire, never discard: a refit reads the tree already sitting in this memory. The declared
  // incoming access stands in for the edges vuk cannot see by itself, and there are two of them.
  // The previous frame's build wrote here, and the previous frame's rays traversed here without ever
  // naming this buffer, because rtao and ddgi_trace take only the TLAS as a pass resource. Drop the
  // ray tracing read and an in place refit is free to run while last frame's traversal is still in it
  auto blas_buffer = vuk::acquire_buf(
    "skinned blas",
    *pool.buffer,
    vuk::Access::eAccelerationStructureBuildWrite | vuk::Access::eRayTracingRead
  );
  // the scratch is addressed by device address rather than bound, so it rides as a pass parameter
  // purely to make vuk serialise this frame's builds against the previous frame's
  auto scratch_buffer = vuk::acquire_buf(
    "skinned blas scratch",
    *pool.scratch_buffer,
    vuk::Access::eAccelerationStructureBuildWrite
  );

  auto build_pass = vuk::make_pass(
    "skinned blas build",
    [builds = ankerl::svector<Build, 8>(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(pending_count))](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eAccelerationStructureBuildRW) blas,
      VUK_BA(vuk::eAccelerationStructureBuildWrite) scratch,
      VUK_BA(vuk::eAccelerationStructureBuildRead) vertices
    ) {
      memory::ScopedStack pass_stack;

      auto geometries = pass_stack.alloc<VkAccelerationStructureGeometryKHR>(builds.size());
      auto build_infos = pass_stack.alloc<VkAccelerationStructureBuildGeometryInfoKHR>(builds.size());
      auto ranges = pass_stack.alloc<VkAccelerationStructureBuildRangeInfoKHR>(builds.size());
      auto range_ptrs = pass_stack.alloc<const VkAccelerationStructureBuildRangeInfoKHR*>(builds.size());

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
          .scratchData = {.deviceAddress = build.scratch_offset},
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

  return build_pass(std::move(blas_buffer), std::move(scratch_buffer), std::move(skinned_vertices));
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
    // the skinned builds were threaded in here to be submitted, so dropping them on the floor would
    // leave an orphan subgraph behind as well as structures nothing ever built
    if (info.skinned_blas_buffer.node != nullptr) {
      render_context.wait_on(std::move(info.skinned_blas_buffer));
    }

    return {};
  }

  // the shader does not read this. It rides along so the skinned structure builds land in the
  // subgraph the wait below submits, which is what makes them run at all and what makes them
  // complete before this build dereferences their addresses. A stand-in keeps the pass signature
  // fixed on a frame where nothing was recorded
  auto skinned_blas_buffer = info.skinned_blas_buffer.node != nullptr ? std::move(info.skinned_blas_buffer)
                                                                      : render_context.scratch_buffer<u32>(0u);

  auto instances_buffer = vuk::discard_buf("tlas instances", *scene_tlas.instances_buffer);
  auto write_instances_pass = vuk::make_pass(
    "tlas write instances",
    [counts = glm::uvec2(info.instance_count, info.skinned_instance_count)](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eComputeWrite) instances,
      VUK_BA(vuk::eComputeRead) mesh_instances,
      VUK_BA(vuk::eComputeRead) transforms,
      VUK_BA(vuk::eComputeRead) blas_addresses,
      VUK_BA(vuk::eComputeRead) skinned_blas_addresses,
      VUK_BA(vuk::eAccelerationStructureBuildRead) skinned_blas
    ) {
      cmd_list //
        .bind_compute_pipeline("tlas_write_instances")
        .bind_buffer(0, 0, mesh_instances)
        .bind_buffer(0, 1, transforms)
        .bind_buffer(0, 2, blas_addresses)
        .bind_buffer(0, 3, instances)
        .bind_buffer(0, 4, skinned_blas_addresses)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, counts)
        .dispatch_invocations(counts.x);

      return instances;
    }
  );

  instances_buffer = write_instances_pass(
    std::move(instances_buffer),
    std::move(info.mesh_instances_buffer),
    std::move(info.transforms_buffer),
    std::move(info.blas_addresses_buffer),
    std::move(info.skinned_blas_addresses_buffer),
    std::move(skinned_blas_buffer)
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
