#pragma once

#include <string>
#include <vulkan/vulkan_core.h>

#include "Render/Graph/Fwd.hpp"
#include "Render/Graph/Types.hpp"

namespace ox::vir {
enum class Access : u64 {
  Empty = 0,
  None = 1_u64 << 0,
  ColorRead = 1_u64 << 1,
  ColorWrite = 1_u64 << 2,
  ColorRW = ColorRead | ColorWrite,
  DepthStencilRead = 1_u64 << 3,
  DepthStencilWrite = 1_u64 << 4,
  DepthStencilRW = DepthStencilRead | DepthStencilWrite,
  VertexSampled = 1_u64 << 5,
  VertexRead = 1_u64 << 6,
  AttributeRead = 1_u64 << 7,
  IndexRead = 1_u64 << 8,
  IndirectRead = 1_u64 << 9,
  VertexUniformRead = 1_u64 << 10,
  FragmentSampled = 1_u64 << 11,
  FragmentRead = 1_u64 << 12,
  FragmentWrite = 1_u64 << 13,
  FragmentRW = FragmentRead | FragmentWrite,
  FragmentUniformRead = 1_u64 << 14,
  CopyRead = 1_u64 << 15,
  CopyWrite = 1_u64 << 16,
  CopyRW = CopyRead | CopyWrite,
  BlitRead = 1_u64 << 17,
  BlitWrite = 1_u64 << 18,
  BlitRW = BlitRead | BlitWrite,
  TaskWrite = 1_u64 << 19,
  Clear = 1_u64 << 20,
  ResolveRead = 1_u64 << 21,
  ResolveWrite = 1_u64 << 22,
  ResolveRW = ResolveRead | ResolveWrite,
  TransferRead = CopyRead | BlitRead | ResolveRead,
  TransferWrite = CopyWrite | BlitWrite | Clear | ResolveWrite,
  TransferRW = TransferRead | TransferWrite,
  ComputeRead = 1_u64 << 23,
  ComputeWrite = 1_u64 << 24,
  ComputeRW = ComputeRead | ComputeWrite,
  ComputeSampled = 1_u64 << 25,
  ComputeUniformRead = 1_u64 << 26,
  RayTracingRead = 1_u64 << 27,
  RayTracingWrite = 1_u64 << 28,
  RayTracingRW = RayTracingRead | RayTracingWrite,
  RayTracingSampled = 1_u64 << 29,
  RayTracingUniformRead = 1_u64 << 30,
  AccelerationStructureBuildRead = 1_u64 << 31,
  AccelerationStructureBuildWrite = 1_u64 << 32,
  AccelerationStructureBuildRW = AccelerationStructureBuildRead | AccelerationStructureBuildWrite,
  HostRead = 1_u64 << 33,
  HostWrite = 1_u64 << 34,
  HostRW = HostRead | HostWrite,
  MemoryRead = 1_u64 << 35,
  MemoryWrite = 1_u64 << 36,
  MemoryRW = MemoryRead | MemoryWrite,
  Present = 1_u64 << 37,
  TaskSampled = 1_u64 << 38,
  TaskRead = 1_u64 << 39,
  TaskRW = TaskRead | TaskWrite,
  TaskUniformRead = 1_u64 << 40,
  VertexWrite = 1_u64 << 41,
  VertexRW = VertexRead | VertexWrite,
  TessellationControlSampled = 1_u64 << 42,
  TessellationControlRead = 1_u64 << 43,
  TessellationControlWrite = 1_u64 << 44,
  TessellationControlRW = TessellationControlRead | TessellationControlWrite,
  TessellationControlUniformRead = 1_u64 << 45,
  TessellationEvaluationSampled = 1_u64 << 46,
  TessellationEvaluationRead = 1_u64 << 47,
  TessellationEvaluationWrite = 1_u64 << 48,
  TessellationEvaluationRW = TessellationEvaluationRead | TessellationEvaluationWrite,
  TessellationEvaluationUniformRead = 1_u64 << 49,
  MeshSampled = 1_u64 << 50,
  MeshRead = 1_u64 << 51,
  MeshWrite = 1_u64 << 52,
  MeshRW = MeshRead | MeshWrite,
  MeshUniformRead = 1_u64 << 53,
  InputAttachmentRead = 1_u64 << 54,
  VertexAccelerationStructureRead = 1_u64 << 55,
  TessellationControlAccelerationStructureRead = 1_u64 << 56,
  TessellationEvaluationAccelerationStructureRead = 1_u64 << 57,
  MeshAccelerationStructureRead = 1_u64 << 58,
  FragmentAccelerationStructureRead = 1_u64 << 59,
  ComputeAccelerationStructureRead = 1_u64 << 60,
  TaskAccelerationStructureRead = 1_u64 << 61,
  Writes = ColorWrite | DepthStencilWrite | VertexWrite | TessellationControlWrite | TessellationEvaluationWrite |
    MeshWrite | TaskWrite | FragmentWrite | ComputeWrite | RayTracingWrite | CopyWrite | BlitWrite | Clear |
    ResolveWrite | AccelerationStructureBuildWrite | HostWrite | MemoryWrite,
};
OX_VIR_FLAGS(Access)

enum class DomainFlag : u32 {
  None = 0,
  Host = 1 << 0,
  Present = 1 << 1,
  Graphics = 1 << 2,
  Compute = 1 << 3,
  Transfer = 1 << 4,
};
OX_VIR_FLAGS(DomainFlag)

constexpr auto writes(Access access) -> bool { return intersects(access, Access::Writes); }
constexpr auto reads(Access access) -> bool { return !is_empty(difference(access, Access::Writes | Access::None)); }

auto sampled_by(ShaderStage stages) -> Access;
auto shader_read_by(ShaderStage stages) -> Access;
auto shader_write_by(ShaderStage stages) -> Access;
auto uniform_by(ShaderStage stages) -> Access;
auto acceleration_structure_read_by(ShaderStage stages) -> Access;

auto to_access_flags(Access access) -> VkAccessFlags2;
auto to_stage_flags(Access access) -> VkPipelineStageFlags2;
auto to_image_layout(Access access) -> VkImageLayout;
auto to_buffer_usage(Access access) -> VkBufferUsageFlags;
auto to_image_usage(Access access) -> ImageUsageFlags;
auto to_string(Access access) -> std::string;
} // namespace ox::vir
