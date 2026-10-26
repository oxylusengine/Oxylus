#include "ShaderLayout.hpp"

#include <algorithm>
#include <fmt/format.h>
#include <slang-com-ptr.h>
#include <vir/Types.hpp>

#include "Core/Option.hpp"

namespace ox::rc {
static auto to_graph_stage(SlangStage stage) -> option<vir::ShaderStage> {
  switch (stage) {
    case SLANG_STAGE_VERTEX        : return vir::ShaderStage::Vertex;
    case SLANG_STAGE_HULL          : return vir::ShaderStage::TessellationControl;
    case SLANG_STAGE_DOMAIN        : return vir::ShaderStage::TessellationEvaluation;
    case SLANG_STAGE_GEOMETRY      : return vir::ShaderStage::Geometry;
    case SLANG_STAGE_FRAGMENT      : return vir::ShaderStage::Fragment;
    case SLANG_STAGE_COMPUTE       : return vir::ShaderStage::Compute;
    case SLANG_STAGE_RAY_GENERATION: return vir::ShaderStage::RayGeneration;
    case SLANG_STAGE_INTERSECTION  : return vir::ShaderStage::Intersection;
    case SLANG_STAGE_ANY_HIT       : return vir::ShaderStage::AnyHit;
    case SLANG_STAGE_CLOSEST_HIT   : return vir::ShaderStage::ClosestHit;
    case SLANG_STAGE_MISS          : return vir::ShaderStage::Miss;
    case SLANG_STAGE_CALLABLE      : return vir::ShaderStage::Callable;
    case SLANG_STAGE_MESH          : return vir::ShaderStage::Mesh;
    case SLANG_STAGE_AMPLIFICATION : return vir::ShaderStage::Task;
    default                        : return nullopt;
  }
}

struct DescriptorKind {
  VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
  vir::DescriptorUsage usage = vir::DescriptorUsage::None;
};

static auto to_descriptor_kind(slang::BindingType type) -> option<DescriptorKind> {
  using enum slang::BindingType;
  switch (type) {
    case Sampler       : return DescriptorKind{VK_DESCRIPTOR_TYPE_SAMPLER, vir::DescriptorUsage::None};
    case Texture       : return DescriptorKind{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, vir::DescriptorUsage::Sampled};
    case MutableTexture: return DescriptorKind{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, vir::DescriptorUsage::ReadWrite};
    case CombinedTextureSampler:
      return DescriptorKind{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, vir::DescriptorUsage::Sampled};
    case ConstantBuffer: return DescriptorKind{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, vir::DescriptorUsage::Uniform};
    case TypedBuffer   : return DescriptorKind{VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, vir::DescriptorUsage::Read};
    case MutableTypedBuffer:
      return DescriptorKind{VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, vir::DescriptorUsage::ReadWrite};
    case RawBuffer       : return DescriptorKind{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, vir::DescriptorUsage::Read};
    case MutableRawBuffer: return DescriptorKind{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, vir::DescriptorUsage::ReadWrite};
    case InputRenderTarget:
      return DescriptorKind{VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, vir::DescriptorUsage::InputAttachment};
    case RayTracingAccelerationStructure:
      return DescriptorKind{
        VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
        vir::DescriptorUsage::AccelerationStructure,
      };
    default: return nullopt;
  }
}

struct LayoutWalk {
  vir::ShaderStage stage = vir::ShaderStage::All;
  std::vector<vir::DescriptorBinding> bindings = {};
  option<std::pair<u32, u32>> push_constants = nullopt;
  std::string error = {};

