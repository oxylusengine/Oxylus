#include "Render/RenderContext.hpp"

#include <algorithm>
#include <ankerl/svector.h>
#include <ranges>
#include <sstream>
#include <string_view>
#include <vuk/ImageAttachment.hpp>
#include <vuk/RenderGraph.hpp>
#include <vuk/runtime/CommandBuffer.hpp>
#include <vuk/runtime/ThisThreadExecutor.hpp>
#include <vuk/runtime/vk/Allocator.hpp>
#include <vuk/runtime/vk/AllocatorHelpers.hpp>
#include <vuk/runtime/vk/Pipeline.hpp>
#include <vuk/runtime/vk/PipelineInstance.hpp>
#include <vuk/runtime/vk/Query.hpp>
#include <zpp_bits.h>

#include "Core/App.hpp"
#include "Core/Enum.hpp"
#include "Memory/Stack.hpp"
#include "Render/Renderer.hpp"
#include "Render/UploadBatch.hpp"
#include "Render/Window.hpp"
#include "Utils/Profiler.hpp"

namespace ox {
// Must outlive every graph it compiles. `allocate_node_links` puts `node->links` in the compiler's
// pool while the nodes themselves live in the process-global IRModule, so a temporary compiler leaves
// them pointing at freed memory and the next compile trips vuk's SSA assert. Reach it through
// `get_compiler` rather than declaring a local one.
thread_local vuk::Compiler this_thread_compiler;

// i hate this
PFN_vkCreateDescriptorPool vkCreateDescriptorPool = nullptr;
PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout = nullptr;
PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets = nullptr;
PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets = nullptr;

PFN_vkGetAccelerationStructureDeviceAddressKHR vkGetAccelerationStructureDeviceAddressKHR = nullptr;
PFN_vkCmdWriteAccelerationStructuresPropertiesKHR vkCmdWriteAccelerationStructuresPropertiesKHR = nullptr;
PFN_vkCmdCopyAccelerationStructureKHR vkCmdCopyAccelerationStructureKHR = nullptr;

template <typename T>
static auto query_device_feature(const vkb::Instance& instance, VkPhysicalDevice physical_device, VkStructureType type)
  -> T {
  T feature = {};
  feature.sType = type;

  const auto get_features_2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
    instance.fp_vkGetInstanceProcAddr(instance.instance, "vkGetPhysicalDeviceFeatures2")
  );
  if (!get_features_2) {
    return feature;
  }

  VkPhysicalDeviceFeatures2 features_2 = {};
  features_2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  features_2.pNext = &feature;
  get_features_2(physical_device, &features_2);

  return feature;
}

static auto query_device_features(const vkb::Instance& instance, VkPhysicalDevice physical_device)
  -> VkPhysicalDeviceFeatures {
  const auto get_features_2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
    instance.fp_vkGetInstanceProcAddr(instance.instance, "vkGetPhysicalDeviceFeatures2")
  );
  if (get_features_2 == nullptr) {
    return {};
  }

  auto features_2 = VkPhysicalDeviceFeatures2{};
  features_2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  get_features_2(physical_device, &features_2);
  return features_2.features;
}

static auto supports_optimal_format_features(
  const vkb::Instance& instance,
  VkPhysicalDevice physical_device,
  VkFormat format,
  VkFormatFeatureFlags required_features
) -> bool {
  const auto get_format_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
    instance.fp_vkGetInstanceProcAddr(instance.instance, "vkGetPhysicalDeviceFormatProperties")
  );
  if (get_format_properties == nullptr) {
    return false;
  }

  auto properties = VkFormatProperties{};
  get_format_properties(physical_device, format, &properties);
  return (properties.optimalTilingFeatures & required_features) == required_features;
}

static auto ray_tracing_stage_order(ShaderStage stage) -> u32 {
  switch (stage) {
    case ShaderStage::RayGeneration: return 0;
    case ShaderStage::Miss         : return 1;
    case ShaderStage::Callable     : return 2;
    default                        : return 3;
  }
}

static auto missing_shader_feature(RenderContext::Feature device_features, ShaderFeatureFlag required)
  -> std::string_view {
  if ((required & ShaderFeatureFlag::MeshShaders) && !(device_features & RenderContext::Feature::MeshShaders)) {
    return "mesh shader";
  }
  if ((required & ShaderFeatureFlag::RayTracing) && !(device_features & RenderContext::Feature::RayTracing)) {
    return "ray tracing";
  }
  if (
    (required & ShaderFeatureFlag::RayTracingPipeline) &&
    !(device_features & RenderContext::Feature::RayTracingPipeline)
  ) {
    return "ray tracing pipeline";
  }

  return {};
}

static VkBool32 debug_callback(
  const VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
  VkDebugUtilsMessageTypeFlagsEXT messageType,
  const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
  void* pUserData
) {
  std::string prefix;
  if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) {
    prefix = "VULKAN VERBOSE: ";
  } else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
    prefix = "VULKAN INFO: ";
  } else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
    prefix = "VULKAN WARNING: ";
  } else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    prefix = "VULKAN ERROR: ";
  }

  std::stringstream debug_message;
  debug_message << prefix << "[" << pCallbackData->messageIdNumber << "][" << pCallbackData->pMessageIdName
                << "] : " << pCallbackData->pMessage;

  if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) {
    OX_LOG_INFO("{}", debug_message.str());
  } else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
    OX_LOG_INFO("{}", debug_message.str());
  } else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
    OX_LOG_WARN(debug_message.str().c_str());
    // OX_DEBUGBREAK();
  } else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    OX_LOG_ERROR("{}", debug_message.str());
  }
  return VK_FALSE;
}

vuk::Swapchain make_swapchain(
  vuk::Runtime& runtime,
  vuk::Allocator& allocator,
  vkb::Device& vkbdevice,
  VkSurfaceKHR surface,
  option<vuk::Swapchain> old_swapchain,
  vuk::PresentModeKHR present_mode,
  u32 frame_count
) {
  vkb::SwapchainBuilder swb(vkbdevice, surface);
  swb.set_desired_min_image_count(frame_count)
    .set_desired_format(
      vuk::SurfaceFormatKHR{.format = vuk::Format::eR8G8B8A8Srgb, .colorSpace = vuk::ColorSpaceKHR::eSrgbNonlinear}
    )
    .add_fallback_format(
      vuk::SurfaceFormatKHR{.format = vuk::Format::eB8G8R8A8Srgb, .colorSpace = vuk::ColorSpaceKHR::eSrgbNonlinear}
    )
    .set_image_usage_flags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
    .set_desired_present_mode(static_cast<VkPresentModeKHR>(present_mode));

  bool recycling = false;
  vkb::Result vkswapchain = {vkb::Swapchain{}};
  if (!old_swapchain) {
    vkswapchain = swb.build();
    old_swapchain.emplace(allocator, vkswapchain->image_count);
  } else {
    recycling = true;
    swb.set_old_swapchain(old_swapchain->swapchain);
    vkswapchain = swb.build();
  }

  if (recycling) {
    allocator.deallocate(std::span{&old_swapchain->swapchain, 1});
    for (auto& iv : old_swapchain->images) {
      allocator.deallocate(std::span{&iv.image_view, 1});
    }
  }

  auto images = *vkswapchain->get_images();
  auto views = *vkswapchain->get_image_views();

  old_swapchain->images.clear();

  for (uint32_t i = 0; i < (uint32_t)images.size(); i++) {
    auto name = fmt::format("swapchain_image_{}", i);
    runtime.set_name(images[i], vuk::Name(name));
    vuk::ImageAttachment attachment = {
      .image = vuk::Image{.image = images[i], .allocation = nullptr},
      .image_view = vuk::ImageView{{0}, views[i]},
      .usage = vuk::ImageUsageFlagBits::eColorAttachment | vuk::ImageUsageFlagBits::eTransferDst,
      .extent = {.width = vkswapchain->extent.width, .height = vkswapchain->extent.height, .depth = 1},
      .format = static_cast<vuk::Format>(vkswapchain->image_format),
      .sample_count = vuk::Samples::e1,
      .view_type = vuk::ImageViewType::e2D,
      .components = {},
      .base_level = 0,
      .level_count = 1,
      .base_layer = 0,
      .layer_count = 1,
    };
    old_swapchain->images.push_back(attachment);
  }

  old_swapchain->swapchain = vkswapchain->swapchain;
  old_swapchain->surface = surface;

  return std::move(*old_swapchain);
}

