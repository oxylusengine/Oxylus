#include "Render/Graph/Pipeline.hpp"

#include <algorithm>
#include <map>

#include "Memory/Stack.hpp"
#include "Render/Graph/Context.hpp"
#include "Utils/Log.hpp"

namespace ox::vir {
auto validate_shader_groups(std::span<const ShaderLayout> reflections, std::span<const RayTracingShaderGroup> groups)
  -> std::expected<void, VkResult> {
  const auto is_stage = [&](u32 index, ShaderStage stage) -> bool {
    return index < reflections.size() && reflections[index].stage == stage;
  };
  const auto optional_stage = [&](u32 index, ShaderStage stage) -> bool {
    return index == VK_SHADER_UNUSED_KHR || is_stage(index, stage);
  };
  for (const auto& reflection : reflections) {
    switch (reflection.stage) {
      case ShaderStage::RayGeneration:
      case ShaderStage::Miss         :
      case ShaderStage::Callable     :
      case ShaderStage::ClosestHit   :
      case ShaderStage::AnyHit       :
      case ShaderStage::Intersection : break;
      default                        : return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }
  }
  u32 raygen_count = 0;
  for (const auto& group : groups) {
    if (group.type == VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR) {
      if (
        (!is_stage(group.general, ShaderStage::RayGeneration) && !is_stage(group.general, ShaderStage::Miss) &&
         !is_stage(group.general, ShaderStage::Callable)) ||
        group.closest_hit != VK_SHADER_UNUSED_KHR || group.any_hit != VK_SHADER_UNUSED_KHR ||
        group.intersection != VK_SHADER_UNUSED_KHR
      ) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
      }
      raygen_count += is_stage(group.general, ShaderStage::RayGeneration);
    } else if (
      group.type == VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR ||
      group.type == VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR
    ) {
      if (
        group.general != VK_SHADER_UNUSED_KHR || !optional_stage(group.closest_hit, ShaderStage::ClosestHit) ||
        !optional_stage(group.any_hit, ShaderStage::AnyHit) ||
        (group.type == VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR
           ? group.intersection != VK_SHADER_UNUSED_KHR
           : !is_stage(group.intersection, ShaderStage::Intersection))
      ) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
      }
    } else {
      return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }
  }
  // trace_rays selects the one raygen record owned by this pipeline
  if (raygen_count != 1) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }
  return {};
}

auto VertexLayout::interleaved(std::span<const ShaderLayout> reflections) -> VertexLayout {
  const auto vertex = std::ranges::find(reflections, ShaderStage::Vertex, &ShaderLayout::stage);
  if (vertex == reflections.end() || vertex->vertex_inputs.empty()) {
    return {};
  }

  auto layout = VertexLayout{};
  u32 offset = 0;
  for (const auto& input : vertex->vertex_inputs) {
    layout.attributes.push_back({.location = input.location, .binding = 0, .format = input.format, .offset = offset});
    offset += input.size;
  }

  layout.bindings.push_back({.binding = 0, .stride = offset, .input_rate = VK_VERTEX_INPUT_RATE_VERTEX});
  return layout;
}

auto push_constant_ranges(std::span<const ShaderLayout> reflections) -> std::vector<VkPushConstantRange> {
  auto merged = std::map<std::pair<u32, u32>, ShaderStage>{};
  for (const auto& reflection : reflections) {
    if (reflection.push_constant_size == 0) {
      continue;
    }

    merged[{reflection.push_constant_offset, reflection.push_constant_size}] |= reflection.stage;
  }

  auto ranges = std::vector<VkPushConstantRange>{};
  for (const auto& [range, stages] : merged) {
    ranges.push_back(
      {.stageFlags = static_cast<VkShaderStageFlags>(stages), .offset = range.first, .size = range.second}
    );
  }

  return ranges;
}

static auto is_scalar_descriptor_type(VkDescriptorType type) -> bool {
  switch (type) {
    case VK_DESCRIPTOR_TYPE_SAMPLER                   :
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER    :
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE             :
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE             :
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER      :
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER      :
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER            :
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER            :
    case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT          :
    case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR: return true;
    default                                           : return false;
  }
}