  auto add_push_constants(this LayoutWalk& self, u32 begin, u32 end) -> void {
    if (end <= begin) {
      return;
    }

    self.push_constants =
      self.push_constants.has_value()
        ? std::pair{std::min(self.push_constants->first, begin), std::max(self.push_constants->second, end)}
        : std::pair{begin, end};
  }
};

// the bytes a push constant block's members actually occupy, which is what the pipeline layout
// declares. Trailing padding the block's size rounds up to never reaches the shader
static auto add_push_constant_block(LayoutWalk& walk, slang::TypeLayoutReflection* element) -> void {
  if (element->getKind() != slang::TypeReflection::Kind::Struct || element->getFieldCount() == 0) {
    walk.add_push_constants(0, static_cast<u32>(element->getSize(slang::ParameterCategory::Uniform)));
    return;
  }

  for (u32 index = 0; index < element->getFieldCount(); index++) {
    auto* field = element->getFieldByIndex(index);
    const auto size = field->getTypeLayout()->getSize(slang::ParameterCategory::Uniform);
    if (size == 0 || size == SLANG_UNBOUNDED_SIZE) {
      continue;
    }

    const auto offset = static_cast<u32>(field->getOffset(slang::ParameterCategory::Uniform));
    walk.add_push_constants(offset, offset + static_cast<u32>(size));
  }
}

static auto add_ranges(LayoutWalk& walk, slang::TypeLayoutReflection* type_layout, u32 space_base, u32 binding_base)
  -> void {
  for (SlangInt set_index = 0; set_index < type_layout->getDescriptorSetCount(); set_index++) {
    const auto space = space_base + static_cast<u32>(type_layout->getDescriptorSetSpaceOffset(set_index));
    for (SlangInt range = 0; range < type_layout->getDescriptorSetDescriptorRangeCount(set_index); range++) {
      if (
        type_layout->getDescriptorSetDescriptorRangeCategory(set_index, range) !=
        slang::ParameterCategory::DescriptorTableSlot
      ) {
        continue;
      }

      const auto binding_type = type_layout->getDescriptorSetDescriptorRangeType(set_index, range);
      const auto kind = to_descriptor_kind(binding_type);
      if (!kind.has_value()) {
        walk.error = fmt::format(
          "descriptor of binding type {} has no Vulkan equivalent",
          std::to_underlying(binding_type)
        );
        return;
      }

      const auto count = type_layout->getDescriptorSetDescriptorRangeDescriptorCount(set_index, range);
      const auto is_unbounded = count < 0 || static_cast<usize>(count) == SLANG_UNBOUNDED_SIZE;
      walk.bindings.push_back({
        .set = space,
        .binding = binding_base +
                   static_cast<u32>(type_layout->getDescriptorSetDescriptorRangeIndexOffset(set_index, range)),
        .descriptor_type = kind->type,
        .count = is_unbounded ? 1_u32 : static_cast<u32>(count),
        .is_variable_count = is_unbounded,
        .stages = walk.stage,
        .usage = kind->usage,
      });
    }
  }

  for (SlangInt sub_object = 0; sub_object < type_layout->getSubObjectRangeCount(); sub_object++) {
    const auto range = type_layout->getSubObjectRangeBindingRangeIndex(sub_object);
    auto* leaf = type_layout->getBindingRangeLeafTypeLayout(range);
    switch (type_layout->getBindingRangeType(range)) {
      case slang::BindingType::ParameterBlock: {
        // the block's own set, with its ordinary data in a uniform buffer ahead of its resources
        const auto space = space_base + static_cast<u32>(type_layout->getSubObjectRangeSpaceOffset(sub_object));
        auto* element = leaf->getElementTypeLayout();
        auto* container = leaf->getContainerVarLayout();
        if (element->getSize(slang::ParameterCategory::Uniform) > 0 && container != nullptr) {
          walk.bindings.push_back({
            .set = space,
            .binding = static_cast<u32>(container->getOffset(slang::ParameterCategory::DescriptorTableSlot)),
            .descriptor_type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .count = 1,
            .is_variable_count = false,
            .stages = walk.stage,
            .usage = vir::DescriptorUsage::Uniform,
          });
        }

        auto* element_var = leaf->getElementVarLayout();
        const auto element_binding = element_var != nullptr
                                       ? static_cast<u32>(
                                           element_var->getOffset(slang::ParameterCategory::DescriptorTableSlot)
                                         )
                                       : 0_u32;
        add_ranges(walk, element, space, element_binding);
        break;
      }
      case slang::BindingType::PushConstant: {
        // entry point uniforms land here, and a ParameterBlock among them is a sub-object of the block
        add_push_constant_block(walk, leaf->getElementTypeLayout());
        add_ranges(
          walk,
          leaf->getElementTypeLayout(),
          space_base + static_cast<u32>(type_layout->getSubObjectRangeSpaceOffset(sub_object)),
          binding_base
        );
        break;
      }
      case slang::BindingType::ConstantBuffer: {
        // the buffer itself is one of the ranges above, a ParameterBlock nested in it is not
        add_ranges(
          walk,
          leaf->getElementTypeLayout(),
          space_base + static_cast<u32>(type_layout->getSubObjectRangeSpaceOffset(sub_object)),
          binding_base
        );
        break;
      }
      default: break;
    }

    if (!walk.error.empty()) {
      return;
    }
  }
}

static auto vertex_format(slang::TypeLayoutReflection* type_layout) -> option<std::pair<vir::Format, u32>> {
  using Kind = slang::TypeReflection::Kind;
  using Scalar = slang::TypeReflection::ScalarType;
  const auto kind = type_layout->getKind();
  if (kind != Kind::Scalar && kind != Kind::Vector) {
    return nullopt;
  }

  const auto count = kind == Kind::Vector ? static_cast<u32>(type_layout->getElementCount()) : 1_u32;
  const auto pick = [count](std::array<vir::Format, 4> formats, u32 width) -> option<std::pair<vir::Format, u32>> {
    if (count == 0 || count > 4) {
      return nullopt;
    }

    return std::pair{formats[count - 1], width * count};
  };

  switch (type_layout->getScalarType()) {
    case Scalar::Float32:
      return pick(
        {vir::Format::R32Sfloat, vir::Format::RG32Sfloat, vir::Format::RGB32Sfloat, vir::Format::RGBA32Sfloat},
        4
      );
    case Scalar::Int32:
      return pick({vir::Format::R32Sint, vir::Format::RG32Sint, vir::Format::RGB32Sint, vir::Format::RGBA32Sint}, 4);
    case Scalar::UInt32:
      return pick({vir::Format::R32Uint, vir::Format::RG32Uint, vir::Format::RGB32Uint, vir::Format::RGBA32Uint}, 4);
    case Scalar::Float16:
      return pick(
        {vir::Format::R16Sfloat, vir::Format::RG16Sfloat, vir::Format::RGB16Sfloat, vir::Format::RGBA16Sfloat},
        2
      );
    case Scalar::Int16:
      return pick({vir::Format::R16Sint, vir::Format::RG16Sint, vir::Format::RGB16Sint, vir::Format::RGBA16Sint}, 2);
    case Scalar::UInt16:
      return pick({vir::Format::R16Uint, vir::Format::RG16Uint, vir::Format::RGB16Uint, vir::Format::RGBA16Uint}, 2);
    case Scalar::Float64:
      return pick(
        {vir::Format::R64Sfloat, vir::Format::RG64Sfloat, vir::Format::RGB64Sfloat, vir::Format::RGBA64Sfloat},
        8
      );
    default: return nullopt;
  }
}

static auto is_system_value(slang::VariableLayoutReflection* variable) -> bool {
  const auto* semantic = variable->getSemanticName();
  if (semantic == nullptr) {
    return false;
  }

  const auto name = std::string_view(semantic);
  return name.size() >= 3 && (name[0] == 'S' || name[0] == 's') && (name[1] == 'V' || name[1] == 'v') && name[2] == '_';
}

static auto add_vertex_inputs(
  std::vector<vir::VertexInput>& inputs, slang::VariableLayoutReflection* variable, u32 location_base
) -> void {
  if (is_system_value(variable)) {
    return;
  }

  auto* type_layout = variable->getTypeLayout();
  const auto location = location_base + static_cast<u32>(variable->getOffset(slang::ParameterCategory::VaryingInput));
  if (type_layout->getKind() == slang::TypeReflection::Kind::Struct) {
    for (u32 index = 0; index < type_layout->getFieldCount(); index++) {
      add_vertex_inputs(inputs, type_layout->getFieldByIndex(index), location);
    }
    return;
  }

  if (variable->getCategory() != slang::ParameterCategory::VaryingInput) {
    return;
  }

  // matrices span several locations and are rebuilt from vectors by hand where the engine needs one
  if (const auto format = vertex_format(type_layout); format.has_value()) {
    inputs.push_back({.location = location, .format = format->first, .size = format->second});
  }
}

auto reflect_shader_layout(slang::IComponentType* linked_program, std::string_view entry_point_name)
  -> std::expected<vir::ShaderLayout, std::string> {
  ZoneScoped;

  auto* program_layout = linked_program->getLayout();
  auto* entry_point = program_layout != nullptr ? program_layout->getEntryPointByIndex(0) : nullptr;
  if (entry_point == nullptr) {
    return std::unexpected("the linked program has no entry point layout");
  }

  const auto stage = to_graph_stage(entry_point->getStage());
  if (!stage.has_value()) {
    return std::unexpected(
      fmt::format("stage {} has no Vulkan pipeline stage", std::to_underlying(entry_point->getStage()))
    );
  }

  auto walk = LayoutWalk{.stage = *stage};
  add_ranges(walk, program_layout->getGlobalParamsTypeLayout(), 0, 0);
  if (walk.error.empty()) {
    // the first set the entry point's own blocks take, after every set the globals claimed
    const auto space = static_cast<u32>(
      entry_point->getVarLayout()->getOffset(slang::ParameterCategory::SubElementRegisterSpace)
    );
    add_ranges(walk, entry_point->getTypeLayout(), space, 0);
  }

  if (!walk.error.empty()) {
    return std::unexpected(std::move(walk.error));
  }

  std::ranges::sort(walk.bindings, {}, [](const vir::DescriptorBinding& binding) {
    return std::pair{binding.set, binding.binding};
  });
  const auto [first, last] = std::ranges::unique(walk.bindings, {}, [](const vir::DescriptorBinding& binding) {
    return std::pair{binding.set, binding.binding};
  });
  walk.bindings.erase(first, last);

  auto layout = vir::ShaderLayout{
    .stage = *stage,
    .entry_point = std::string(entry_point_name),
    .bindings = std::move(walk.bindings),
  };
  if (walk.push_constants.has_value()) {
    layout.push_constant_offset = walk.push_constants->first;
    layout.push_constant_size = walk.push_constants->second - walk.push_constants->first;
  }

  for (u32 index = 0; index < program_layout->getParameterCount(); index++) {
    auto* parameter = program_layout->getParameterByIndex(index);
    if (parameter->getCategory() != slang::ParameterCategory::SpecializationConstant) {
      continue;
    }

    const auto size = parameter->getTypeLayout()->getSize(slang::ParameterCategory::Uniform);
    layout.spec_constants.push_back({
      .id = static_cast<u32>(parameter->getOffset(slang::ParameterCategory::SpecializationConstant)),
      .size = size == 8 ? 8_u32 : 4_u32,
      .stage = *stage,
    });
  }

  std::ranges::sort(layout.spec_constants, {}, &vir::SpecConstant::id);

  if (*stage == vir::ShaderStage::Compute || *stage == vir::ShaderStage::Mesh || *stage == vir::ShaderStage::Task) {
    auto sizes = std::array<SlangUInt, 3>{};
    entry_point->getComputeThreadGroupSize(3, sizes.data());
    if (std::ranges::all_of(sizes, [](SlangUInt axis) { return axis > 0; })) {
      layout.local_size = {static_cast<u32>(sizes[0]), static_cast<u32>(sizes[1]), static_cast<u32>(sizes[2])};
    }
  }

  if (*stage == vir::ShaderStage::Vertex) {
    for (u32 index = 0; index < entry_point->getParameterCount(); index++) {
      add_vertex_inputs(layout.vertex_inputs, entry_point->getParameterByIndex(index), 0);
    }

    std::ranges::sort(layout.vertex_inputs, {}, &vir::VertexInput::location);
  }

  return layout;
}

auto remove_unused_bindings(vir::ShaderLayout& layout, slang::IComponentType* linked_program) -> void {
  ZoneScoped;

  auto metadata = Slang::ComPtr<slang::IMetadata>();
  auto diagnostics = Slang::ComPtr<slang::IBlob>();
  if (
    SLANG_FAILED(linked_program->getEntryPointMetadata(0, 0, metadata.writeRef(), diagnostics.writeRef())) || !metadata
  ) {
    return;
  }

  std::erase_if(layout.bindings, [&metadata](const vir::DescriptorBinding& binding) -> bool {
    auto is_used = true;
    const auto queried = metadata->isParameterLocationUsed(
      SLANG_PARAMETER_CATEGORY_DESCRIPTOR_TABLE_SLOT,
      binding.set,
      binding.binding,
      is_used
    );
    return SLANG_SUCCEEDED(queried) && !is_used;
  });
}
} // namespace ox::rc
