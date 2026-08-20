#pragma once

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
};

auto build_scene_tlas(RenderContext& render_context, SceneTLAS& tlas, TLASBuildInfo&& info) -> vuk::Value<vuk::Buffer>;
} // namespace ox