auto validate_descriptor_bindings(
  std::span<const ShaderLayout> reflections, const option<BindlessDescriptorSet>& bindless
) -> std::expected<void, VkResult> {
  if (bindless.has_value() && (bindless->layout == VK_NULL_HANDLE || bindless->set == VK_NULL_HANDLE)) {
    OX_LOG_ERROR("external bindless handles of set {} must not be null", bindless->index);
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }

  struct Shape {
    VkDescriptorType type;
    u32 count;
    bool is_variable_count;
  };
  auto seen = std::map<std::pair<u32, u32>, Shape>{};
  for (const auto& reflection : reflections) {
    for (const auto& binding : reflection.bindings) {
      const auto shape = Shape{binding.descriptor_type, binding.count, binding.is_variable_count};
      const auto [it, is_inserted] = seen.emplace(std::pair{binding.set, binding.binding}, shape);
      if (
        !is_inserted && (it->second.type != shape.type || it->second.count != shape.count ||
                         it->second.is_variable_count != shape.is_variable_count)
      ) {
        OX_LOG_ERROR(
          "shader stages disagree on the shape of descriptor set {} binding {}",
          binding.set,
          binding.binding
        );
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
      }

      if (bindless.has_value() && bindless->index == binding.set) {
        continue;
      }

      if (!is_scalar_descriptor_type(binding.descriptor_type) || binding.count != 1 || binding.is_variable_count) {
        OX_LOG_ERROR(
          "descriptor set {} binding {} is an array, only the bindless set may hold descriptor arrays",
          binding.set,
          binding.binding
        );
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
      }
    }
  }

  return {};
}

auto merged_bindings(std::span<const ShaderLayout> reflections) -> std::vector<DescriptorBinding> {
  auto merged = std::map<std::pair<u32, u32>, DescriptorBinding>{};
  for (const auto& reflection : reflections) {
    for (const auto& binding : reflection.bindings) {
      auto [it, is_inserted] = merged.emplace(std::pair{binding.set, binding.binding}, binding);
      if (is_inserted) {
        continue;
      }

      auto& shared = it->second;
      shared.stages |= binding.stages;
      const auto combined = difference(shared.access, Access::None) | difference(binding.access, Access::None);
      shared.access = is_empty(combined) ? Access::None : combined;
    }
  }

  auto bindings = std::vector<DescriptorBinding>{};
  for (auto& [key, binding] : merged) {
    bindings.push_back(binding);
  }

  return bindings;
}

auto default_shader_groups(std::span<const ShaderLayout> reflections) -> std::vector<RayTracingShaderGroup> {
  auto groups = std::vector<RayTracingShaderGroup>{};
  auto hit_group = RayTracingShaderGroup{.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR};
  auto has_hit_group = false;
  for (u32 index = 0; index < reflections.size(); index++) {
    switch (reflections[index].stage) {
      case ShaderStage::RayGeneration:
      case ShaderStage::Miss         :
      case ShaderStage::Callable:
        groups.push_back({.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, .general = index});
        break;
      case ShaderStage::ClosestHit:
        hit_group.closest_hit = index;
        has_hit_group = true;
        break;
      case ShaderStage::AnyHit:
        hit_group.any_hit = index;
        has_hit_group = true;
        break;
      case ShaderStage::Intersection:
        hit_group.intersection = index;
        hit_group.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR;
        has_hit_group = true;
        break;
      default: break;
    }
  }

  // the shader binding table lays regions out raygen, miss, hit, callable, and groups are
  // numbered in that order too
  std::ranges::stable_sort(groups, {}, [&](const RayTracingShaderGroup& group) {
    switch (reflections[group.general].stage) {
      case ShaderStage::RayGeneration: return 0;
      case ShaderStage::Miss         : return 1;
      default                        : return 3;
    }
  });
  if (has_hit_group) {
    const auto callable = std::ranges::find_if(groups, [&](const RayTracingShaderGroup& group) {
      return reflections[group.general].stage == ShaderStage::Callable;
    });
    groups.insert(callable, hit_group);
  }

  return groups;
}

