#include "Render/Graph/Access.hpp"

#include <array>
#include <span>

#include "Render/Graph/ShaderLayout.hpp"
#include "Render/Graph/Types.hpp"
#include "Utils/Log.hpp"

namespace ox::vir {
struct StageAccess {
  ShaderStage stage;
  Access access;
};

static auto by_shader_stage(ShaderStage stages, std::span<const StageAccess> mapped, Access fallback) -> Access {
  auto access = Access::Empty;
  auto covered = ShaderStage::None;
  for (const auto& [stage, mapped_access] : mapped) {
    if ((stages & stage) == stage) {
      access |= mapped_access;
      covered |= stage;
    }
  }

  const auto uncovered = stages & ~covered;
  if (!is_empty(uncovered)) {
    OX_LOG_WARN("no access covers shader stages {:#x}, memory is waited on instead", std::to_underlying(uncovered));
    access |= fallback;
  }

  return access;
}

static auto per_stage(
  Access vertex,
  Access tessellation_control,
  Access tessellation_evaluation,
  Access task,
  Access mesh,
  Access fragment,
  Access compute,
  Access ray_tracing
) -> std::array<StageAccess, 13> {
  return {{
    {ShaderStage::Vertex, vertex},
    {ShaderStage::TessellationControl, tessellation_control},
    {ShaderStage::TessellationEvaluation, tessellation_evaluation},
    {ShaderStage::Task, task},
    {ShaderStage::Mesh, mesh},
    {ShaderStage::Fragment, fragment},
    {ShaderStage::Compute, compute},
    {ShaderStage::RayGeneration, ray_tracing},
    {ShaderStage::AnyHit, ray_tracing},
    {ShaderStage::ClosestHit, ray_tracing},
    {ShaderStage::Miss, ray_tracing},
    {ShaderStage::Intersection, ray_tracing},
    {ShaderStage::Callable, ray_tracing},
  }};
}

auto sampled_by(ShaderStage stages) -> Access {
  const auto mapped = per_stage(
    Access::VertexSampled,
    Access::TessellationControlSampled,
    Access::TessellationEvaluationSampled,
    Access::TaskSampled,
    Access::MeshSampled,
    Access::FragmentSampled,
    Access::ComputeSampled,
    Access::RayTracingSampled
  );
  return by_shader_stage(stages, mapped, Access::MemoryRead);
}

auto shader_read_by(ShaderStage stages) -> Access {
  const auto mapped = per_stage(
    Access::VertexRead,
    Access::TessellationControlRead,
    Access::TessellationEvaluationRead,
    Access::TaskRead,
    Access::MeshRead,
    Access::FragmentRead,
    Access::ComputeRead,
    Access::RayTracingRead
  );
  return by_shader_stage(stages, mapped, Access::MemoryRead);
}

auto shader_write_by(ShaderStage stages) -> Access {
  const auto mapped = per_stage(
    Access::VertexWrite,
    Access::TessellationControlWrite,
    Access::TessellationEvaluationWrite,
    Access::TaskWrite,
    Access::MeshWrite,
    Access::FragmentWrite,
    Access::ComputeWrite,
    Access::RayTracingWrite
  );
  return by_shader_stage(stages, mapped, Access::MemoryWrite);
}

auto uniform_by(ShaderStage stages) -> Access {
  const auto mapped = per_stage(
    Access::VertexUniformRead,
    Access::TessellationControlUniformRead,
    Access::TessellationEvaluationUniformRead,
    Access::TaskUniformRead,
    Access::MeshUniformRead,
    Access::FragmentUniformRead,
    Access::ComputeUniformRead,
    Access::RayTracingUniformRead
  );
  return by_shader_stage(stages, mapped, Access::MemoryRead);
}

auto acceleration_structure_read_by(ShaderStage stages) -> Access {
  const auto mapped = per_stage(
    Access::VertexAccelerationStructureRead,
    Access::TessellationControlAccelerationStructureRead,
    Access::TessellationEvaluationAccelerationStructureRead,
    Access::TaskAccelerationStructureRead,
    Access::MeshAccelerationStructureRead,
    Access::FragmentAccelerationStructureRead,
    Access::ComputeAccelerationStructureRead,
    Access::RayTracingRead
  );
  return by_shader_stage(stages, mapped, Access::MemoryRead);
}

auto descriptor_access(DescriptorUsage usage, ShaderStage stages) -> Access {
  switch (usage) {
    case DescriptorUsage::None                 : return Access::None;
    case DescriptorUsage::Read                 : return shader_read_by(stages);
    case DescriptorUsage::ReadWrite            : return shader_read_by(stages) | shader_write_by(stages);
    case DescriptorUsage::Sampled              : return sampled_by(stages);
    case DescriptorUsage::Uniform              : return uniform_by(stages);
    case DescriptorUsage::AccelerationStructure: return acceleration_structure_read_by(stages);
    case DescriptorUsage::InputAttachment      : return Access::InputAttachmentRead;
  }

  return Access::None;
}

struct AccessMapping {
  Access access;
  VkAccessFlags2 flags;
  VkPipelineStageFlags2 stages;
};

constexpr auto SAMPLED = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
constexpr auto READ = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
constexpr auto WRITE = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
constexpr auto UNIFORM = VK_ACCESS_2_UNIFORM_READ_BIT;
constexpr auto AS_READ = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;

constexpr auto VERTEX = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
constexpr auto TESC = VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT;
constexpr auto TESE = VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT;
constexpr auto TASK = VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT;
constexpr auto MESH = VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;
constexpr auto FRAGMENT = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
constexpr auto COMPUTE = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
constexpr auto RAY_TRACING = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;

// storage reads and writes use the storage-specific access bits rather than SHADER_READ/WRITE, since
// a buffer reached through a device address is read as storage too
constexpr auto ACCESS_MAPPINGS = std::to_array<AccessMapping>({
  {Access::ColorRead, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT},
  {Access::ColorWrite, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT},
  {Access::DepthStencilRead,
   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
   VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT},
  {Access::DepthStencilWrite,
   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
   VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT},
  {Access::VertexSampled, SAMPLED, VERTEX},
  {Access::VertexRead, READ, VERTEX},
  {Access::VertexWrite, WRITE, VERTEX},
  {Access::VertexUniformRead, UNIFORM, VERTEX},
  {Access::VertexAccelerationStructureRead, AS_READ, VERTEX},
  {Access::AttributeRead, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT, VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT},
  {Access::IndexRead, VK_ACCESS_2_INDEX_READ_BIT, VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT},
  {Access::IndirectRead, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT},
  {Access::FragmentSampled, SAMPLED, FRAGMENT},
  {Access::FragmentRead, READ, FRAGMENT},
  {Access::FragmentWrite, WRITE, FRAGMENT},
  {Access::FragmentUniformRead, UNIFORM, FRAGMENT},
  {Access::FragmentAccelerationStructureRead, AS_READ, FRAGMENT},
  {Access::InputAttachmentRead, VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT, FRAGMENT},
  {Access::CopyRead, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT},
  {Access::CopyWrite, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT},
  {Access::BlitRead, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT},
  {Access::BlitWrite, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT},
  {Access::Clear, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_CLEAR_BIT},
  {Access::ResolveRead, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_RESOLVE_BIT},
  {Access::ResolveWrite, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_RESOLVE_BIT},
  {Access::ComputeRead, READ, COMPUTE},
  {Access::ComputeWrite, WRITE, COMPUTE},
  {Access::ComputeSampled, SAMPLED, COMPUTE},
  {Access::ComputeUniformRead, UNIFORM, COMPUTE},
  {Access::ComputeAccelerationStructureRead, AS_READ, COMPUTE},
  {Access::RayTracingRead, READ | AS_READ, RAY_TRACING},
  {Access::RayTracingWrite, WRITE, RAY_TRACING},
  {Access::RayTracingSampled, SAMPLED, RAY_TRACING},
  {Access::RayTracingUniformRead, UNIFORM, RAY_TRACING},
  {Access::AccelerationStructureBuildRead,
   VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_SHADER_READ_BIT,
   VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR},
  {Access::AccelerationStructureBuildWrite,
   VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
   VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR},
  {Access::HostRead, VK_ACCESS_2_HOST_READ_BIT, VK_PIPELINE_STAGE_2_HOST_BIT},
  {Access::HostWrite, VK_ACCESS_2_HOST_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT},
  {Access::MemoryRead, VK_ACCESS_2_MEMORY_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
  {Access::MemoryWrite, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
  {Access::Present, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
  {Access::TaskSampled, SAMPLED, TASK},
  {Access::TaskRead, READ, TASK},
  {Access::TaskWrite, WRITE, TASK},
  {Access::TaskUniformRead, UNIFORM, TASK},
  {Access::TaskAccelerationStructureRead, AS_READ, TASK},
  {Access::MeshSampled, SAMPLED, MESH},
  {Access::MeshRead, READ, MESH},
  {Access::MeshWrite, WRITE, MESH},
  {Access::MeshUniformRead, UNIFORM, MESH},
  {Access::MeshAccelerationStructureRead, AS_READ, MESH},
  {Access::TessellationControlSampled, SAMPLED, TESC},
  {Access::TessellationControlRead, READ, TESC},
  {Access::TessellationControlWrite, WRITE, TESC},
  {Access::TessellationControlUniformRead, UNIFORM, TESC},
  {Access::TessellationControlAccelerationStructureRead, AS_READ, TESC},
  {Access::TessellationEvaluationSampled, SAMPLED, TESE},
  {Access::TessellationEvaluationRead, READ, TESE},
  {Access::TessellationEvaluationWrite, WRITE, TESE},
  {Access::TessellationEvaluationUniformRead, UNIFORM, TESE},
  {Access::TessellationEvaluationAccelerationStructureRead, AS_READ, TESE},
});

auto to_access_flags(Access access) -> VkAccessFlags2 {
  VkAccessFlags2 flags = VK_ACCESS_2_NONE;
  for (const auto& mapping : ACCESS_MAPPINGS) {
    if (contains(access, mapping.access)) {
      flags |= mapping.flags;
    }
  }

  return flags;
}

auto to_stage_flags(Access access) -> VkPipelineStageFlags2 {
  VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_NONE;
  for (const auto& mapping : ACCESS_MAPPINGS) {
    if (contains(access, mapping.access)) {
      stages |= mapping.stages;
    }
  }

  return stages;
}

constexpr auto SAMPLED_ACCESS = Access::FragmentSampled | Access::VertexSampled | Access::ComputeSampled |
                                Access::RayTracingSampled | Access::TaskSampled | Access::MeshSampled |
                                Access::TessellationControlSampled | Access::TessellationEvaluationSampled;
constexpr auto STORAGE_ACCESS = Access::FragmentRW | Access::VertexRW | Access::TessellationControlRW |
                                Access::TessellationEvaluationRW | Access::TaskRW | Access::MeshRW | Access::ComputeRW |
                                Access::RayTracingRW;
constexpr auto UNIFORM_ACCESS = Access::ComputeUniformRead | Access::VertexUniformRead | Access::FragmentUniformRead |
                                Access::TaskUniformRead | Access::MeshUniformRead |
                                Access::TessellationControlUniformRead | Access::TessellationEvaluationUniformRead |
                                Access::RayTracingUniformRead;

auto to_image_layout(Access access) -> VkImageLayout {
  if (contains(access, Access::Present)) {
    return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  }

  if (intersects(access, Access::ColorRW)) {
    return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  }

  if (contains(access, Access::DepthStencilWrite)) {
    return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  }

  // sampling a depth attachment the pass only tests against is legal in this layout too
  if (contains(access, Access::DepthStencilRead)) {
    return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  }

  // storage access forces GENERAL, so an image both sampled and written through storage in one
  // pass lands there too
  if (intersects(access, STORAGE_ACCESS)) {
    return VK_IMAGE_LAYOUT_GENERAL;
  }

  if (intersects(access, SAMPLED_ACCESS | Access::InputAttachmentRead)) {
    return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }

  if (intersects(access, Access::TransferWrite)) {
    return intersects(access, Access::TransferRead) ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  }

  if (intersects(access, Access::TransferRead)) {
    return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  }

  if (intersects(access, Access::MemoryRW | Access::HostRW)) {
    return VK_IMAGE_LAYOUT_GENERAL;
  }

  return VK_IMAGE_LAYOUT_UNDEFINED;
}

auto to_buffer_usage(Access access) -> VkBufferUsageFlags {
  VkBufferUsageFlags usage = 0;
  if (intersects(access, Access::MemoryRW | STORAGE_ACCESS)) {
    usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  }

  if (intersects(access, Access::MemoryRW | UNIFORM_ACCESS)) {
    usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
  }

  if (intersects(access, Access::MemoryRW | Access::AttributeRead)) {
    usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  }

  if (intersects(access, Access::MemoryRW | Access::IndexRead)) {
    usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
  }

  if (intersects(access, Access::MemoryRW | Access::IndirectRead)) {
    usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
  }

  if (intersects(access, Access::MemoryRW | Access::TransferRead)) {
    usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  }

  if (intersects(access, Access::MemoryRW | Access::TransferWrite)) {
    usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  }

  if (intersects(access, Access::AccelerationStructureBuildRead)) {
    usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  }

  if (intersects(access, Access::AccelerationStructureBuildWrite)) {
    usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  }

  return usage;
}

auto to_image_usage(Access access) -> ImageUsageFlags {
  ImageUsageFlags usage = ImageUsageFlags::None;
  if (intersects(access, Access::MemoryRW | Access::ColorRW)) {
    usage |= ImageUsageFlags::ColorAttachment;
  }

  if (intersects(access, Access::MemoryRW | SAMPLED_ACCESS)) {
    usage |= ImageUsageFlags::Sampled;
  }

  if (intersects(access, Access::MemoryRW | Access::DepthStencilRW)) {
    usage |= ImageUsageFlags::DepthStencilAttachment;
  }

  if (intersects(access, Access::MemoryRW | Access::TransferRead)) {
    usage |= ImageUsageFlags::TransferSrc;
  }

  if (intersects(access, Access::MemoryRW | Access::TransferWrite)) {
    usage |= ImageUsageFlags::TransferDst;
  }

  if (intersects(access, Access::MemoryRW | STORAGE_ACCESS)) {
    usage |= ImageUsageFlags::Storage;
  }

  if (intersects(access, Access::MemoryRW | Access::InputAttachmentRead)) {
    usage |= ImageUsageFlags::InputAttachment;
  }

  return usage;
}

struct AccessName {
  Access access;
  std::string_view name;
};

constexpr auto ACCESS_NAMES = std::to_array<AccessName>({
  {Access::None, "None"},
  {Access::ColorRead, "ColorRead"},
  {Access::ColorWrite, "ColorWrite"},
  {Access::DepthStencilRead, "DepthStencilRead"},
  {Access::DepthStencilWrite, "DepthStencilWrite"},
  {Access::VertexSampled, "VertexSampled"},
  {Access::VertexRead, "VertexRead"},
  {Access::VertexWrite, "VertexWrite"},
  {Access::VertexUniformRead, "VertexUniformRead"},
  {Access::VertexAccelerationStructureRead, "VertexAccelerationStructureRead"},
  {Access::AttributeRead, "AttributeRead"},
  {Access::IndexRead, "IndexRead"},
  {Access::IndirectRead, "IndirectRead"},
  {Access::FragmentSampled, "FragmentSampled"},
  {Access::FragmentRead, "FragmentRead"},
  {Access::FragmentWrite, "FragmentWrite"},
  {Access::FragmentUniformRead, "FragmentUniformRead"},
  {Access::FragmentAccelerationStructureRead, "FragmentAccelerationStructureRead"},
  {Access::InputAttachmentRead, "InputAttachmentRead"},
  {Access::CopyRead, "CopyRead"},
  {Access::CopyWrite, "CopyWrite"},
  {Access::BlitRead, "BlitRead"},
  {Access::BlitWrite, "BlitWrite"},
  {Access::Clear, "Clear"},
  {Access::ResolveRead, "ResolveRead"},
  {Access::ResolveWrite, "ResolveWrite"},
  {Access::ComputeRead, "ComputeRead"},
  {Access::ComputeWrite, "ComputeWrite"},
  {Access::ComputeSampled, "ComputeSampled"},
  {Access::ComputeUniformRead, "ComputeUniformRead"},
  {Access::ComputeAccelerationStructureRead, "ComputeAccelerationStructureRead"},
  {Access::RayTracingRead, "RayTracingRead"},
  {Access::RayTracingWrite, "RayTracingWrite"},
  {Access::RayTracingSampled, "RayTracingSampled"},
  {Access::RayTracingUniformRead, "RayTracingUniformRead"},
  {Access::AccelerationStructureBuildRead, "AccelerationStructureBuildRead"},
  {Access::AccelerationStructureBuildWrite, "AccelerationStructureBuildWrite"},
  {Access::HostRead, "HostRead"},
  {Access::HostWrite, "HostWrite"},
  {Access::MemoryRead, "MemoryRead"},
  {Access::MemoryWrite, "MemoryWrite"},
  {Access::Present, "Present"},
  {Access::TaskSampled, "TaskSampled"},
  {Access::TaskRead, "TaskRead"},
  {Access::TaskWrite, "TaskWrite"},
  {Access::TaskUniformRead, "TaskUniformRead"},
  {Access::TaskAccelerationStructureRead, "TaskAccelerationStructureRead"},
  {Access::MeshSampled, "MeshSampled"},
  {Access::MeshRead, "MeshRead"},
  {Access::MeshWrite, "MeshWrite"},
  {Access::MeshUniformRead, "MeshUniformRead"},
  {Access::MeshAccelerationStructureRead, "MeshAccelerationStructureRead"},
  {Access::TessellationControlSampled, "TessellationControlSampled"},
  {Access::TessellationControlRead, "TessellationControlRead"},
  {Access::TessellationControlWrite, "TessellationControlWrite"},
  {Access::TessellationControlUniformRead, "TessellationControlUniformRead"},
  {Access::TessellationControlAccelerationStructureRead, "TessellationControlAccelerationStructureRead"},
  {Access::TessellationEvaluationSampled, "TessellationEvaluationSampled"},
  {Access::TessellationEvaluationRead, "TessellationEvaluationRead"},
  {Access::TessellationEvaluationWrite, "TessellationEvaluationWrite"},
  {Access::TessellationEvaluationUniformRead, "TessellationEvaluationUniformRead"},
  {Access::TessellationEvaluationAccelerationStructureRead, "TessellationEvaluationAccelerationStructureRead"},
});

auto to_string(Access access) -> std::string {
  if (is_empty(access)) {
    return "Empty";
  }

  auto result = std::string{};
  for (const auto& [flag, name] : ACCESS_NAMES) {
    if (contains(access, flag)) {
      if (!result.empty()) {
        result += '|';
      }

      result += name;
    }
  }

  return result;
}
} // namespace ox::vir