auto RenderContext::create_context(this RenderContext& self, const Window& window, bool vulkan_validation_layers)
  -> void {
  ZoneScoped;
  vkb::InstanceBuilder builder;
  builder //
    .set_app_name("Oxylus App")
    .set_engine_name("Oxylus")
    .require_api_version(1, 3, 0)
    .set_app_version(0, 1, 0);

  if (vulkan_validation_layers) {
    OX_LOG_INFO("Enabled vulkan validation layers.");
    builder.request_validation_layers();
  }

  builder.enable_extension(VK_KHR_SURFACE_EXTENSION_NAME)
    .enable_extension(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)
    .set_debug_callback(
      [](
        const VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
        const VkDebugUtilsMessageTypeFlagsEXT messageType,
        const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
        void* pUserData
      ) -> VkBool32 { return debug_callback(messageSeverity, messageType, pCallbackData, pUserData); }
    );
  ;

  auto inst_ret = builder.build();
  if (!inst_ret) {
    OX_LOG_ERROR(
      "Couldn't initialize the instance! Make sure your GPU drivers are up to date and it supports Vulkan 1.3"
    );
  }

  self.vkb_instance = inst_ret.value();
  auto instance = self.vkb_instance.instance;
  vkb::PhysicalDeviceSelector selector{self.vkb_instance};
  self.surface = window.get_surface(instance);
  selector //
    .set_surface(self.surface)
    .set_minimum_version(1, 3)
    .disable_portability_subset();
#ifdef OX_USE_LLVMPIPE
  selector.prefer_gpu_device_type(vkb::PreferredDeviceType::cpu);
  selector.allow_any_gpu_device_type(false);
#else
  selector.prefer_gpu_device_type(vkb::PreferredDeviceType::discrete);
#endif

  selector.add_required_extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME)
    .add_required_extension(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)
#ifndef OX_PLATFORM_MACOSX
    .add_required_extension(VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME)