auto PipelineLayout::create(
  const Context& context,
  std::span<const ShaderLayout> reflections,
  const option<BindlessDescriptorSet>& bindless,
  u32 max_push_descriptors
) -> std::expected<PipelineLayout, VkResult> {
  struct Merged {
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    u32 count = 0;
    bool is_variable_count = false;
    ShaderStage stages = ShaderStage::None;
  };

  auto merged = std::map<std::pair<u32, u32>, Merged>{};
  for (const auto& reflection : reflections) {
    for (const auto& binding : reflection.bindings) {
      auto [it, is_inserted] = merged.emplace(
        std::pair{binding.set, binding.binding},
        Merged{binding.descriptor_type, binding.count, binding.is_variable_count, ShaderStage::None}
      );
      if (!is_inserted && (it->second.type != binding.descriptor_type || it->second.count != binding.count)) {
        OX_LOG_ERROR("stages disagree on descriptor set {} binding {}", binding.set, binding.binding);
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
      }

      it->second.stages |= reflection.stage;
    }
  }

  u32 set_count = 0;
  for (const auto& [key, binding] : merged) {
    set_count = std::max(set_count, key.first + 1);
  }

  if (bindless.has_value()) {
    set_count = std::max(set_count, bindless->index + 1);
  }

  auto layout = PipelineLayout{.bindless = bindless};
  const auto destroy_sets = [&] {
    for (const auto& set : layout.sets) {
      if (set.is_owned) {
        context.fn.vkDestroyDescriptorSetLayout(context.device, set.handle, nullptr);
      }
    }
  };

  for (u32 set = 0; set < set_count; set++) {
    if (bindless.has_value() && bindless->index == set) {
      layout.sets.push_back({.handle = bindless->layout});
      continue;
    }

    auto bindings = std::vector<VkDescriptorSetLayoutBinding>{};
    auto sizes = std::vector<std::pair<VkDescriptorType, u32>>{};
    u32 descriptor_count = 0;
    for (auto it = merged.lower_bound({set, 0}); it != merged.end() && it->first.first == set; ++it) {
      bindings.push_back({
        .binding = it->first.second,
        .descriptorType = it->second.type,
        .descriptorCount = it->second.count,
        .stageFlags = static_cast<VkShaderStageFlags>(it->second.stages),
        .pImmutableSamplers = nullptr,
      });
      sizes.emplace_back(it->second.type, it->second.count);
      descriptor_count += it->second.count;
    }

    const auto is_push = !bindings.empty() && max_push_descriptors != 0 &&
                         std::ranges::none_of(layout.sets, &SetLayout::is_push) &&
                         descriptor_count <= max_push_descriptors;
    const auto create_info = VkDescriptorSetLayoutCreateInfo{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .pNext = nullptr,
      .flags = is_push ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0_u32,
      .bindingCount = static_cast<u32>(bindings.size()),
      .pBindings = bindings.data(),
    };

    VkDescriptorSetLayout handle = VK_NULL_HANDLE;
    if (
      const auto result = context.fn.vkCreateDescriptorSetLayout(context.device, &create_info, nullptr, &handle);
      result != VK_SUCCESS
    ) {
      destroy_sets();
      return std::unexpected(result);
    }

    layout.sets.push_back({.handle = handle, .sizes = std::move(sizes), .is_push = is_push, .is_owned = true});
  }

  layout.push_constant_ranges = vir::push_constant_ranges(reflections);
  auto set_layouts = std::vector<VkDescriptorSetLayout>{};
  for (const auto& set : layout.sets) {
    set_layouts.push_back(set.handle);
  }

  const auto create_info = VkPipelineLayoutCreateInfo{
    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    .pNext = nullptr,
    .flags = 0,
    .setLayoutCount = static_cast<u32>(set_layouts.size()),
    .pSetLayouts = set_layouts.data(),
    .pushConstantRangeCount = static_cast<u32>(layout.push_constant_ranges.size()),
    .pPushConstantRanges = layout.push_constant_ranges.data(),
  };
  if (
    const auto result = context.fn.vkCreatePipelineLayout(context.device, &create_info, nullptr, &layout.handle);
    result != VK_SUCCESS
  ) {
    destroy_sets();
    return std::unexpected(result);
  }

  return layout;
}

