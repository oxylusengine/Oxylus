#pragma once

#include <expected>
#include <span>
#include <string>
#include <vector>

#include "Render/Graph/PipelineState.hpp"
#include "Render/Graph/ShaderLayout.hpp"
#include "Render/Graph/Types.hpp"

namespace ox::vir {
struct Context;

struct BindlessDescriptorSet {
  u32 index = 0;
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  VkDescriptorSet set = VK_NULL_HANDLE;

  auto operator==(const BindlessDescriptorSet&) const -> bool = default;
};

struct VertexBinding {
  u32 binding = 0;
  u32 stride = 0;
  VkVertexInputRate input_rate = VK_VERTEX_INPUT_RATE_VERTEX;

  auto operator==(const VertexBinding&) const -> bool = default;
};

struct VertexAttribute {
  u32 location = 0;
  u32 binding = 0;
  Format format = Format::Undefined;
  u32 offset = 0;

  auto operator==(const VertexAttribute&) const -> bool = default;
};

struct VertexLayout {
  std::vector<VertexBinding> bindings = {};
  std::vector<VertexAttribute> attributes = {};

  // one binding at rate vertex, attributes packed in location order
  static auto interleaved(std::span<const ShaderLayout> reflections) -> VertexLayout;

  auto is_empty(this const VertexLayout& self) -> bool { return self.attributes.empty(); }
  auto operator==(const VertexLayout&) const -> bool = default;
};

struct GraphicsPipelineInfo {
  std::vector<ShaderCode> shaders = {};
  option<BindlessDescriptorSet> bindless = nullopt;
  // left empty to come from vertex stage reflection
  option<VertexLayout> vertex_layout = nullopt;
  std::string name = {};
};

struct ComputePipelineInfo {
  ShaderCode shader = {};
  option<BindlessDescriptorSet> bindless = nullopt;
  std::string name = {};
};

struct RayTracingShaderGroup {
  VkRayTracingShaderGroupTypeKHR type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
  u32 general = VK_SHADER_UNUSED_KHR;
  u32 closest_hit = VK_SHADER_UNUSED_KHR;
  u32 any_hit = VK_SHADER_UNUSED_KHR;
  u32 intersection = VK_SHADER_UNUSED_KHR;
};

struct RayTracingPipelineInfo {
  std::vector<ShaderCode> shaders = {};
  // left empty to give every raygen, miss and callable shader a group of its own and gather the hit
  // shaders into one hit group
  std::vector<RayTracingShaderGroup> groups = {};
  u32 max_recursion_depth = 1;
  option<BindlessDescriptorSet> bindless = nullopt;
  std::string name = {};
};

auto push_constant_ranges(std::span<const ShaderLayout> reflections) -> std::vector<VkPushConstantRange>;
auto validate_descriptor_bindings(
  std::span<const ShaderLayout> reflections, const option<BindlessDescriptorSet>& bindless
) -> std::expected<void, VkResult>;
auto merged_bindings(std::span<const ShaderLayout> reflections) -> std::vector<DescriptorBinding>;
auto default_shader_groups(std::span<const ShaderLayout> reflections) -> std::vector<RayTracingShaderGroup>;
auto validate_shader_groups(std::span<const ShaderLayout> reflections, std::span<const RayTracingShaderGroup> groups)
  -> std::expected<void, VkResult>;

struct SetLayout {
  VkDescriptorSetLayout handle = VK_NULL_HANDLE;
  std::vector<std::pair<VkDescriptorType, u32>> sizes = {};
  bool is_push = false;
  bool is_owned = false;
};

struct PushRange {
  ShaderStage stages = ShaderStage::None;
  u32 offset = 0;
  u32 size = 0;
};

struct PipelineLayout {
  VkPipelineLayout handle = VK_NULL_HANDLE;
  std::vector<SetLayout> sets = {};
  std::vector<VkPushConstantRange> push_constant_ranges = {};
  option<BindlessDescriptorSet> bindless = nullopt;

  static auto create(
    const Context& context,
    std::span<const ShaderLayout> reflections,
    const option<BindlessDescriptorSet>& bindless,
    u32 max_push_descriptors
  ) -> std::expected<PipelineLayout, VkResult>;

  // the slices of [offset, offset + size) each declared push constant range covers
  auto cover(this const PipelineLayout& self, u32 offset, u32 size) -> std::vector<PushRange>;
  auto destroy(this const PipelineLayout& self, const Context& context) -> void;
};

struct PackedSpecialization {
  std::vector<u8> data = {};
  std::vector<std::vector<VkSpecializationMapEntry>> entries = {};
};

auto pack_specialization(std::span<const ShaderLayout> reflections, const Specialization& specialization)
  -> std::expected<PackedSpecialization, VkResult>;

struct GraphicsPipelineRequest {
  const GraphicsPipelineInfo* info = nullptr;
  std::span<const ShaderLayout> reflections = {};
  const VertexLayout* vertex = nullptr;
  const PipelineState* state = nullptr;
  VkPipelineLayout layout = VK_NULL_HANDLE;
};

struct ComputePipelineRequest {
  const ComputePipelineInfo* info = nullptr;
  const ShaderLayout* reflection = nullptr;
  const Specialization* specialization = nullptr;
  VkPipelineLayout layout = VK_NULL_HANDLE;
};

auto create_graphics_pipelines(const Context& context, std::span<const GraphicsPipelineRequest> requests)
  -> std::expected<std::vector<VkPipeline>, VkResult>;
auto create_compute_pipelines(const Context& context, std::span<const ComputePipelineRequest> requests)
  -> std::expected<std::vector<VkPipeline>, VkResult>;
auto create_ray_tracing_pipeline(
  const Context& context,
  const RayTracingPipelineInfo& info,
  std::span<const ShaderLayout> reflections,
  std::span<const RayTracingShaderGroup> groups,
  const Specialization& specialization,
  VkPipelineLayout layout
) -> std::expected<VkPipeline, VkResult>;
} // namespace ox::vir