#endif
    .add_required_extension(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);

  if (auto phys_ret = selector.select(); !phys_ret) {
    OX_LOG_FATAL("{}", phys_ret.full_error().type.message());
  } else {
    self.vkbphysical_device = phys_ret.value();
    self.device_name = phys_ret.value().name;
  }

  self.physical_device = self.vkbphysical_device.physical_device;

  const auto vk10_supported = query_device_features(self.vkb_instance, self.physical_device);
  if (!vk10_supported.shaderStorageImageExtendedFormats) {
    OX_LOG_FATAL(
      "The selected device does not support shaderStorageImageExtendedFormats, which is required by the renderer."
    );
  }

  constexpr auto sampled_color = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
  constexpr auto sampled_storage = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
  constexpr auto sampled_storage_color = sampled_storage | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
  const auto require_format = [&](VkFormat format, VkFormatFeatureFlags features, std::string_view name) {
    if (!supports_optimal_format_features(self.vkb_instance, self.physical_device, format, features)) {
      OX_LOG_FATAL("The selected device does not support the required sampled/color/storage usage for {}.", name);
    }
  };
  require_format(VK_FORMAT_R8G8B8A8_SNORM, sampled_color, "R8G8B8A8_SNORM normals");
  require_format(VK_FORMAT_R8G8_UNORM, sampled_storage_color, "R8G8_UNORM material and shadow targets");
  require_format(VK_FORMAT_R8_UNORM, sampled_storage, "R8_UNORM ambient occlusion");
  require_format(
    VK_FORMAT_B10G11R11_UFLOAT_PACK32,
    sampled_storage_color,
    "B10G11R11_UFLOAT_PACK32 HDR and bloom targets"
  );

  const auto vk12_supported = query_device_feature<VkPhysicalDeviceVulkan12Features>(
    self.vkb_instance,
    self.physical_device,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES
  );
  if (!vk12_supported.drawIndirectCount) {
    OX_LOG_FATAL("The selected device does not support drawIndirectCount, which is required by the renderer.");
  }
  VkPhysicalDeviceMeshShaderFeaturesEXT mesh_shader_features = {};
  mesh_shader_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
  if (self.vkbphysical_device.is_extension_present(VK_EXT_MESH_SHADER_EXTENSION_NAME)) {
    const auto supported = query_device_feature<VkPhysicalDeviceMeshShaderFeaturesEXT>(
      self.vkb_instance,
      self.physical_device,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT
    );
    if (supported.meshShader && supported.taskShader) {
      self.vkbphysical_device.enable_extension_if_present(VK_EXT_MESH_SHADER_EXTENSION_NAME);
      mesh_shader_features.meshShader = true;
      mesh_shader_features.taskShader = true;
      self.features |= RenderContext::Feature::MeshShaders;
    } else {
      OX_LOG_WARN(
        "{} is present but meshShader({}) and taskShader({}) are not both supported; mesh shading is disabled.",
        VK_EXT_MESH_SHADER_EXTENSION_NAME,
        static_cast<bool>(supported.meshShader),
        static_cast<bool>(supported.taskShader)
      );
    }
  }

  VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration_structure_features = {};
  acceleration_structure_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
  VkPhysicalDeviceRayQueryFeaturesKHR ray_query_features = {};
  ray_query_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
  VkPhysicalDeviceRayTracingPipelineFeaturesKHR ray_tracing_pipeline_features = {};
  ray_tracing_pipeline_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
  if (
    self.vkbphysical_device.is_extension_present(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
    self.vkbphysical_device.is_extension_present(VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
    self.vkbphysical_device.is_extension_present(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME)
  ) {
    const auto as_supported = query_device_feature<VkPhysicalDeviceAccelerationStructureFeaturesKHR>(
      self.vkb_instance,
      self.physical_device,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR
    );
    const auto ray_query_supported = query_device_feature<VkPhysicalDeviceRayQueryFeaturesKHR>(
      self.vkb_instance,
      self.physical_device,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR
    );
    if (as_supported.accelerationStructure && ray_query_supported.rayQuery) {
      self.vkbphysical_device.enable_extension_if_present(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
      self.vkbphysical_device.enable_extension_if_present(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
      self.vkbphysical_device.enable_extension_if_present(VK_KHR_RAY_QUERY_EXTENSION_NAME);
      acceleration_structure_features.accelerationStructure = true;
      ray_query_features.rayQuery = true;
      self.features |= RenderContext::Feature::RayTracing;

      if (self.vkbphysical_device.is_extension_present(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME)) {
        const auto pipeline_supported = query_device_feature<VkPhysicalDeviceRayTracingPipelineFeaturesKHR>(
          self.vkb_instance,
          self.physical_device,
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR
        );
        if (pipeline_supported.rayTracingPipeline) {
          self.vkbphysical_device.enable_extension_if_present(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
          ray_tracing_pipeline_features.rayTracingPipeline = true;
          self.features |= RenderContext::Feature::RayTracingPipeline;
        }
      }
    } else {
      OX_LOG_WARN(
        "{} and {} are present but accelerationStructure({}) and rayQuery({}) are not both supported; ray "
        "tracing is disabled.",
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_RAY_QUERY_EXTENSION_NAME,
        static_cast<bool>(as_supported.accelerationStructure),
        static_cast<bool>(ray_query_supported.rayQuery)
      );
    }
  }

  vkb::DeviceBuilder device_builder{self.vkbphysical_device};

  VkPhysicalDeviceFeatures2 vk10_features{};
  vk10_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  vk10_features.features.shaderInt64 = true;
  vk10_features.features.vertexPipelineStoresAndAtomics = true;
  vk10_features.features.depthClamp = true;
#ifndef OX_PLATFORM_MACOSX
  vk10_features.features.fillModeNonSolid = true;
#endif
  vk10_features.features.multiViewport = true;
  vk10_features.features.samplerAnisotropy = true;
  vk10_features.features.multiDrawIndirect = true;
  vk10_features.features.fragmentStoresAndAtomics = true;
  vk10_features.features.shaderImageGatherExtended = true;
  vk10_features.features.shaderStorageImageExtendedFormats = true;
  vk10_features.features.shaderInt16 = true;
  vk10_features.features.tessellationShader = true;

  VkPhysicalDeviceVulkan11Features vk11_features{};
  vk11_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
  vk11_features.shaderDrawParameters = true;
  vk11_features.variablePointers = true;
  vk11_features.variablePointersStorageBuffer = true;
  vk11_features.uniformAndStorageBuffer16BitAccess = true;
  vk11_features.storageBuffer16BitAccess = true;
  vk11_features.storagePushConstant16 = true;

  VkPhysicalDeviceVulkan12Features vk12_features{};
  vk12_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  vk12_features.drawIndirectCount = true;
  vk12_features.descriptorIndexing = true;
  vk12_features.shaderOutputLayer = true;
  vk12_features.shaderSampledImageArrayNonUniformIndexing = true;
  vk12_features.shaderStorageBufferArrayNonUniformIndexing = true;
  vk12_features.descriptorBindingSampledImageUpdateAfterBind = true;
  vk12_features.descriptorBindingStorageImageUpdateAfterBind = true;
  vk12_features.descriptorBindingStorageBufferUpdateAfterBind = true;
  vk12_features.descriptorBindingUpdateUnusedWhilePending = true;
  vk12_features.descriptorBindingPartiallyBound = true;
  vk12_features.descriptorBindingVariableDescriptorCount = true;
  vk12_features.runtimeDescriptorArray = true;
  vk12_features.timelineSemaphore = true;
  vk12_features.bufferDeviceAddress = true;
  vk12_features.hostQueryReset = true;
  vk12_features.uniformAndStorageBuffer8BitAccess = true;
  // Shader features
  vk12_features.vulkanMemoryModel = true;
  vk12_features.storageBuffer8BitAccess = true;
  vk12_features.scalarBlockLayout = true;
  vk12_features.shaderInt8 = true;
  vk12_features.vulkanMemoryModelDeviceScope = true;
  vk12_features.shaderSubgroupExtendedTypes = true;
#ifndef OX_PLATFORM_MACOSX
  vk12_features.samplerFilterMinmax = true;
#endif

  VkPhysicalDeviceVulkan13Features vk13_features = {};
  vk13_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  vk13_features.synchronization2 = true;
  vk13_features.shaderDemoteToHelperInvocation = true;

  VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT image_atomic_int64_features = {};
  image_atomic_int64_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT;
  image_atomic_int64_features.shaderImageInt64Atomics = true;

  VkPhysicalDeviceVulkan14Features vk14_features = {};
  vk14_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;
  vk14_features.pushDescriptor = true;
  device_builder //
    .add_pNext(&vk14_features)
    .add_pNext(&vk13_features)
    .add_pNext(&vk12_features)
    .add_pNext(&vk11_features)
#ifndef OX_PLATFORM_MACOSX
    .add_pNext(&image_atomic_int64_features)
#endif
    .add_pNext(&vk10_features);

  if (self.features & RenderContext::Feature::MeshShaders) {
    device_builder.add_pNext(&mesh_shader_features);
  }

  if (self.features & RenderContext::Feature::RayTracing) {
    device_builder.add_pNext(&acceleration_structure_features);
    device_builder.add_pNext(&ray_query_features);
  }

  if (self.features & RenderContext::Feature::RayTracingPipeline) {
    device_builder.add_pNext(&ray_tracing_pipeline_features);
  }

  auto dev_ret = device_builder.build();
  if (!dev_ret) {
    OX_LOG_ERROR("Couldn't create device");
  }

  OX_LOG_INFO(
    "Device '{}' features: mesh shaders({}) ray tracing({}) ray tracing pipeline({})",
    self.device_name,
    self.features & RenderContext::Feature::MeshShaders,
    self.features & RenderContext::Feature::RayTracing,
    self.features & RenderContext::Feature::RayTracingPipeline
  );

  self.vkb_device = dev_ret.value();
  self.device = self.vkb_device.device;
  vuk::FunctionPointers fps;
  fps.vkGetInstanceProcAddr = self.vkb_instance.fp_vkGetInstanceProcAddr;
  fps.vkGetDeviceProcAddr = self.vkb_instance.fp_vkGetDeviceProcAddr;
  fps.load_pfns(instance, self.device, true);
  vkCreateDescriptorPool = fps.vkCreateDescriptorPool;
  vkCreateDescriptorSetLayout = fps.vkCreateDescriptorSetLayout;
  vkAllocateDescriptorSets = fps.vkAllocateDescriptorSets;
  vkUpdateDescriptorSets = fps.vkUpdateDescriptorSets;
  if (self.features & RenderContext::Feature::RayTracing) {
    vkGetAccelerationStructureDeviceAddressKHR = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
      self.vkb_instance.fp_vkGetDeviceProcAddr(self.device, "vkGetAccelerationStructureDeviceAddressKHR")
    );
    vkCmdWriteAccelerationStructuresPropertiesKHR = reinterpret_cast<PFN_vkCmdWriteAccelerationStructuresPropertiesKHR>(
      self.vkb_instance.fp_vkGetDeviceProcAddr(self.device, "vkCmdWriteAccelerationStructuresPropertiesKHR")
    );
    vkCmdCopyAccelerationStructureKHR = reinterpret_cast<PFN_vkCmdCopyAccelerationStructureKHR>(
      self.vkb_instance.fp_vkGetDeviceProcAddr(self.device, "vkCmdCopyAccelerationStructureKHR")
    );
  }

  std::vector<std::unique_ptr<vuk::Executor>> executors;

  auto graphics_queue_result = self.vkb_device.get_queue(vkb::QueueType::graphics);
  if (!graphics_queue_result) {
    OX_LOG_FATAL("Failed creating graphics queue. Error: {}", graphics_queue_result.error().message());
  }
  self.graphics_queue = graphics_queue_result.value();
  u32 graphics_queue_family_index = self.vkb_device.get_queue_index(vkb::QueueType::graphics).value();
  executors.push_back(create_vkqueue_executor(
    fps,
    self.device,
    self.graphics_queue,
    graphics_queue_family_index,
    vuk::DomainFlagBits::eGraphicsQueue
  ));
#if !defined(OX_USE_LLVMPIPE) && !defined(OX_PLATFORM_MACOSX)
  auto transfer_queue_result = self.vkb_device.get_queue(vkb::QueueType::transfer);
  if (!transfer_queue_result) {
    OX_LOG_ERROR("Failed creating transfer queue. Error: {}", transfer_queue_result.error().message());
  }
  self.transfer_queue = transfer_queue_result.value();
  auto transfer_queue_family_index_result = self.vkb_device.get_queue_index(vkb::QueueType::transfer);
  if (!transfer_queue_family_index_result) {
    OX_LOG_FATAL(
      "Failed getting transfer queue family index. Error: {}",
      transfer_queue_family_index_result.error().message()
    );
  }
  auto transfer_queue_family_index = transfer_queue_family_index_result.value();
  executors.push_back(create_vkqueue_executor(
    fps,
    self.device,
    self.transfer_queue,
    transfer_queue_family_index,
    vuk::DomainFlagBits::eTransferQueue
  ));
#endif
  executors.push_back(std::make_unique<vuk::ThisThreadExecutor>());

  self.runtime.emplace(
    vuk::RuntimeCreateParameters{instance, self.device, self.physical_device, std::move(executors), fps}
  );

  self.set_vsync(static_cast<bool>(self.context_cvar.cvar_vsync.get()));

  self.superframe_resource.emplace(*self.runtime, self.num_inflight_frames);
  self.superframe_allocator.emplace(*self.superframe_resource);

  auto& frame_resource = self.superframe_resource->get_next_frame();
  self.frame_allocator.emplace(frame_resource);

  self.runtime->set_shader_target_version(VK_API_VERSION_1_3);

  self.tracy_profiler = std::make_shared<TracyProfiler>();
  self.tracy_profiler->init_for_vulkan(&self);

  u32 instanceVersion = VK_API_VERSION_1_0;
  auto FN_vkEnumerateInstanceVersion = PFN_vkEnumerateInstanceVersion(
    fps.vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion")
  );
  if (FN_vkEnumerateInstanceVersion) {
    FN_vkEnumerateInstanceVersion(&instanceVersion);
  }

  // Initialize resource descriptors
  constexpr auto MAX_DESCRIPTORS = 1024_sz; // TODO: Change this to devicelimits
  VkDescriptorSetLayoutBinding bindless_set_info[] = {
    // Samplers
    {.binding = DescriptorTable_SamplerIndex,
     .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
     .descriptorCount = MAX_DESCRIPTORS,
     .stageFlags = VK_SHADER_STAGE_ALL,
     .pImmutableSamplers = nullptr},
    // Sampled Images
    {.binding = DescriptorTable_SampledImageIndex,
     .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
     .descriptorCount = MAX_DESCRIPTORS,
     .stageFlags = VK_SHADER_STAGE_ALL,
     .pImmutableSamplers = nullptr},
    // Storage Images
    {.binding = DescriptorTable_StorageImageIndex,
     .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
     .descriptorCount = MAX_DESCRIPTORS,
     .stageFlags = VK_SHADER_STAGE_ALL,
     .pImmutableSamplers = nullptr},
  };

  constexpr static auto bindless_flags = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                         VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
  VkDescriptorBindingFlags bindless_set_binding_flags[] = {
    bindless_flags,
    bindless_flags,
    bindless_flags,
  };
  self.resources.descriptor_set = self
                                    .create_persistent_descriptor_set(1, bindless_set_info, bindless_set_binding_flags);

  auto& event_system = App::get_event_system();
  auto sub_result = event_system.subscribe<WindowResizeEvent>([&self](const WindowResizeEvent& e) {
    self.handle_resize(e.width, e.height);
  });
  if (!sub_result.has_value()) {
    OX_LOG_ERROR("Failed to subscribe for WindowResizeEvent!");
  }

  const u32 major = VK_VERSION_MAJOR(instanceVersion);
  const u32 minor = VK_VERSION_MINOR(instanceVersion);
  const u32 patch = VK_VERSION_PATCH(instanceVersion);

  OX_LOG_INFO(
    "Vulkan context initialized using device: {} with Vulkan Version: {}.{}.{}",
    self.device_name,
    major,
    minor,
    patch
  );
}

auto RenderContext::destroy_context(this RenderContext& self) -> void {
  ZoneScoped;
  self.runtime->wait_idle();

  auto destroy_resource_pool = [&self](auto& pool) -> void {
    for (auto i = 0_sz; i < pool.size(); i++) {
      auto* v = pool.slot_from_index(i);
      if (v) {
        self.superframe_allocator->deallocate({v, 1});
      }
    }

    pool.reset();
  };

  destroy_resource_pool(self.resources.buffers);
  destroy_resource_pool(self.resources.images);
  destroy_resource_pool(self.resources.image_views);
  self.resources.samplers.reset();
  self.resources.pipelines.reset();

  self.superframe_allocator->deallocate(std::span(&self.resources.descriptor_set, 1));
  self.resources.descriptor_set = {};

  self.tracy_profiler.reset();
  self.swapchain.reset();
  self.frame_allocator.reset();
  self.superframe_allocator.reset();
  self.superframe_resource.reset();
  self.runtime.reset();

  vkb::destroy_surface(self.vkb_instance, self.surface);
  self.surface = VK_NULL_HANDLE;
  vkb::destroy_device(self.vkb_device);
  self.device = VK_NULL_HANDLE;
  vkb::destroy_instance(self.vkb_instance);
}

auto RenderContext::handle_resize(u32 width, u32 height) -> void {
  wait();

  swapchain = make_swapchain(
    *runtime,
    *superframe_allocator,
    vkb_device,
    surface,
    std::move(swapchain),
    present_mode,
    num_inflight_frames
  );
}

auto RenderContext::set_vsync(bool enable) -> void {
  const auto set_present_mode = enable ? vuk::PresentModeKHR::eFifo : vuk::PresentModeKHR::eImmediate;
  present_mode = set_present_mode;
}

auto RenderContext::is_vsync() const -> bool { return present_mode == vuk::PresentModeKHR::eFifo; }

auto RenderContext::new_frame(this RenderContext& self) -> vuk::Value<vuk::ImageAttachment> {
  ZoneScoped;

  auto vsync_cvar_enabled = static_cast<bool>(self.context_cvar.cvar_vsync.get());
  auto wanted_vsync = vsync_cvar_enabled ? vuk::PresentModeKHR::eFifo : vuk::PresentModeKHR::eImmediate;
  auto present_mode_changed = wanted_vsync != self.present_mode;
  if (present_mode_changed) {
    self.present_mode = wanted_vsync;
    self.handle_resize(1, 1);
  }

  {
    auto write_lock = std::unique_lock(self.frame_allocator_mutex);
    auto queue_lock = std::unique_lock(self.queue_mutex);
    if (self.frame_allocator) {
      self.frame_allocator.reset();
    }

    auto& frame_resource = self.superframe_resource->get_next_frame();
    self.frame_allocator.emplace(frame_resource);
  }
  self.runtime->next_frame();

  if (!self.swapchain.has_value()) {
    self.swapchain = make_swapchain(
      *self.runtime,
      *self.superframe_allocator,
      self.vkb_device,
      self.surface,
      {},
      self.present_mode,
      self.num_inflight_frames
    );
  }

  auto acquired_swapchain = vuk::acquire_swapchain(*self.swapchain);
  auto acquired_image = vuk::acquire_next_image("present_image", std::move(acquired_swapchain));

  self.swapchain_extent = glm::vec2(acquired_image->extent.width, acquired_image->extent.height);

  return acquired_image;
}

auto RenderContext::end_frame(this RenderContext& self, vuk::Value<vuk::ImageAttachment> target_) -> void {
  ZoneScoped;

  auto entire_thing = vuk::enqueue_presentation(std::move(target_));
  vuk::ProfilingCallbacks cbs = self.tracy_profiler->setup_vuk_callback();
  try {
    auto queue_lock = std::unique_lock(self.queue_mutex);
    entire_thing.submit(*self.frame_allocator, self.compiler, {.graph_label = {}, .callbacks = cbs});
  } catch (vuk::VkException& exception) {
    // Actions such as minimizing the window will report a VK_ERROR_OUT_OF_DATE_KHR error,
    // so we don't wanna fatal error out on those.
    if (exception.code() != VK_ERROR_OUT_OF_DATE_KHR) {
      OX_LOG_FATAL("{}", exception.what());
    }
  }

  self.current_frame = (self.current_frame + 1) % self.num_inflight_frames;
  self.num_frames = self.runtime->get_frame_count();
}

auto RenderContext::wait(this RenderContext& self) -> void {
  ZoneScoped;

  OX_LOG_INFO("Device wait idle triggered!");
  self.runtime->wait_idle();
}

auto RenderContext::wait_on(vuk::UntypedValue&& fut) -> void {
  ZoneScoped;

  auto queue_lock = std::unique_lock(queue_mutex);
  fut.wait(superframe_allocator.value(), this_thread_compiler);
}

auto RenderContext::submit_now(vuk::UntypedValue&& fut) -> void {
  ZoneScoped;

  auto read_lock = std::shared_lock(frame_allocator_mutex);
  auto queue_lock = std::unique_lock(queue_mutex);
  fut.submit(frame_allocator.value(), this_thread_compiler);
}

auto RenderContext::get_compiler(this RenderContext&) -> vuk::Compiler& { return this_thread_compiler; }

auto RenderContext::wait_on_multiple(std::span<vuk::UntypedValue> values) -> void {
  ZoneScoped;

  auto queue_lock = std::unique_lock(queue_mutex);
  vuk::wait_for_values_explicit(superframe_allocator.value(), this_thread_compiler, values);
}

auto RenderContext::submit_multiple(std::span<vuk::UntypedValue> values) -> void {
  ZoneScoped;

  auto queue_lock = std::unique_lock(queue_mutex);
  vuk::submit(superframe_allocator.value(), this_thread_compiler, values, {});
}

auto RenderContext::create_persistent_descriptor_set(
  this RenderContext& self,
  u32 set_index,
  std::span<VkDescriptorSetLayoutBinding> bindings,
  std::span<VkDescriptorBindingFlags> binding_flags
) -> vuk::PersistentDescriptorSet {
  ZoneScoped;

  OX_CHECK_EQ(bindings.size(), binding_flags.size());

  auto descriptor_sizes = std::vector<VkDescriptorPoolSize>();
  for (const auto& binding : bindings) {
    OX_CHECK_LT(binding.descriptorType, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT);
    descriptor_sizes.emplace_back(binding.descriptorType, binding.descriptorCount);
  }

  auto pool_flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
  auto pool_info = VkDescriptorPoolCreateInfo{
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
    .pNext = nullptr,
    .flags = static_cast<VkDescriptorPoolCreateFlags>(pool_flags),
    .maxSets = 1,
    .poolSizeCount = static_cast<u32>(descriptor_sizes.size()),
    .pPoolSizes = descriptor_sizes.data(),
  };
  auto pool = VkDescriptorPool{};
  vkCreateDescriptorPool(self.device, &pool_info, nullptr, &pool);

  auto set_layout_binding_flags_info = VkDescriptorSetLayoutBindingFlagsCreateInfo{
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
    .pNext = nullptr,
    .bindingCount = static_cast<u32>(binding_flags.size()),
    .pBindingFlags = binding_flags.data(),
  };

  auto set_layout_info = VkDescriptorSetLayoutCreateInfo{
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
    .pNext = &set_layout_binding_flags_info,
    .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
    .bindingCount = static_cast<u32>(bindings.size()),
    .pBindings = bindings.data(),
  };
  auto set_layout = VkDescriptorSetLayout{};
  vkCreateDescriptorSetLayout(self.device, &set_layout_info, nullptr, &set_layout);

  auto set_alloc_info = VkDescriptorSetAllocateInfo{
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
    .pNext = nullptr,
    .descriptorPool = pool,
    .descriptorSetCount = 1,
    .pSetLayouts = &set_layout,
  };
  auto descriptor_set = VkDescriptorSet{};
  vkAllocateDescriptorSets(self.device, &set_alloc_info, &descriptor_set);

  auto persistent_set_create_info = vuk::DescriptorSetLayoutCreateInfo{
    .dslci = set_layout_info,
    .index = set_index,
    .bindings = std::vector(bindings.begin(), bindings.end()),
    .flags = std::vector(binding_flags.begin(), binding_flags.end()),
  };
  return vuk::PersistentDescriptorSet{
    .backing_pool = pool,
    .set_layout_create_info = persistent_set_create_info,
    .set_layout = set_layout,
    .backing_set = descriptor_set,
    .wdss = {},
    .descriptor_bindings = {},
  };
}

auto RenderContext::commit_descriptor_set(this RenderContext& self, std::span<VkWriteDescriptorSet> writes) -> void {
  ZoneScoped;

  auto write_lock = std::unique_lock(self.descriptor_mutex);
  vkUpdateDescriptorSets(self.device, static_cast<u32>(writes.size()), writes.data(), 0, nullptr);
}

auto RenderContext::create_pipeline(this RenderContext& self, const ShaderPipelineData& pipeline_data) -> bool {
  ZoneScoped;

  if (const auto missing = missing_shader_feature(self.features, pipeline_data.required_features); !missing.empty()) {
    OX_LOG_INFO("Skipped pipeline named {}, device has no {} support.", pipeline_data.module_name, missing);
    return true;
  }

  auto pipeline_ci = vuk::PipelineBaseCreateInfo{};
  if (pipeline_data.required_features & ShaderFeatureFlag::Bindless) {
    const auto& bindless_set = self.resources.descriptor_set;
    const auto& set_layout_create_info = bindless_set.set_layout_create_info;
    pipeline_ci.explicit_set_layouts.emplace_back(set_layout_create_info);

    for (const auto& [binding, binding_flags] :
         std::views::zip(set_layout_create_info.bindings, set_layout_create_info.flags)) {
      pipeline_ci.set_binding_flags(
        static_cast<u32>(set_layout_create_info.index),
        binding.binding,
        static_cast<vuk::DescriptorBindingFlagBits>(binding_flags)
      );
    }
  }

  const auto ray_tracing = std::ranges::any_of(pipeline_data.entry_points, [](const auto& entry_point) {
    return entry_point.shader_stage == ShaderStage::RayGeneration;
  });

  if (ray_tracing) {
    auto ordered = ankerl::svector<const ShaderEntryPointData*, 8>{};
    for (const auto& entry_point : pipeline_data.entry_points) {
      ordered.emplace_back(&entry_point);
    }
    std::ranges::stable_sort(ordered, {}, [](const ShaderEntryPointData* entry_point) {
      return ray_tracing_stage_order(entry_point->shader_stage);
    });

    auto hit_group = vuk::HitGroup{.type = vuk::HitGroupType::eTriangles};
    for (u32 index = 0; index < static_cast<u32>(ordered.size()); index++) {
      const auto& entry_point = *ordered[index];
      pipeline_ci.add_spirv(entry_point.spirv, pipeline_data.module_name, entry_point.name);

      switch (entry_point.shader_stage) {
        case ShaderStage::ClosestHit  : hit_group.closest_hit = index; break;
        case ShaderStage::AnyHit      : hit_group.any_hit = index; break;
        case ShaderStage::Intersection: hit_group.intersection = index; break;
        default                       : break;
      }
    }

    if (hit_group.intersection != VK_SHADER_UNUSED_KHR) {
      hit_group.type = vuk::HitGroupType::eProcedural;
    }
    if (
      hit_group.closest_hit != VK_SHADER_UNUSED_KHR || hit_group.any_hit != VK_SHADER_UNUSED_KHR ||
      hit_group.intersection != VK_SHADER_UNUSED_KHR
    ) {
      pipeline_ci.add_hit_group(hit_group);
    }
  } else {
    for (const auto& entry_point : pipeline_data.entry_points) {
      pipeline_ci.add_spirv(entry_point.spirv, pipeline_data.module_name, entry_point.name);
    }
  }

  self.runtime->create_named_pipeline(pipeline_data.module_name.c_str(), pipeline_ci);

  OX_LOG_INFO("Created pipeline named {}.", pipeline_data.module_name);

  return true;
}

auto RenderContext::use_mesh_shaders(this const RenderContext& self) -> bool {
  return (self.features & RenderContext::Feature::MeshShaders) && self.context_cvar.cvar_mesh_shaders.as_bool();
}

auto RenderContext::use_ray_tracing(this const RenderContext& self) -> bool {
  return (self.features & RenderContext::Feature::RayTracing) && self.context_cvar.cvar_ray_tracing.as_bool() &&
         vkGetAccelerationStructureDeviceAddressKHR != nullptr;
}

auto RenderContext::use_ray_tracing_pipeline(this const RenderContext& self) -> bool {
  return self.use_ray_tracing() && (self.features & RenderContext::Feature::RayTracingPipeline);
}

auto RenderContext::as_scratch_alignment(this const RenderContext& self) -> u64 {
  return self.runtime->as_properties.minAccelerationStructureScratchOffsetAlignment;
}

auto RenderContext::allocate_image(const vuk::ImageAttachment& image_attachment) -> ImageID {
  ZoneScoped;

  vuk::ImageCreateInfo ici;
  ici.format = vuk::Format(image_attachment.format);
  ici.imageType = image_attachment.image_type;
  ici.flags = image_attachment.image_flags;
  ici.arrayLayers = image_attachment.layer_count;
  ici.samples = image_attachment.sample_count.count;
  ici.tiling = image_attachment.tiling;
  ici.mipLevels = image_attachment.level_count;
  ici.usage = image_attachment.usage;
  ici.extent = image_attachment.extent;

  VkImageFormatListCreateInfo listci = {};
  listci.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
  VkFormat formats[2];
  if (image_attachment.allow_srgb_unorm_mutable) {
    auto unorm_fmt = srgb_to_unorm(image_attachment.format);
    auto srgb_fmt = unorm_to_srgb(image_attachment.format);
    formats[0] = (VkFormat)image_attachment.format;
    formats[1] = unorm_fmt == vuk::Format::eUndefined ? (VkFormat)srgb_fmt : (VkFormat)unorm_fmt;
    listci.pViewFormats = formats;
    listci.viewFormatCount = formats[1] == VK_FORMAT_UNDEFINED ? 1 : 2;
    if (listci.viewFormatCount > 1) {
      ici.flags |= vuk::ImageCreateFlagBits::eMutableFormat;
      ici.pNext = &listci;
    }
  }

  vuk::Image image = {};

  if (auto res = superframe_allocator->allocate_images(std::span{&image, 1}, std::span{&ici, 1}); !res) {
    OX_LOG_ERROR("{}", res.error().error_message);
  }

  return resources.images.create_slot(std::move(image));
}

auto RenderContext::destroy_image(const ImageID id) -> void {
  ZoneScoped;

  auto image = resources.images.copy_slot(id);
  if (!image) {
    return;
  }

  superframe_allocator->deallocate({&image.value(), 1});
  resources.images.destroy_slot(id);
}

auto RenderContext::image(const ImageID id) -> vuk::Image {
  ZoneScoped;

  return resources.images.copy_slot(id).value_or(vuk::Image{});
}

auto RenderContext::allocate_image_view(const vuk::ImageAttachment& image_attachment, UploadBatch* batch)
  -> ImageViewID {
  ZoneScoped;

  vuk::ImageViewCreateInfo ivci;
  OX_CHECK_EQ((bool)image_attachment.image, true);
  ivci.flags = image_attachment.image_view_flags;
  ivci.image = image_attachment.image.image;
  ivci.viewType = image_attachment.view_type;
  ivci.format = vuk::Format(image_attachment.format);
  ivci.components = image_attachment.components;
  ivci.view_usage = image_attachment.usage;

  vuk::ImageSubresourceRange& isr = ivci.subresourceRange;
  isr.aspectMask = format_to_aspect(ivci.format);
  isr.baseArrayLayer = image_attachment.base_layer;
  isr.layerCount = image_attachment.layer_count;
  isr.baseMipLevel = image_attachment.base_level;
  isr.levelCount = image_attachment.level_count;

  vuk::ImageView view;

  if (auto res = superframe_allocator->allocate_image_views(std::span{&view, 1}, std::span{&ivci, 1}); !res) {
    OX_LOG_ERROR("{}", res.error().error_message);
  }

  auto* image_view_handle = view.payload;
  auto image_view_id = resources.image_views.create_slot(std::move(view));

  auto& bindless_set = get_descriptor_set();

  auto sampled_image_descriptor = VkDescriptorImageInfo{
    .sampler = nullptr,
    .imageView = image_view_handle,
    .imageLayout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
  };

  auto storage_image_descriptor = VkDescriptorImageInfo{
    .sampler = nullptr,
    .imageView = image_view_handle,
    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };

  const auto array_element = SlotMap_decode_id(image_view_id).index;
  if (batch) {
    if (image_attachment.usage & vuk::ImageUsageFlagBits::eSampled) {
      batch->add_descriptor_write({
        .binding = DescriptorTable_SampledImageIndex,
        .array_element = array_element,
        .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        .image_info = sampled_image_descriptor,
      });
    }
    if (image_attachment.usage & vuk::ImageUsageFlagBits::eStorage) {
      batch->add_descriptor_write({
        .binding = DescriptorTable_StorageImageIndex,
        .array_element = array_element,
        .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .image_info = storage_image_descriptor,
      });
    }

    return image_view_id;
  }

  auto descriptor_count = 0_sz;
  auto descriptor_writes = std::array<VkWriteDescriptorSet, 2>();
  if (image_attachment.usage & vuk::ImageUsageFlagBits::eSampled) {
    descriptor_writes[descriptor_count++] = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .pNext = nullptr,
      .dstSet = bindless_set.backing_set,
      .dstBinding = DescriptorTable_SampledImageIndex,
      .dstArrayElement = SlotMap_decode_id(image_view_id).index,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
      .pImageInfo = &sampled_image_descriptor,
      .pBufferInfo = nullptr,
      .pTexelBufferView = nullptr,
    };
  }
  if (image_attachment.usage & vuk::ImageUsageFlagBits::eStorage) {
    descriptor_writes[descriptor_count++] = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .pNext = nullptr,
      .dstSet = bindless_set.backing_set,
      .dstBinding = DescriptorTable_StorageImageIndex,
      .dstArrayElement = SlotMap_decode_id(image_view_id).index,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
      .pImageInfo = &storage_image_descriptor,
      .pBufferInfo = nullptr,
      .pTexelBufferView = nullptr,
    };
  }
  commit_descriptor_set({descriptor_writes.data(), descriptor_count});

  return image_view_id;
}

auto RenderContext::destroy_image_view(const ImageViewID id) -> void {
  ZoneScoped;

  auto view = resources.image_views.copy_slot(id);
  if (!view) {
    return;
  }

  superframe_allocator->deallocate({&view.value(), 1});
  resources.image_views.destroy_slot(id);
}

auto RenderContext::image_view(const ImageViewID id) -> vuk::ImageView {
  ZoneScoped;

  return resources.image_views.copy_slot(id).value_or(vuk::ImageView{});
}

auto RenderContext::allocate_sampler(const vuk::SamplerCreateInfo& sampler_info, UploadBatch* batch) -> SamplerID {
  ZoneScoped;

  auto sampler = runtime->acquire_sampler(sampler_info, num_frames);
  auto sampler_handle = sampler.payload;
  auto sampler_id = resources.samplers.create_slot(std::move(sampler));

  auto sampler_descriptor = VkDescriptorImageInfo{
    .sampler = sampler_handle,
    .imageView = nullptr,
    .imageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };

  if (batch) {
    batch->add_descriptor_write({
      .binding = DescriptorTable_SamplerIndex,
      .array_element = SlotMap_decode_id(sampler_id).index,
      .type = VK_DESCRIPTOR_TYPE_SAMPLER,
      .image_info = sampler_descriptor,
    });

    return sampler_id;
  }

  auto& bindless_set = get_descriptor_set();
  auto descriptor_write = VkWriteDescriptorSet{
    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, //
    .pNext = nullptr,
    .dstSet = bindless_set.backing_set,
    .dstBinding = DescriptorTable_SamplerIndex,
    .dstArrayElement = SlotMap_decode_id(sampler_id).index,
    .descriptorCount = 1,
    .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
    .pImageInfo = &sampler_descriptor,
    .pBufferInfo = nullptr,
    .pTexelBufferView = nullptr,
  };
  commit_descriptor_set({&descriptor_write, 1});

  return sampler_id;
}

auto RenderContext::destroy_sampler(const SamplerID id) -> void {
  ZoneScoped;

  resources.samplers.destroy_slot(id);
}

auto RenderContext::sampler(const SamplerID id) -> vuk::Sampler {
  ZoneScoped;

  return resources.samplers.copy_slot(id).value_or(vuk::Sampler{});
}

auto RenderContext::get_accel_structure_device_address(
  this const RenderContext& self, VkAccelerationStructureKHR handle
) -> u64 {
  ZoneScoped;

  auto address_info = VkAccelerationStructureDeviceAddressInfoKHR{
    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
    .pNext = nullptr,
    .accelerationStructure = handle,
  };

  return vkGetAccelerationStructureDeviceAddressKHR(self.device, &address_info);
}

auto RenderContext::cmd_write_accel_structure_compacted_size(
  this const RenderContext& self,
  VkCommandBuffer command_buffer,
  VkAccelerationStructureKHR handle,
  VkQueryPool query_pool,
  u32 query_index
) -> void {
  ZoneScoped;

  if (!vkCmdWriteAccelerationStructuresPropertiesKHR) {
    return;
  }

  vkCmdWriteAccelerationStructuresPropertiesKHR(
    command_buffer,
    1,
    &handle,
    VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
    query_pool,
    query_index
  );
}

auto RenderContext::cmd_copy_accel_structure(
  this const RenderContext& self, VkCommandBuffer command_buffer, const VkCopyAccelerationStructureInfoKHR& info
) -> void {
  ZoneScoped;

  if (!vkCmdCopyAccelerationStructureKHR) {
    return;
  }

  vkCmdCopyAccelerationStructureKHR(command_buffer, &info);
}

auto RenderContext::create_query_pool(this const RenderContext& self, VkQueryType type, u32 count) -> VkQueryPool {
  ZoneScoped;

  const auto pool_info = VkQueryPoolCreateInfo{
    .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
    .pNext = nullptr,
    .flags = 0,
    .queryType = type,
    .queryCount = count,
    .pipelineStatistics = 0,
  };

  auto query_pool = VkQueryPool{VK_NULL_HANDLE};
  if (self.runtime->vkCreateQueryPool(self.device, &pool_info, nullptr, &query_pool) != VK_SUCCESS) {
    OX_LOG_ERROR("Failed to create query pool.");
    return VK_NULL_HANDLE;
  }

  self.runtime->vkResetQueryPool(self.device, query_pool, 0, count);

  return query_pool;
}

auto RenderContext::destroy_query_pool(this const RenderContext& self, VkQueryPool query_pool) -> void {
  ZoneScoped;

  if (query_pool == VK_NULL_HANDLE) {
    return;
  }

  self.runtime->vkDestroyQueryPool(self.device, query_pool, nullptr);
}

auto RenderContext::read_query_pool(this const RenderContext& self, VkQueryPool query_pool, std::span<u64> results)
  -> bool {
  ZoneScoped;

  if (query_pool == VK_NULL_HANDLE || results.empty()) {
    return false;
  }

  const auto result = self.runtime->vkGetQueryPoolResults(
    self.device,
    query_pool,
    0,
    static_cast<u32>(results.size()),
    results.size_bytes(),
    results.data(),
    sizeof(u64),
    VK_QUERY_RESULT_64_BIT
  );

  return result == VK_SUCCESS;
}

auto RenderContext::resize_buffer(vuk::Unique<vuk::Buffer>&& buffer, vuk::MemoryUsage usage, u64 new_size)
  -> vuk::Unique<vuk::Buffer> {
  if (!buffer || new_size > buffer->size) {
    const auto old_size = buffer ? buffer->size : 0_u64;
    const auto alloc_size = buffer ? std::max(new_size, buffer->size * 2_u64) : new_size;

    buffer.reset();

    OX_LOG_TRACE("resize_buffer growth {} -> {}", old_size, alloc_size);
    return allocate_buffer_super(usage, alloc_size);
  }

  return std::move(buffer);
}

auto RenderContext::allocate_buffer_super(vuk::MemoryUsage usage, u64 size, u64 alignment) -> vuk::Unique<vuk::Buffer> {
  return *vuk::allocate_buffer(
    superframe_allocator.value(),
    {.mem_usage = usage, .size = size, .alignment = alignment}
  );
}

auto RenderContext::alloc_image_buffer(vuk::Format format, vuk::Extent3D extent, vuk::source_location LOC) noexcept
  -> vuk::Unique<vuk::Buffer> {
  ZoneScoped;

  auto alignment = vuk::format_to_texel_block_size(format);
  auto size = vuk::compute_image_size(format, extent);

  return *vuk::allocate_buffer(
    superframe_allocator.value(),
    {.mem_usage = vuk::MemoryUsage::eCPUtoGPU, .size = size, .alignment = alignment},
    LOC
  );
}

auto RenderContext::alloc_transient_buffer_raw(
  vuk::MemoryUsage usage, usize size, usize alignment, vuk::source_location LOC
) -> vuk::Buffer {
  ZoneScoped;

  auto read_lock = std::shared_lock(frame_allocator_mutex);

  auto buffer = *vuk::allocate_buffer(
    frame_allocator.value(),
    {.mem_usage = usage, .size = size, .alignment = alignment},
    LOC
  );
  return *buffer;
}

auto RenderContext::alloc_transient_buffer(
  vuk::MemoryUsage usage, usize size, usize alignment, vuk::source_location LOC
) -> vuk::Value<vuk::Buffer> {
  ZoneScoped;

  auto buffer = alloc_transient_buffer_raw(usage, size, alignment, LOC);
  return vuk::acquire_buf("transient buffer", buffer, vuk::Access::eNone, LOC);
}

auto RenderContext::upload_staging(
  vuk::Value<vuk::Buffer>&& src, vuk::Value<vuk::Buffer>&& dst, vuk::source_location LOC
) -> vuk::Value<vuk::Buffer> {
  ZoneScoped;
  memory::ScopedStack stack;

  auto upload_pass = vuk::make_pass(
    stack.format_char("{}", LOC),
    [](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::Access::eTransferRead) src_ba,
      VUK_BA(vuk::Access::eTransferWrite) dst_ba
    ) {
      cmd_list.copy_buffer(src_ba, dst_ba);
      return dst_ba;
    },
    vuk::DomainFlagBits::eAny,
    LOC
  );

  return upload_pass(std::move(src), std::move(dst));
}