auto PipelineLayout::cover(this const PipelineLayout& self, u32 offset, u32 size) -> std::vector<PushRange> {
  auto covered = std::vector<PushRange>{};
  const auto end = offset + size;
  for (const auto& range : self.push_constant_ranges) {
    const auto start = std::max(range.offset, offset);
    const auto stop = std::min(range.offset + range.size, end);
    if (stop > start) {
      covered.push_back({.stages = static_cast<ShaderStage>(range.stageFlags), .offset = start, .size = stop - start});
    }
  }

  return covered;
}

auto PipelineLayout::destroy(this const PipelineLayout& self, const Context& context) -> void {
  context.fn.vkDestroyPipelineLayout(context.device, self.handle, nullptr);
  for (const auto& set : self.sets) {
    if (set.is_owned) {
      context.fn.vkDestroyDescriptorSetLayout(context.device, set.handle, nullptr);
    }
  }
}

auto pack_specialization(std::span<const ShaderLayout> reflections, const Specialization& specialization)
  -> std::expected<PackedSpecialization, VkResult> {
  struct Declared {
    u32 size = 0;
    ShaderStage stages = ShaderStage::None;
  };

  auto merged = std::map<u32, Declared>{};
  for (const auto& reflection : reflections) {
    for (const auto& constant : reflection.spec_constants) {
      auto [it, is_inserted] = merged.emplace(constant.id, Declared{constant.size, constant.stage});
      if (is_inserted) {
        continue;
      }

      if (it->second.size != constant.size) {
        OX_LOG_ERROR("shader stages disagree on the size of specialization constant {}", constant.id);
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
      }

      it->second.stages |= constant.stage;
    }
  }

  // an id no stage declares is fine, the constants in force are whatever the pass set for any of
  // the pipelines it binds
  for (const auto& [id, value] : specialization.values) {
    const auto it = merged.find(id);
    if (it != merged.end() && it->second.size != value.size()) {
      OX_LOG_ERROR(
        "specialization constant {} is given {} bytes but the shader declares {}",
        id,
        value.size(),
        it->second.size
      );
      return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }
  }

  struct Placed {
    u32 id;
    u32 offset;
    u32 size;
    ShaderStage stages;
  };

  auto packed = PackedSpecialization{};
  auto placed = std::vector<Placed>{};
  for (const auto& [id, declared] : merged) {
    const auto value = specialization.get(id);
    if (!value.has_value()) {
      continue;
    }

    const auto offset = static_cast<u32>(packed.data.size());
    const auto bytes = value->bytes();
    packed.data.insert(packed.data.end(), bytes.begin(), bytes.begin() + declared.size);
    placed.push_back({id, offset, declared.size, declared.stages});
  }

  for (const auto& reflection : reflections) {
    auto& entries = packed.entries.emplace_back();
    for (const auto& constant : placed) {
      if (intersects(constant.stages, reflection.stage)) {
        entries.push_back({.constantID = constant.id, .offset = constant.offset, .size = constant.size});
      }
    }
  }

  return packed;
}

struct ShaderModules {
  const Context* context = nullptr;
  std::vector<VkShaderModule> modules = {};

  ShaderModules(const Context& ctx) : context(&ctx) {}
  ShaderModules(const ShaderModules&) = delete;
  ShaderModules(ShaderModules&&) = default;
  ~ShaderModules() {
    for (auto module : modules) {
      context->fn.vkDestroyShaderModule(context->device, module, nullptr);
    }
  }

  auto operator=(const ShaderModules&) -> ShaderModules& = delete;
  auto operator=(ShaderModules&&) -> ShaderModules& = default;

