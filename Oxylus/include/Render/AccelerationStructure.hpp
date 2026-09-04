#pragma once

#include <ankerl/unordered_dense.h>
#include <span>
#include <vector>
#include <vuk/Value.hpp>
#include <vuk/runtime/vk/Allocator.hpp>

#include "Animation/Fwd.hpp"
#include "Core/Types.hpp"

namespace ox {
class RenderContext;

struct AccelerationStructure {
  vuk::Unique<vuk::Buffer> buffer{};
  vuk::Unique<VkAccelerationStructureKHR> handle{};
  VkDeviceAddress device_address = 0;

  explicit operator bool(this const AccelerationStructure& self) { return self.device_address != 0; }
};

// LOD0 of mesh only
struct BLASBuildInfo {
  u64 vertex_positions = 0;
  u64 indices = 0;
  u32 vertex_count = 0;
  u32 index_count = 0;
};

auto build_mesh_blas(
  RenderContext& render_context,
  const BLASBuildInfo& info,
  vuk::Value<vuk::Buffer>&& mesh_buffer,
  AccelerationStructure& out_blas,
  vuk::Unique<vuk::Buffer>& out_scratch
) -> vuk::Value<vuk::Buffer>;

// a skinned instance cannot share its mesh's structure, which only ever covers the bind pose, so it
// gets one of its own built over its slice of the skinned vertex arena. They are suballocated out of
// one arena and share one scratch allocation, so a frame's builds go out as a single command instead
// of one per character
struct SkinnedBLASPool {
  struct Entry {
    vuk::Unique<VkAccelerationStructureKHR> handle{};
    // the structure this entry occupied before the arena was replaced under it. An update off it
    // carries the tree over to the new memory instead of paying for a full rebuild
    vuk::Unique<VkAccelerationStructureKHR> previous_handle{};
    u64 offset = 0;
    u64 size = 0;
    u64 build_scratch_size = 0;
    u64 update_scratch_size = 0;
    u64 device_address = 0;

    u64 vertex_positions = 0;
    u64 indices = 0;
    u32 vertex_count = 0;
    u32 index_count = 0;

    u32 mesh_instance_slot = 0;
    u32 refits_since_build = 0;
    // no tree yet, so nothing may refit from it and its address must not reach the TLAS
    bool built_once = false;
    bool alive = false;
    bool pose_advanced = false;
  };

  struct Block {
    u64 offset = 0;
    u64 size = 0;
  };

  // the arena a growth replaced cannot be dropped on the spot: frames still in flight reference both
  // the memory and the handles that were created over it
  struct Retired {
    vuk::Unique<vuk::Buffer> buffer{};
    std::vector<vuk::Unique<VkAccelerationStructureKHR>> handles{};
    u64 frame = 0;
  };

  vuk::Unique<vuk::Buffer> buffer{};
  vuk::Unique<vuk::Buffer> scratch_buffer{};
  // the replaced arena, held rather than retired for as long as anything still has to migrate off
  // it, because the copy sources live in it
  vuk::Unique<vuk::Buffer> migration_source_buffer{};
  u64 capacity = 0;
  u64 bump = 0;

  std::vector<Entry> entries{};
  std::vector<u32> free_entries{};
  std::vector<Block> free_blocks{};
  ankerl::unordered_dense::map<u32, u32> slot_to_entry{};
  std::vector<Retired> retired{};

  // recorded this frame and not yet accounted for, because a structure only counts as built once the
  // work that builds it has actually been submitted
  std::vector<u32> pending_rebuilt{};
  std::vector<u32> pending_refit{};
  std::vector<u32> pending_migrated{};

  auto sync(
    this SkinnedBLASPool& self,
    RenderContext& render_context,
    std::span<const SkinnedMeshInstance> instances,
    u64 arena_address
  ) -> void;
  auto address_of(this const SkinnedBLASPool& self, u32 mesh_instance_slot) -> u64;
  auto commit_builds(this SkinnedBLASPool& self) -> void;
  auto reset(this SkinnedBLASPool& self, RenderContext& render_context) -> void;
  auto collect_retired(this SkinnedBLASPool& self, RenderContext& render_context) -> void;
};

// full builds are staggered across frames against these, so a crowd costs a bounded amount per frame
// instead of everyone rebuilding at once
struct SkinnedBLASBudget {
  // triangles a frame may spend on full builds. Zero lifts the cap
  u32 rebuild_primitive_budget = 0;
  u32 max_refits_before_rebuild = 8;
};

// returns a null value when there was nothing to record, so the caller can tell that apart from a
// build it has to wire into the TLAS
auto build_skinned_blases(
  RenderContext& render_context,
  SkinnedBLASPool& pool,
  const SkinnedBLASBudget& budget,
  vuk::Value<vuk::Buffer>&& skinned_vertices
) -> vuk::Value<vuk::Buffer>;

struct SceneTLAS {
  AccelerationStructure acceleration_structure{};
  vuk::Unique<vuk::Buffer> instances_buffer{};
  vuk::Unique<vuk::Buffer> scratch_buffer{};
  u32 capacity = 0;

  auto reserve(this SceneTLAS& self, RenderContext& render_context, u32 instance_count) -> bool;
};

struct TLASBuildInfo {
  u32 instance_count = 0;
  u32 skinned_instance_count = 0;

  vuk::Value<vuk::Buffer> mesh_instances_buffer = {};
  vuk::Value<vuk::Buffer> transforms_buffer = {};
  vuk::Value<vuk::Buffer> blas_addresses_buffer = {};
  vuk::Value<vuk::Buffer> skinned_blas_addresses_buffer = {};
  // the skinned structure builds, threaded through purely so they land in the subgraph this build
  // waits on. Null when nothing was recorded
  vuk::Value<vuk::Buffer> skinned_blas_buffer = {};
};

auto build_scene_tlas(RenderContext& render_context, SceneTLAS& scene_tlas, TLASBuildInfo&& info)
  -> vuk::Value<vuk::Buffer>;
} // namespace ox
