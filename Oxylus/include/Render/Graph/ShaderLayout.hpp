#pragma once

#include <array>
#include <string>
#include <vector>
#include <vulkan/vulkan_core.h>

#include "Render/Graph/Access.hpp"
#include "Render/Graph/Types.hpp"

namespace ox::vir {
// what a descriptor's declared type allows. The stages using it turn this into an `Access`
enum class DescriptorUsage : u8 { None, Read, ReadWrite, Sampled, Uniform, AccelerationStructure, InputAttachment };

constexpr auto NO_SPEC_ID = ~0_u32;

struct DescriptorBinding {
  u32 set = 0;
  u32 binding = 0;
  VkDescriptorType descriptor_type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
  u32 count = 1;
  bool is_variable_count = false;
  ShaderStage stages = ShaderStage::None;
  DescriptorUsage usage = DescriptorUsage::None;
  // derived from `usage` and `stages` when a pipeline is declared
  Access access = Access::None;

  auto operator==(const DescriptorBinding&) const -> bool = default;
};

struct VertexInput {
  u32 location = 0;
  Format format = Format::Undefined;
  u32 size = 0;

  auto operator==(const VertexInput&) const -> bool = default;
};

struct SpecConstant {
  u32 id = 0;
  u32 size = 0;
  ShaderStage stage = ShaderStage::None;

  auto operator==(const SpecConstant&) const -> bool = default;
};

// what one entry point needs from its pipeline, written by the shader compiler next to the SPIR-V
struct ShaderLayout {
  ShaderStage stage = ShaderStage::All;
  std::string entry_point = {};
  std::vector<DescriptorBinding> bindings = {};
  // sorted by id
  std::vector<SpecConstant> spec_constants = {};
  std::array<u32, 3> local_size_spec_ids = {NO_SPEC_ID, NO_SPEC_ID, NO_SPEC_ID};
  u32 push_constant_offset = 0;
  u32 push_constant_size = 0;
  // empty for every stage but the vertex stage
  std::vector<VertexInput> vertex_inputs = {};
  std::array<u32, 3> local_size = {1, 1, 1};

  auto operator==(const ShaderLayout&) const -> bool = default;
};

struct ShaderCode {
  std::vector<u32> spirv = {};
  ShaderLayout layout = {};
};

auto descriptor_access(DescriptorUsage usage, ShaderStage stages) -> Access;
} // namespace ox::vir