  auto add(this ShaderModules& self, std::span<const u32> spirv) -> std::expected<VkShaderModule, VkResult> {
    const auto create_info = VkShaderModuleCreateInfo{
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .codeSize = spirv.size_bytes(),
      .pCode = spirv.data(),
    };

    VkShaderModule module = VK_NULL_HANDLE;
    if (
      const auto result = self.context->fn.vkCreateShaderModule(self.context->device, &create_info, nullptr, &module);
      result != VK_SUCCESS
    ) {
      return std::unexpected(result);
    }

    self.modules.push_back(module);
    return module;
  }
};

static auto specialization_infos(const PackedSpecialization& packed) -> std::vector<VkSpecializationInfo> {
  auto infos = std::vector<VkSpecializationInfo>{};
  for (const auto& entries : packed.entries) {
    infos.push_back({
      .mapEntryCount = static_cast<u32>(entries.size()),
      .pMapEntries = entries.data(),
      .dataSize = packed.data.size(),
      .pData = packed.data.data(),
    });
  }

  return infos;
}

auto create_graphics_pipelines(const Context& context, std::span<const GraphicsPipelineRequest> requests)
  -> std::expected<std::vector<VkPipeline>, VkResult> {
  ZoneScoped;

  if (requests.empty()) {
    return std::vector<VkPipeline>{};
  }

  // everything a create info points at has to stay put until the pipelines are made
  struct Storage {
    PackedSpecialization packed = {};
    std::vector<VkSpecializationInfo> specializations = {};
    std::vector<VkPipelineShaderStageCreateInfo> stages = {};
    std::vector<VkPipelineColorBlendAttachmentState> blend_attachments = {};
    std::vector<VkViewport> viewports = {};
    std::vector<VkDynamicState> dynamic_states = {};
    std::vector<VkVertexInputBindingDescription> vertex_bindings = {};
    std::vector<VkVertexInputAttributeDescription> vertex_attributes = {};
    VkPipelineVertexInputStateCreateInfo vertex_input = {};
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {};
    VkPipelineTessellationStateCreateInfo tessellation = {};
    VkPipelineViewportStateCreateInfo viewport = {};
    VkPipelineRasterizationConservativeStateCreateInfoEXT conservative = {};
    VkPipelineRasterizationStateCreateInfo rasterization = {};
    VkPipelineMultisampleStateCreateInfo multisample = {};
    VkPipelineDepthStencilStateCreateInfo depth_stencil = {};
    VkPipelineColorBlendStateCreateInfo color_blend = {};
    VkPipelineDynamicStateCreateInfo dynamic = {};
    VkPipelineRenderingCreateInfo rendering = {};
  };

  auto stack = memory::ScopedStack{};
  auto storages = std::vector<Storage>(requests.size());
  auto modules = ShaderModules(context);
  auto create_infos = std::vector<VkGraphicsPipelineCreateInfo>{};

  for (usize index = 0; index < requests.size(); index++) {
    const auto& request = requests[index];
    const auto& state = *request.state;
    auto& storage = storages[index];

    auto packed = pack_specialization(request.reflections, state.specialization);
    if (!packed) {
      return std::unexpected(packed.error());
    }

    storage.packed = std::move(*packed);
    storage.specializations = specialization_infos(storage.packed);
    for (usize stage_index = 0; stage_index < request.reflections.size(); stage_index++) {
      const auto& reflection = request.reflections[stage_index];
      auto module = modules.add(request.info->shaders[stage_index].spirv);
      if (!module) {
        return std::unexpected(module.error());
      }

      const auto& specialization = storage.specializations[stage_index];
      storage.stages.push_back({
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stage = static_cast<VkShaderStageFlagBits>(reflection.stage),
        .module = *module,
        .pName = reflection.entry_point.c_str(),
        .pSpecializationInfo = specialization.mapEntryCount != 0 ? &specialization : nullptr,
      });
    }

    for (const auto& blend : state.blend) {
      storage.blend_attachments.push_back(blend.to_vk());
    }

    for (const auto& viewport : state.viewports) {
      storage.viewports.push_back(viewport.to_vk());
    }

    storage.dynamic_states = state.dynamic_states();
    for (const auto& binding : request.vertex->bindings) {
      storage.vertex_bindings.push_back({
        .binding = binding.binding,
        .stride = binding.stride,
        .inputRate = binding.input_rate,
      });
    }

    for (const auto& attribute : request.vertex->attributes) {
      storage.vertex_attributes.push_back({
        .location = attribute.location,
        .binding = attribute.binding,
        .format = static_cast<VkFormat>(attribute.format),
        .offset = attribute.offset,
      });
    }

    storage.vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .vertexBindingDescriptionCount = static_cast<u32>(storage.vertex_bindings.size()),
      .pVertexBindingDescriptions = storage.vertex_bindings.data(),
      .vertexAttributeDescriptionCount = static_cast<u32>(storage.vertex_attributes.size()),
      .pVertexAttributeDescriptions = storage.vertex_attributes.data(),
    };
    storage.input_assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .topology = state.topology,
      .primitiveRestartEnable = VK_FALSE,
    };
    storage.tessellation = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .patchControlPoints = state.patch_control_points,
    };
    storage.viewport = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .viewportCount = state.viewport_count,
      .pViewports = storage.viewports.empty() ? nullptr : storage.viewports.data(),
      .scissorCount = state.viewport_count,
      .pScissors = state.scissors.empty() ? nullptr : state.scissors.data(),
    };
    storage.conservative = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT,
      .pNext = nullptr,
      .flags = 0,
      .conservativeRasterizationMode = VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT,
      .extraPrimitiveOverestimationSize = 0.0f,
    };
    const auto& raster = state.rasterization;
    storage.rasterization = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .pNext = raster.is_conservative ? &storage.conservative : nullptr,
      .flags = 0,
      .depthClampEnable = raster.is_depth_clamp_enabled ? VK_TRUE : VK_FALSE,
      .rasterizerDiscardEnable = raster.is_rasterizer_discard_enabled ? VK_TRUE : VK_FALSE,
      .polygonMode = raster.polygon_mode,
      .cullMode = raster.cull_mode,
      .frontFace = raster.front_face,
      .depthBiasEnable = raster.is_depth_bias_enabled ? VK_TRUE : VK_FALSE,
      .depthBiasConstantFactor = raster.depth_bias_constant,
      .depthBiasClamp = raster.depth_bias_clamp,
      .depthBiasSlopeFactor = raster.depth_bias_slope,
      .lineWidth = raster.line_width,
    };
    storage.multisample = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .rasterizationSamples = static_cast<VkSampleCountFlagBits>(state.rendering.samples),
      .sampleShadingEnable = VK_FALSE,
      .minSampleShading = 0.0f,
      .pSampleMask = nullptr,
      .alphaToCoverageEnable = VK_FALSE,
      .alphaToOneEnable = VK_FALSE,
    };
    storage.depth_stencil = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .depthTestEnable = state.depth.is_test_enabled ? VK_TRUE : VK_FALSE,
      .depthWriteEnable = state.depth.is_write_enabled ? VK_TRUE : VK_FALSE,
      .depthCompareOp = state.depth.compare_op,
      .depthBoundsTestEnable = VK_FALSE,
      .stencilTestEnable = state.stencil.is_test_enabled ? VK_TRUE : VK_FALSE,
      .front = state.stencil.front,
      .back = state.stencil.back,
      .minDepthBounds = 0.0f,
      .maxDepthBounds = 1.0f,
    };
    storage.color_blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .logicOpEnable = VK_FALSE,
      .logicOp = VK_LOGIC_OP_CLEAR,
      .attachmentCount = static_cast<u32>(storage.blend_attachments.size()),
      .pAttachments = storage.blend_attachments.data(),
      .blendConstants = {0.0f, 0.0f, 0.0f, 0.0f},
    };
    storage.dynamic = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .dynamicStateCount = static_cast<u32>(storage.dynamic_states.size()),
      .pDynamicStates = storage.dynamic_states.data(),
    };
    auto color_formats = stack.alloc<VkFormat>(state.rendering.color_formats.size());
    for (usize color_index = 0; color_index < color_formats.size(); color_index++) {
      color_formats[color_index] = static_cast<VkFormat>(state.rendering.color_formats[color_index]);
    }
    storage.rendering = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .pNext = nullptr,
      .viewMask = state.rendering.view_mask,
      .colorAttachmentCount = static_cast<u32>(state.rendering.color_formats.size()),
      .pColorAttachmentFormats = color_formats.data(),
      .depthAttachmentFormat = static_cast<VkFormat>(state.rendering.depth_format),
      .stencilAttachmentFormat = static_cast<VkFormat>(state.rendering.stencil_format),
    };

    create_infos.push_back({
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = &storage.rendering,
      .flags = 0,
      .stageCount = static_cast<u32>(storage.stages.size()),
      .pStages = storage.stages.data(),
      .pVertexInputState = &storage.vertex_input,
      .pInputAssemblyState = &storage.input_assembly,
      .pTessellationState = state.patch_control_points != 0 ? &storage.tessellation : nullptr,
      .pViewportState = &storage.viewport,
      .pRasterizationState = &storage.rasterization,
      .pMultisampleState = &storage.multisample,
      .pDepthStencilState = &storage.depth_stencil,
      .pColorBlendState = &storage.color_blend,
      .pDynamicState = &storage.dynamic,
      .layout = request.layout,
      .renderPass = VK_NULL_HANDLE,
      .subpass = 0,
      .basePipelineHandle = VK_NULL_HANDLE,
      .basePipelineIndex = -1,
    });
  }

  auto pipelines = std::vector<VkPipeline>(create_infos.size(), VK_NULL_HANDLE);
  const auto result = context.fn.vkCreateGraphicsPipelines(
    context.device,
    VK_NULL_HANDLE,
    static_cast<u32>(create_infos.size()),
    create_infos.data(),
    nullptr,
    pipelines.data()
  );
  if (result != VK_SUCCESS) {
    for (auto pipeline : pipelines) {
      if (pipeline != VK_NULL_HANDLE) {
        context.fn.vkDestroyPipeline(context.device, pipeline, nullptr);
      }
    }

    return std::unexpected(result);
  }

  return pipelines;
}

