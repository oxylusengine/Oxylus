#pragma once

#include <ankerl/unordered_dense.h>
#include <span>
#include <vector>

#include "Animation/Fwd.hpp"
#include "Core/Types.hpp"
#include "Render/Fwd.hpp"
#include "Render/Graph/Access.hpp"

namespace ox::vir {
struct AccelerationStructureBuild;
}

namespace ox {
class RenderContext;

// LOD0 of mesh only
struct BLASBuildInfo {
  u64 vertex_positions = 0;
  u64 indices = 0;
  u32 vertex_count = 0;
  u32 index_count = 0;
};

// builds and compacts a structure over geometry already in `mesh_buffer`, which stays at
// `mesh_rests_at`. Blocks until the compacted copy is done, since its size only comes back to the host
auto build_mesh_blas(
  RenderContext& render_context, BufferID mesh_buffer, vir::Access mesh_rests_at, const BLASBuildInfo& info
) -> AccelerationStructureID;

// a skinned instance cannot share its mesh's structure, which only ever covers the bind pose, so it
// gets one of its own built over its slice of the skinned vertex arena. They are suballocated out of
// one arena and share one scratch allocation, so a frame's builds go out as a single command instead
// of one per character. Every structure and buffer released here retires through the render context,
// so frames still in flight keep what they reference
struct SkinnedBLASPool {
  struct Entry {
    AccelerationStructureID structure = AccelerationStructureID::Invalid;
    // the structure this entry occupied before the arena was replaced under it. An update off it
    // carries the tree over to the new memory instead of paying for a full rebuild
    AccelerationStructureID previous_structure = AccelerationStructureID::Invalid;
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

  BufferID buffer = BufferID::Invalid;
  BufferID scratch_buffer = BufferID::Invalid;
  // the replaced arena, held for as long as anything still has to migrate off it, because the copy
  // sources live in it
  BufferID migration_source_buffer = BufferID::Invalid;
  u64 capacity = 0;
  u64 bump = 0;

  std::vector<Entry> entries{};
  std::vector<u32> free_entries{};
  std::vector<Block> free_blocks{};
  ankerl::unordered_dense::map<u32, u32> slot_to_entry{};

  // staged this frame and not yet accounted for, because a structure only counts as built once the
  // work that builds it has been handed to the program
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
};

// full builds are staggered across frames against these, so a crowd costs a bounded amount per frame
// instead of everyone rebuilding at once
struct SkinnedBLASBudget {
  // triangles a frame may spend on full builds. Zero lifts the cap
  u32 rebuild_primitive_budget = 0;
  u32 max_refits_before_rebuild = 8;
};

// the builds this frame runs, against the arena and scratch the program binds. Every address in them
// is final, the arena and scratch are persistent
auto stage_skinned_blas_builds(RenderContext& render_context, SkinnedBLASPool& pool, const SkinnedBLASBudget& budget)
  -> std::vector<vir::AccelerationStructureBuild>;

struct SceneTLAS {
  AccelerationStructureID structure = AccelerationStructureID::Invalid;
  BufferID instances_buffer = BufferID::Invalid;
  BufferID scratch_buffer = BufferID::Invalid;
  u32 capacity = 0;

  auto reserve(this SceneTLAS& self, RenderContext& render_context, u32 instance_count) -> bool;
  auto destroy(this SceneTLAS& self, RenderContext& render_context) -> void;
  // the build over the first `instance_count` written instances
  auto build(this const SceneTLAS& self, RenderContext& render_context, u32 instance_count)
    -> vir::AccelerationStructureBuild;
};
} // namespace ox
