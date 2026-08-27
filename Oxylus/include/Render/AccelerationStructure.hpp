#pragma once

#include <span>
#include <vector>
#include <vuk/Value.hpp>
#include <vuk/runtime/vk/Allocator.hpp>

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

// a pose that only moved its vertices can refit the tree the last full build produced, which is a
// small fraction of the cost of producing a new one. Rebuild is for structures a refit cannot serve:
// one that has never been built, and one whose tree has drifted too far from the pose it was built at
enum class SkinnedBLASBuildMode : u8 { None = 0, Refit, Rebuild };

// a skinned instance cannot share its mesh's BLAS, which only ever covers the bind pose, so it gets
// one of its own built over its slice of the skinned vertex arena. One storage allocation and one
// scratch allocation cover the whole set, so the frame's builds go out as a single command instead
// of one per character
struct SkinnedBLASPool {
  vuk::Unique<vuk::Buffer> buffer{};
  vuk::Unique<vuk::Buffer> scratch_buffer{};
  std::vector<vuk::Unique<VkAccelerationStructureKHR>> handles{};
  // parallel to the infos handed to `reserve`, and zero where an entry cannot be built
  std::vector<u64> device_addresses{};
  std::vector<u64> scratch_offsets{};
  std::vector<BLASBuildInfo> builds{};
  // sticky: raised when a structure is created or its pose moves, cleared once the work is done
  std::vector<SkinnedBLASBuildMode> build_modes{};
  // a refit inherits the tree a full build produced, so traversal degrades as the pose walks away
  // from that one. Counting refits since the last full build is what lets the budget spend itself
  // on the structures that have drifted furthest
  std::vector<u32> refit_counts{};
  u64 layout_key = 0;

  auto reserve(this SkinnedBLASPool& self, RenderContext& render_context, std::span<const BLASBuildInfo> infos) -> bool;
  auto reset(this SkinnedBLASPool& self) -> void;
};

// full builds are staggered across frames against these, so a crowd costs a bounded amount per frame
// instead of everyone rebuilding at once
struct SkinnedBLASBudget {
  // triangles a frame may spend on full builds, over and above the ones correctness demands.
  // Zero lifts the cap, which is the old behaviour of rebuilding everything that came due
  u32 rebuild_primitive_budget = 0;
  u32 max_refits_before_rebuild = 0;
};

// consumes `skinned_vertices` only when there is something to build, and returns a null value
// otherwise so the caller can tell the two apart
auto build_skinned_blases(
  RenderContext& render_context,
  SkinnedBLASPool& pool,
  const SkinnedBLASBudget& budget,
  vuk::Value<vuk::Buffer>& skinned_vertices
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

  vuk::Value<vuk::Buffer> mesh_instances_buffer = {};
  vuk::Value<vuk::Buffer> transforms_buffer = {};
  vuk::Value<vuk::Buffer> blas_addresses_buffer = {};
  // null when nothing in the scene is skinned, in which case the build reads no per-instance
  // structures and needs no edge to them
  vuk::Value<vuk::Buffer> skinned_blas_buffer = {};
};

auto build_scene_tlas(RenderContext& render_context, SceneTLAS& scene_tlas, TLASBuildInfo&& info)
  -> vuk::Value<vuk::Buffer>;
} // namespace ox