auto create_compute_pipelines(const Context& context, std::span<const ComputePipelineRequest> requests)
  -> std::expected<std::vector<VkPipeline>, VkResult> {
  ZoneScoped;

  if (requests.empty()) {
    return std::vector<VkPipeline>{};
  }

  auto packed = std::vector<PackedSpecialization>{};
  auto specializations = std::vector<std::vector<VkSpecializationInfo>>{};
  auto modules = ShaderModules(context);
  auto create_infos = std::vector<VkComputePipelineCreateInfo>{};
  packed.reserve(requests.size());
  specializations.reserve(requests.size());
  for (const auto& request : requests) {
    auto request_packed = pack_specialization(std::span(request.reflection, 1), *request.specialization);
    if (!request_packed) {
      return std::unexpected(request_packed.error());
    }

    packed.push_back(std::move(*request_packed));
    specializations.push_back(specialization_infos(packed.back()));

    auto module = modules.add(request.info->shader.spirv);
    if (!module) {
      return std::unexpected(module.error());
    }

    const auto& specialization = specializations.back().front();
    create_infos.push_back({
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .stage =
        {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .pNext = nullptr,
          .flags = 0,
          .stage = static_cast<VkShaderStageFlagBits>(ShaderStage::Compute),
          .module = *module,
          .pName = request.reflection->entry_point.c_str(),
          .pSpecializationInfo = specialization.mapEntryCount != 0 ? &specialization : nullptr,
        },
      .layout = request.layout,
      .basePipelineHandle = VK_NULL_HANDLE,
      .basePipelineIndex = -1,
    });
  }

  auto pipelines = std::vector<VkPipeline>(create_infos.size(), VK_NULL_HANDLE);
  const auto result = context.fn.vkCreateComputePipelines(
    context.device,
    VK_NULL_HANDLE,
    static_cast<u32>(create_infos.size()),
    create_infos.data(),
    nullptr,
    pipelines.data()
  );
  if (result != VK_SUCCESS) {
    for (auto pipeline : pipelines) {
      if (pipeline != VK_NULL_HANDLE) {
        context.fn.vkDestroyPipeline(context.device, pipeline, nullptr);
      }
    }

    return std::unexpected(result);
  }

  return pipelines;
}