auto RenderContext::upload_staging(
  vuk::Value<vuk::Buffer>&& src, vuk::Buffer& dst, u64 dst_offset, vuk::source_location LOC
) -> vuk::Value<vuk::Buffer> {
  ZoneScoped;

  auto dst_buffer = vuk::discard_buf("dst", dst.subrange(dst_offset, src->size), LOC);
  return upload_staging(std::move(src), std::move(dst_buffer), LOC);
}

auto RenderContext::upload_staging(
  void* data, u64 data_size, vuk::Value<vuk::Buffer>&& dst, u64 dst_offset, vuk::source_location LOC
) -> vuk::Value<vuk::Buffer> {
  ZoneScoped;

  auto cpu_buffer = alloc_transient_buffer(vuk::MemoryUsage::eCPUonly, data_size, 8, LOC);
  std::memcpy(cpu_buffer->mapped_ptr, data, data_size);

  auto dst_buffer = vuk::discard_buf("dst", dst->subrange(dst_offset, cpu_buffer->size), LOC);
  return upload_staging(std::move(cpu_buffer), std::move(dst_buffer), LOC);
}

auto RenderContext::upload_staging(
  void* data, u64 data_size, vuk::Buffer& dst, u64 dst_offset, vuk::source_location LOC
) -> vuk::Value<vuk::Buffer> {
  ZoneScoped;

  auto cpu_buffer = alloc_transient_buffer(vuk::MemoryUsage::eCPUonly, data_size, 8, LOC);
  std::memcpy(cpu_buffer->mapped_ptr, data, data_size);

  auto dst_buffer = vuk::discard_buf("dst", dst.subrange(dst_offset, cpu_buffer->size), LOC);
  return upload_staging(std::move(cpu_buffer), std::move(dst_buffer), LOC);
}

