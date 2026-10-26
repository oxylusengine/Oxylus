#include "Render/AccelerationStructure.hpp"

#include <algorithm>
#include <ankerl/svector.h>
#include <bit>
#include <glm/glm.hpp>
#include <limits>
#include <vir/Allocator.hpp>
#include <vir/Context.hpp>
#include <vir/Module.hpp>
#include <vir/Program.hpp>
#include <vir/Types.hpp>

#include "Core/Base.hpp"
#include "Core/Option.hpp"
#include "Memory/Stack.hpp"
#include "Render/RenderContext.hpp"
#include "Scene/SceneGPU.hpp"
#include "Utils/Log.hpp"

namespace ox {
constexpr static auto BLAS_VERTEX_FORMAT = vir::Format::RGBA16Sfloat;
constexpr static auto BLAS_VERTEX_STRIDE = 8_u64;
// fixed by the spec rather than a device limit: VkAccelerationStructureCreateInfoKHR::offset must be
// a multiple of 256 and there is nothing to query it from. Scratch is the opposite case and reads
// minAccelerationStructureScratchOffsetAlignment through RenderContext::as_scratch_alignment
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
           .vertexFormat = static_cast<VkFormat>(BLAS_VERTEX_FORMAT),
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

static auto query_blas_sizes(
  RenderContext& render_context, const BLASBuildInfo& info, VkBuildAccelerationStructureFlagsKHR flags
) -> VkAccelerationStructureBuildSizesInfoKHR {
  const auto primitive_count = info.index_count / 3;
  const auto geometry = make_triangle_geometry(info);
  const auto build_info = VkAccelerationStructureBuildGeometryInfoKHR{
    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
    .pNext = nullptr,
    .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
    .flags = flags,
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
  render_context.context->fn.vkGetAccelerationStructureBuildSizesKHR(
    render_context.device,
    VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
    &build_info,
    &primitive_count,
    &size_info
  );

  return size_info;
}

// the copy is sized off a query that has to reach the host first, so it is a submission of its own.
// The structure handed in is retired once the compacted one replaces it
static auto compact_blas(RenderContext& render_context, AccelerationStructureID blas_id, u64 compacted_size)
  -> AccelerationStructureID {
  ZoneScoped;

  const auto blas = render_context.acceleration_structure(blas_id);
  if (compacted_size == 0 || compacted_size >= blas.size) {
    return blas_id;
  }

  const auto compacted_id = render_context.allocate_acceleration_structure(
    VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
    compacted_size,
    "blas compacted"
  );
  if (!compacted_id) {
    return blas_id;
  }

  const auto compacted = render_context.acceleration_structure(*compacted_id);
  auto module = vir::Module{};
  const auto source = module.import_buffer(
    render_context.buffer(blas.buffer),
    vir::Access::AccelerationStructureBuildRead
  );
  const auto copied = module.copy_acceleration_structure(
    source,
    module.import_buffer(render_context.buffer(compacted.buffer), vir::Access::None),
    blas.handle,
    compacted.handle
  );
  module.export_to(copied, vir::Access::AccelerationStructureBuildRead);
  if (const auto executed = render_context.execute_blocking(std::move(module)); !executed) {
    OX_LOG_ERROR("Failed to compact a BLAS ({}).", std::to_underlying(executed.error()));
    render_context.destroy_acceleration_structure(*compacted_id);
    return blas_id;
  }

  render_context.destroy_acceleration_structure(blas_id);
  return *compacted_id;
}

auto build_mesh_blas(
  RenderContext& render_context, BufferID mesh_buffer, vir::Access mesh_rests_at, const BLASBuildInfo& info
) -> AccelerationStructureID {
  ZoneScoped;

  if (!render_context.use_ray_tracing() || !is_buildable(info)) {
    return AccelerationStructureID::Invalid;
  }

  const auto size_info = query_blas_sizes(render_context, info, STATIC_BLAS_BUILD_FLAGS);
  const auto blas_id = render_context.allocate_acceleration_structure(
    VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
    size_info.accelerationStructureSize,
    "blas"
  );
  if (!blas_id) {
    OX_LOG_ERROR("Failed to allocate a BLAS ({}).", std::to_underlying(blas_id.error()));
    return AccelerationStructureID::Invalid;
  }

  auto scratch = render_context.persistent->allocate_buffer({
    .size = size_info.buildScratchSize,
    .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    .location = vir::MemoryLocation::GpuOnly,
    .alignment = render_context.as_scratch_alignment(),
    .name = "blas scratch",
  });
  const auto query_pool = render_context.create_query_pool(VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, 1);
  OX_DEFER(&) {
    if (scratch) {
      render_context.persistent->deallocate_buffer(*scratch);
    }

    render_context.destroy_query_pool(query_pool);
  };
  if (!scratch || query_pool == VK_NULL_HANDLE) {
    render_context.destroy_acceleration_structure(*blas_id);
    return AccelerationStructureID::Invalid;
  }

  const auto blas = render_context.acceleration_structure(*blas_id);
  auto build = vir::AccelerationStructureBuild{
    .info =
      {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .pNext = nullptr,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
        .flags = STATIC_BLAS_BUILD_FLAGS,
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        .srcAccelerationStructure = VK_NULL_HANDLE,
        .dstAccelerationStructure = blas.handle,
        .geometryCount = 0,
        .pGeometries = nullptr,
        .ppGeometries = nullptr,
        .scratchData = {.deviceAddress = scratch->device_address},
      },
    .geometries = {make_triangle_geometry(info)},
    .ranges = {{.primitiveCount = info.index_count / 3, .primitiveOffset = 0, .firstVertex = 0, .transformOffset = 0}},
  };

  auto module = vir::Module{};
  const auto builds = module.declare_acceleration_structure_builds_var("blas builds");
  const auto queries = module.declare_query_pool_var("blas compacted size");
  const auto resources = std::array{
    vir::ResourceAccess{
      module.import_buffer(render_context.buffer(blas.buffer), vir::Access::None),
      vir::Access::AccelerationStructureBuildWrite
    },
    vir::ResourceAccess{module.import_buffer(*scratch, vir::Access::None), vir::Access::AccelerationStructureBuildRW},
    vir::ResourceAccess{
      module.import_buffer(render_context.buffer(mesh_buffer), mesh_rests_at),
      vir::Access::AccelerationStructureBuildRead
    },
  };
  const auto built = module.build_acceleration_structures(builds, {}, resources);
  const auto queried = module.write_acceleration_structure_properties(built[0], blas.handle, queries);
  module.export_to(queried, vir::Access::AccelerationStructureBuildRead);
  module.export_to(built[2], mesh_rests_at);

  auto program = render_context.compile(std::move(module));
  if (!program) {
    OX_LOG_ERROR("Failed to compile a BLAS build ({}).", std::to_underlying(program.error()));
    render_context.destroy_acceleration_structure(*blas_id);
    return AccelerationStructureID::Invalid;
  }

  program->set(builds, std::make_shared<const std::vector<vir::AccelerationStructureBuild>>(1, std::move(build)));
  program->set(
    queries,
    vir::QueryPool{.handle = query_pool, .type = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, .count = 1}
  );
  if (const auto executed = render_context.execute_blocking(*program); !executed) {
    OX_LOG_ERROR("Failed to build a BLAS ({}).", std::to_underlying(executed.error()));
    render_context.destroy_acceleration_structure(*blas_id);
    return AccelerationStructureID::Invalid;
  }

  auto compacted_size = 0_u64;
  if (!render_context.read_query_pool(query_pool, {&compacted_size, 1})) {
    return *blas_id;
  }

  return compact_blas(render_context, *blas_id, compacted_size);
}

constexpr static auto SKINNED_ARENA_MIN_SIZE = 1_u64 << 20;

static auto query_skinned_blas_sizes(RenderContext& render_context, const BLASBuildInfo& info)
  -> VkAccelerationStructureBuildSizesInfoKHR {
  return query_blas_sizes(render_context, info, SKINNED_BLAS_BUILD_FLAGS);
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

static auto allocate_arena(RenderContext& render_context, u64 size, u64 alignment, std::string_view name)
  -> std::expected<BufferID, VkResult> {
  return render_context.allocate_buffer({
    .size = size,
    .usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    .location = vir::MemoryLocation::GpuOnly,
    .alignment = alignment,
    .name = std::string(name),
  });
}

// releasing through the render context is what lets a frame in flight keep traversing it
static auto release_structure(RenderContext& render_context, AccelerationStructureID& id) -> void {
  if (id != AccelerationStructureID::Invalid) {
    render_context.destroy_acceleration_structure(id);
    id = AccelerationStructureID::Invalid;
  }
}

static auto release_buffer(RenderContext& render_context, BufferID& id) -> void {
  if (id != BufferID::Invalid) {
    render_context.destroy_buffer(id);
    id = BufferID::Invalid;
  }
}

static auto create_pool_structure(RenderContext& render_context, SkinnedBLASPool& pool, SkinnedBLASPool::Entry& entry)
  -> bool {
  const auto structure = render_context.create_acceleration_structure_in(
    VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
    pool.buffer,
    entry.offset,
    entry.size
  );
  if (!structure) {
    OX_LOG_ERROR("Failed to allocate skinned bottom level acceleration structure.");
    entry.structure = AccelerationStructureID::Invalid;
    entry.device_address = 0;
    return false;
  }

  entry.structure = *structure;
  entry.device_address = render_context.acceleration_structure(*structure).device_address;
  // a fresh structure over fresh memory holds no tree, so nothing may refit from it and its address
  // stays out of the TLAS until a build has actually run
  entry.built_once = false;
  entry.refits_since_build = 0;

  return true;
}

// a staged migration can only run against the arena it was staged over, so anything that replaces or
// drops that arena has to give up the sources first
static auto discard_migrations(RenderContext& render_context, SkinnedBLASPool& pool) -> void {
  for (auto& entry : pool.entries) {
    release_structure(render_context, entry.previous_structure);
  }

  release_buffer(render_context, pool.migration_source_buffer);
}

// replaces the arena with a larger one and re-places every live entry in it. An entry that already
// holds a tree keeps it by migrating off its old structure, and one that does not falls back to a
// full build, which is the safe direction: an entry with no tree publishes no address
static auto grow_skinned_arena(RenderContext& render_context, SkinnedBLASPool& pool, u64 required) -> bool {
  ZoneScoped;

  // whatever was staged against the arena about to be replaced can no longer run
  discard_migrations(render_context, pool);

  auto live = 0_u64;
  for (const auto& entry : pool.entries) {
    if (entry.structure != AccelerationStructureID::Invalid) {
      live += align_up(entry.size, AS_BUFFER_ALIGNMENT);
    }
  }

  const auto capacity = ox::max(ox::max(live + required, pool.capacity * 2), SKINNED_ARENA_MIN_SIZE);

  // allocated before anything is given up, so a failure leaves the arena that is already working
  // alone rather than trading it for nothing
  const auto arena = allocate_arena(render_context, capacity, AS_BUFFER_ALIGNMENT, "skinned blas arena");
  if (!arena) {
    return false;
  }

  auto previous_arena = std::exchange(pool.buffer, *arena);
  pool.capacity = capacity;
  pool.bump = 0;
  pool.free_blocks.clear();

  auto is_migrating = false;
  for (u32 index = 0; index < static_cast<u32>(pool.entries.size()); index++) {
    auto& entry = pool.entries[index];
    if (entry.size == 0) {
      continue;
    }

    // create_pool_structure clears both, and a migrated tree inherits the staleness of the one it
    // came from rather than counting as freshly built
    const auto refits_since_build = entry.refits_since_build;
    const auto can_migrate = entry.structure != AccelerationStructureID::Invalid && entry.built_once;
    auto previous = std::exchange(entry.structure, AccelerationStructureID::Invalid);

    entry.offset = pool.bump;
    pool.bump += align_up(entry.size, AS_BUFFER_ALIGNMENT);
    if (!create_pool_structure(render_context, pool, entry)) {
      release_structure(render_context, previous);
      pool.slot_to_entry.erase(entry.mesh_instance_slot);
      entry = {};
      pool.free_entries.push_back(index);
      continue;
    }

    if (can_migrate) {
      entry.previous_structure = previous;
      entry.refits_since_build = refits_since_build;
      is_migrating = true;
    } else {
      release_structure(render_context, previous);
    }
  }

  if (is_migrating) {
    pool.migration_source_buffer = previous_arena;
  } else {
    release_buffer(render_context, previous_arena);
  }

  return true;
}

auto SkinnedBLASPool::reset(this SkinnedBLASPool& self, RenderContext& render_context) -> void {
  discard_migrations(render_context, self);
  for (auto& entry : self.entries) {
    release_structure(render_context, entry.structure);
  }

  release_buffer(render_context, self.buffer);
  release_buffer(render_context, self.scratch_buffer);

  self.entries.clear();
  self.free_entries.clear();
  self.free_blocks.clear();
  self.slot_to_entry.clear();
  self.pending_rebuilt.clear();
  self.pending_refit.clear();
  self.pending_migrated.clear();
  self.capacity = 0;
  self.bump = 0;
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
  // a migration is an update, so it degrades the tree it carried over exactly like a refit does, and
  // the entry is only back in the TLAS once that update has been handed over
  for (const auto index : self.pending_migrated) {
    self.entries[index].refits_since_build += 1;
    self.entries[index].built_once = true;
  }
  for (const auto index : self.pending_rebuilt) {
    self.entries[index].refits_since_build = 0;
    self.entries[index].built_once = true;
  }

  self.pending_refit.clear();
  self.pending_migrated.clear();
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

  // anything a build was staged for last frame but never committed is not built, and the flags below
  // are rebuilt from scratch anyway
  self.pending_rebuilt.clear();
  self.pending_refit.clear();
  self.pending_migrated.clear();

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
        entry.structure != AccelerationStructureID::Invalid && entry.indices == info.indices &&
        entry.vertex_count == info.vertex_count && entry.index_count == info.index_count
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
    release_structure(render_context, entry.structure);
    release_structure(render_context, entry.previous_structure);
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
    if (!offset && self.buffer != BufferID::Invalid && self.bump + block_size <= self.capacity) {
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

    if (!create_pool_structure(render_context, self, entry)) {
      give_free_block(self.free_blocks, entry.offset, block_size);
      entry = {};
      self.free_entries.push_back(index);
      continue;
    }

    self.slot_to_entry.insert_or_assign(skinned.mesh_instance_slot, index);
  }

  // the replaced arena is only worth holding while something still has to read a tree out of it
  const auto is_migrating = std::ranges::any_of(self.entries, [](const Entry& entry) {
    return entry.previous_structure != AccelerationStructureID::Invalid;
  });
  if (!is_migrating) {
    discard_migrations(render_context, self);
  }
}

auto stage_skinned_blas_builds(RenderContext& render_context, SkinnedBLASPool& pool, const SkinnedBLASBudget& budget)
  -> std::vector<vir::AccelerationStructureBuild> {
  ZoneScoped;
  memory::ScopedStack stack;

  if (!render_context.use_ray_tracing() || pool.buffer == BufferID::Invalid || pool.entries.empty()) {
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
  auto migrations = stack.alloc<u32>(entry_count);
  auto migration_count = 0_sz;

  const auto refit_limit = ox::max(budget.max_refits_before_rebuild, 1_u32);
  for (u32 index = 0; index < static_cast<u32>(entry_count); index++) {
    const auto& entry = pool.entries[index];
    if (!entry.alive || entry.structure == AccelerationStructureID::Invalid || entry.device_address == 0) {
      continue;
    }

    // an entry holding a source structure has a tree already, it is just sitting in the arena that
    // was replaced, so it updates out of that one instead of joining the rebuild queue
    if (entry.previous_structure != AccelerationStructureID::Invalid) {
      migrations[migration_count++] = index;
    } else if (!entry.built_once) {
      unbuilt[unbuilt_count++] = index;
    } else if (entry.refits_since_build >= refit_limit) {
      stale[stale_count++] = index;
    } else if (entry.pose_advanced) {
      refits[refit_count++] = index;
    }
  }

  if (migration_count == 0 && unbuilt_count == 0 && stale_count == 0 && refit_count == 0) {
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

  enum class Kind : u8 { Build, Refit, Migrate };
  struct Staged {
    u32 index = 0;
    Kind kind = Kind::Build;
    u64 scratch_offset = 0;
  };

  auto staged = stack.alloc<Staged>(entry_count);
  auto staged_count = 0_sz;
  auto scratch_total = 0_u64;
  const auto scratch_alignment = render_context.as_scratch_alignment();

  // the builds in one command run concurrently, so every one of them needs its own scratch region
  const auto stage = [&](const u32 index, const Kind kind) {
    const auto& entry = pool.entries[index];
    const auto scratch_size = kind == Kind::Build ? entry.build_scratch_size : entry.update_scratch_size;
    staged[staged_count++] = Staged{.index = index, .kind = kind, .scratch_offset = scratch_total};
    scratch_total += align_up(scratch_size, scratch_alignment);

    switch (kind) {
      case Kind::Build  : pool.pending_rebuilt.push_back(index); break;
      case Kind::Refit  : pool.pending_refit.push_back(index); break;
      case Kind::Migrate: pool.pending_migrated.push_back(index); break;
    }
  };

  // ahead of the budget and outside it: a migration costs an update rather than a build, and until it
  // runs the entry is out of the TLAS while holding a perfectly good tree in the old arena
  for (usize k = 0; k < migration_count; k++) {
    stage(migrations[k], Kind::Migrate);
  }

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
    stage(index, Kind::Build);
  }
  for (usize k = 0; k < stale_count; k++) {
    const auto index = stale[k];
    const auto primitives = primitives_of(index);
    if (primitives > remaining_budget) {
      // over budget this frame, so it keeps refitting rather than sitting on a frozen tree
      if (pool.entries[index].pose_advanced) {
        stage(index, Kind::Refit);
      }
      continue;
    }

    remaining_budget -= primitives;
    stage(index, Kind::Build);
  }
  for (usize k = 0; k < refit_count; k++) {
    stage(refits[k], Kind::Refit);
  }

  if (staged_count == 0) {
    return {};
  }

  // not resize_buffer: that allocates at the default alignment, and a scratch address the device
  // rejects is a driver crash rather than a validation message
  const auto scratch_size = pool.scratch_buffer != BufferID::Invalid ? render_context.buffer(pool.scratch_buffer).size
                                                                     : 0_u64;
  if (scratch_size < scratch_total) {
    const auto scratch = allocate_arena(
      render_context,
      ox::max(scratch_total, scratch_size * 2),
      scratch_alignment,
      "skinned blas scratch"
    );
    release_buffer(render_context, pool.scratch_buffer);
    if (scratch) {
      pool.scratch_buffer = *scratch;
    }
  }
  if (pool.scratch_buffer == BufferID::Invalid) {
    pool.pending_refit.clear();
    pool.pending_migrated.clear();
    pool.pending_rebuilt.clear();
    return {};
  }

  const auto scratch_address = render_context.buffer(pool.scratch_buffer).device_address;
  auto builds = std::vector<vir::AccelerationStructureBuild>{};
  builds.reserve(staged_count);
  for (usize i = 0; i < staged_count; i++) {
    const auto& [index, kind, scratch_offset] = staged[i];
    const auto& entry = pool.entries[index];
    const auto source = kind == Kind::Build     ? VK_NULL_HANDLE
                        : kind == Kind::Migrate ? render_context.acceleration_structure(entry.previous_structure).handle
                                                : render_context.acceleration_structure(entry.structure).handle;
    builds.push_back({
      .info =
        {
          .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
          .pNext = nullptr,
          .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
          .flags = SKINNED_BLAS_BUILD_FLAGS,
          // refits are in place: the tree being rewritten is also the tree being read. A migration
          // is the same update pointed at a source in the arena that was replaced, so the tree
          // survives the move for the price of a refit instead of a full build
          .mode = source != VK_NULL_HANDLE ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                                           : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
          .srcAccelerationStructure = source,
          .dstAccelerationStructure = render_context.acceleration_structure(entry.structure).handle,
          .geometryCount = 0,
          .pGeometries = nullptr,
          .ppGeometries = nullptr,
          .scratchData = {.deviceAddress = scratch_address + scratch_offset},
        },
      .geometries = {make_triangle_geometry(
        BLASBuildInfo{
          .vertex_positions = entry.vertex_positions,
          .indices = entry.indices,
          .vertex_count = entry.vertex_count,
          .index_count = entry.index_count,
        }
      )},
      .ranges = {
        {.primitiveCount = entry.index_count / 3, .primitiveOffset = 0, .firstVertex = 0, .transformOffset = 0}
      },
    });
  }

  return builds;
}

auto SceneTLAS::reserve(this SceneTLAS& self, RenderContext& render_context, u32 instance_count) -> bool {
  ZoneScoped;

  if (!render_context.use_ray_tracing() || instance_count == 0) {
    return false;
  }

  if (self.structure != AccelerationStructureID::Invalid && instance_count <= self.capacity) {
    return true;
  }

  // grow only, deliberately: releasing on a dip would hand the next spike a fresh structure, buffer
  // and scratch allocation, and the frame shape keys on the structure so every change recompiles
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
  render_context.context->fn.vkGetAccelerationStructureBuildSizesKHR(
    render_context.device,
    VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
    &build_info,
    &capacity,
    &size_info
  );

  self.destroy(render_context);

  const auto structure = render_context.allocate_acceleration_structure(
    VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
    size_info.accelerationStructureSize,
    "tlas"
  );
  const auto instances = render_context.allocate_buffer({
    .size = static_cast<u64>(capacity) * sizeof(GPU::AccelerationStructureInstance),
    .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
             VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
    .location = vir::MemoryLocation::GpuOnly,
    .alignment = 16,
    .name = "tlas instances",
  });
  const auto scratch = allocate_arena(
    render_context,
    size_info.buildScratchSize,
    render_context.as_scratch_alignment(),
    "tlas scratch"
  );
  if (!structure || !instances || !scratch) {
    OX_LOG_ERROR("Failed to allocate the scene TLAS.");
    if (structure) {
      render_context.destroy_acceleration_structure(*structure);
    }
    if (instances) {
      render_context.destroy_buffer(*instances);
    }
    if (scratch) {
      render_context.destroy_buffer(*scratch);
    }

    return false;
  }

  self.structure = *structure;
  self.instances_buffer = *instances;
  self.scratch_buffer = *scratch;
  self.capacity = capacity;

  return true;
}

auto SceneTLAS::destroy(this SceneTLAS& self, RenderContext& render_context) -> void {
  if (self.structure != AccelerationStructureID::Invalid) {
    render_context.destroy_acceleration_structure(self.structure);
    self.structure = AccelerationStructureID::Invalid;
  }

  release_buffer(render_context, self.instances_buffer);
  release_buffer(render_context, self.scratch_buffer);
  self.capacity = 0;
}

auto SceneTLAS::build(this const SceneTLAS& self, RenderContext& render_context, u32 instance_count)
  -> vir::AccelerationStructureBuild {
  return {
    .info =
      {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .pNext = nullptr,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        .flags = TLAS_BUILD_FLAGS,
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        .srcAccelerationStructure = VK_NULL_HANDLE,
        .dstAccelerationStructure = render_context.acceleration_structure(self.structure).handle,
        .geometryCount = 0,
        .pGeometries = nullptr,
        .ppGeometries = nullptr,
        .scratchData = {.deviceAddress = render_context.buffer(self.scratch_buffer).device_address},
      },
    .geometries = {{
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
      .pNext = nullptr,
      .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
      .geometry =
        {.instances =
           {
             .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
             .pNext = nullptr,
             .arrayOfPointers = VK_FALSE,
             .data = {.deviceAddress = render_context.buffer(self.instances_buffer).device_address},
           }},
      .flags = 0,
    }},
    .ranges = {{.primitiveCount = instance_count, .primitiveOffset = 0, .firstVertex = 0, .transformOffset = 0}},
  };
}
} // namespace ox