auto create_ray_tracing_pipeline(
  const Context& context,
  const RayTracingPipelineInfo& info,
  std::span<const ShaderLayout> reflections,
  std::span<const RayTracingShaderGroup> groups,
  const Specialization& specialization,
  VkPipelineLayout layout
) -> std::expected<VkPipeline, VkResult> {
  ZoneScoped;

  if (!context.fn.vkCreateRayTracingPipelinesKHR) {
    OX_LOG_ERROR("ray tracing pipeline {} declared on a device without ray tracing", info.name);
    return std::unexpected(VK_ERROR_FEATURE_NOT_PRESENT);
  }
  if (
    info.shaders.size() != reflections.size() || info.max_recursion_depth == 0 ||
    info.max_recursion_depth > context.ray_tracing.max_ray_recursion_depth
  ) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }
  const auto valid = validate_shader_groups(reflections, groups);
  if (!valid) {
    return std::unexpected(valid.error());
  }

  auto packed = pack_specialization(reflections, specialization);
  if (!packed) {
    return std::unexpected(packed.error());
  }

  const auto specializations = specialization_infos(*packed);
  auto modules = ShaderModules(context);
  auto stages = std::vector<VkPipelineShaderStageCreateInfo>{};
  for (usize index = 0; index < reflections.size(); index++) {
    auto module = modules.add(info.shaders[index].spirv);
    if (!module) {
      return std::unexpected(module.error());
    }

    const auto& stage_specialization = specializations[index];
    stages.push_back({
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .stage = static_cast<VkShaderStageFlagBits>(reflections[index].stage),
      .module = *module,
      .pName = reflections[index].entry_point.c_str(),
      .pSpecializationInfo = stage_specialization.mapEntryCount != 0 ? &stage_specialization : nullptr,
    });
  }

  auto group_infos = std::vector<VkRayTracingShaderGroupCreateInfoKHR>{};
  for (const auto& group : groups) {
    group_infos.push_back({
      .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
      .pNext = nullptr,
      .type = group.type,
      .generalShader = group.general,
      .closestHitShader = group.closest_hit,
      .anyHitShader = group.any_hit,
      .intersectionShader = group.intersection,
      .pShaderGroupCaptureReplayHandle = nullptr,
    });
  }

  const auto create_info = VkRayTracingPipelineCreateInfoKHR{
    .sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,
    .pNext = nullptr,
    .flags = 0,
    .stageCount = static_cast<u32>(stages.size()),
    .pStages = stages.data(),
    .groupCount = static_cast<u32>(group_infos.size()),
    .pGroups = group_infos.data(),
    .maxPipelineRayRecursionDepth = info.max_recursion_depth,
    .pLibraryInfo = nullptr,
    .pLibraryInterface = nullptr,
    .pDynamicState = nullptr,
    .layout = layout,
    .basePipelineHandle = VK_NULL_HANDLE,
    .basePipelineIndex = -1,
  };

  VkPipeline pipeline = VK_NULL_HANDLE;
  const auto result = context.fn.vkCreateRayTracingPipelinesKHR(
    context.device,
    VK_NULL_HANDLE,
    VK_NULL_HANDLE,
    1,
    &create_info,
    nullptr,
    &pipeline
  );
  if (result != VK_SUCCESS) {
    return std::unexpected(result);
  }

  return pipeline;
}
} // namespace ox::vir