auto RenderContext::scratch_buffer(const void* data, u64 size, usize alignment, vuk::source_location LOC)
  -> vuk::Value<vuk::Buffer> {
  ZoneScoped;

#define SCRATCH_BUFFER_USE_BAR
#ifndef SCRATCH_BUFFER_USE_BAR
  auto cpu_buffer = alloc_transient_buffer(vuk::MemoryUsage::eCPUonly, size, alignment, LOC);
  std::memcpy(cpu_buffer->mapped_ptr, data, size);
  auto gpu_buffer = alloc_transient_buffer(vuk::MemoryUsage::eGPUonly, size, alignment, LOC);

  auto upload_pass = vuk::make_pass(
    "scratch_buffer",
    [](vuk::CommandBuffer& cmd_list, VUK_BA(vuk::Access::eTransferRead) src, VUK_BA(vuk::Access::eTransferWrite) dst) {
      cmd_list.copy_buffer(src, dst);
      return dst;
    },
    vuk::DomainFlagBits::eAny,
    LOC
  );

  return upload_pass(std::move(cpu_buffer), std::move(gpu_buffer));
#else
  auto buffer = alloc_transient_buffer(vuk::MemoryUsage::eGPUtoCPU, size, alignment, LOC);
  std::memcpy(buffer->mapped_ptr, data, size);
  return buffer;
#endif
}
} // namespace ox
