#include "Render/RenderContext.hpp"

#include <VkBootstrap.h>
#include <algorithm>
#include <array>
#include <bit>
#include <ranges>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vir/Log.hpp>
#include <vir/RenderGraph.hpp>
#include <vir/Types.hpp>

#include "Asset/AssetFile.hpp"
#include "Core/App.hpp"
#include "Core/Enum.hpp"
#include "Core/Version.hpp"
#include "Memory/Stack.hpp"
#include "Render/RenderResources.hpp"
#include "Render/UploadBatch.hpp"
#include "Render/Utils/TextureFormat.hpp"
#include "Render/Window.hpp"
#include "Utils/Log.hpp"
#include "Utils/Profiler.hpp"

namespace ox {
struct RenderDevice {
  vkb::Instance instance = {};
  vkb::PhysicalDevice physical = {};
  vkb::Device device = {};
  vkb::Swapchain swapchain = {};
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  bool owns_device = false;
};

static auto vir_log_sink(vir::LogLevel level, std::string_view message, const c8* file, u32 line) -> void {
  auto verbosity = loguru::Verbosity_MAX;
  switch (level) {
    case vir::LogLevel::Debug: verbosity = loguru::Verbosity_MAX; break;
    case vir::LogLevel::Warn : verbosity = loguru::Verbosity_WARNING; break;
    case vir::LogLevel::Error: verbosity = loguru::Verbosity_ERROR; break;
    case vir::LogLevel::Fatal: verbosity = loguru::Verbosity_FATAL; break;
  }

  loguru::log(verbosity, file, line, "{}", message);
}

static auto debug_callback(
  VkDebugUtilsMessageSeverityFlagBitsEXT severity,
  VkDebugUtilsMessageTypeFlagsEXT,
  const VkDebugUtilsMessengerCallbackDataEXT* data,
  void*
) -> VkBool32 {
  if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    OX_LOG_ERROR("Vulkan [{}] {}: {}", data->messageIdNumber, data->pMessageIdName, data->pMessage);
  } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
    OX_LOG_WARN("Vulkan [{}] {}: {}", data->messageIdNumber, data->pMessageIdName, data->pMessage);
  } else {
    OX_LOG_DEBUG("Vulkan [{}] {}: {}", data->messageIdNumber, data->pMessageIdName, data->pMessage);
  }
  return VK_FALSE;
}

static auto program_acquires_swapchain(const vir::Program& program) -> bool {
  return std::ranges::any_of(program.instructions, [](const vir::Instr& instruction) -> bool {
    return std::holds_alternative<vir::ir::SwapchainImage>(instruction.ir);
  });
}

static auto destroy_retired_resource(RenderContext& render_context, const RetiredRenderResource& resource) -> void {
  std::visit(
    [&](const auto& value) -> void {
      using T = std::decay_t<decltype(value)>;
      if constexpr (std::is_same_v<T, vir::Buffer>) {
        render_context.persistent->deallocate_buffer(value);
      } else if constexpr (std::is_same_v<T, vir::Image>) {
        render_context.persistent->deallocate_image(value);
      } else if constexpr (std::is_same_v<T, vir::ImageView>) {
        render_context.persistent->deallocate_image_view(value.handle);
      } else if constexpr (std::is_same_v<T, vir::Sampler>) {
        render_context.persistent->deallocate_sampler(value.handle);
      } else {
        render_context.context->fn.vkDestroyAccelerationStructureKHR(render_context.device, value.handle, nullptr);
      }
    },
    resource.value
  );
}

template <typename T, typename ID>
static auto retire_resource(RenderContext& render_context, SlotMap<T, ID>& slots, ID id) -> void {
  auto* queue = render_context.context->queue(vir::DomainFlag::Graphics);
  auto queue_lock = std::unique_lock(queue->submit_mutex);
  auto lock = std::unique_lock(render_context.resources->retirement_mutex);
  if (std::ranges::any_of(render_context.resources->retired, [id](const RetiredRenderResource& entry) -> bool {
        const auto* pending = std::get_if<ID>(&entry.id);
        return pending && *pending == id;
      })) {
    return;
  }
  if (const auto value = slots.copy_slot(id)) {
    render_context.resources->retired.push_back({
      .value = *value,
      .id = id,
      .ticket = {render_context.context->queue_id(vir::DomainFlag::Graphics), queue->timeline_value},
    });
  }
}

static auto descriptor_capacity(const DescriptorSet& set, u32 binding) -> u32 {
  const auto it = std::ranges::find(set.bindings, binding, &VkDescriptorSetLayoutBinding::binding);
  return it == set.bindings.end() ? 0 : it->descriptorCount;
}

static auto destroy_swapchain(RenderContext& render_context, vir::SwapChain& swapchain) -> void {
  for (const auto& image : swapchain.attachments) {
    render_context.persistent->deallocate_image_view(image.image_view);
  }
  for (const auto semaphore : swapchain.semaphores) {
    render_context.persistent->deallocate_semaphore(semaphore);
  }
  swapchain = {};
}

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

template <typename T>
static auto query_device_property(const vkb::Instance& instance, VkPhysicalDevice physical_device, VkStructureType type)
  -> T {
  T property = {};
  property.sType = type;

  const auto get_properties_2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
    instance.fp_vkGetInstanceProcAddr(instance.instance, "vkGetPhysicalDeviceProperties2")
  );
  if (!get_properties_2) {
    return property;
  }

  VkPhysicalDeviceProperties2 properties_2 = {};
  properties_2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  properties_2.pNext = &property;
  get_properties_2(physical_device, &properties_2);

  return property;
}

static auto supports_optimal_format_features(
  const vkb::Instance& instance,
  VkPhysicalDevice physical_device,
  vir::Format format,
  VkFormatFeatureFlags required_features
) -> bool {
  const auto get_format_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
    instance.fp_vkGetInstanceProcAddr(instance.instance, "vkGetPhysicalDeviceFormatProperties")
  );
  if (get_format_properties == nullptr) {
    return false;
  }

  auto properties = VkFormatProperties{};
  get_format_properties(physical_device, static_cast<VkFormat>(format), &properties);
  return (properties.optimalTilingFeatures & required_features) == required_features;
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

template <typename T, typename ID>
static auto live_resource(const RenderResources& resources, const SlotMap<T, ID>& slots, ID id) -> T {
  auto lock = std::shared_lock(resources.retirement_mutex);
  if (std::ranges::any_of(resources.retired, [id](const RetiredRenderResource& entry) -> bool {
        const auto* pending = std::get_if<ID>(&entry.id);
        return pending && *pending == id;
      })) {
    return {};
  }
  return slots.copy_slot(id).value_or(T{});
}

RenderContext::RenderContext()
    : resources(std::make_unique<RenderResources>()),
      render_device(std::make_unique<RenderDevice>()) {}

RenderContext::~RenderContext() { destroy_context(); }

auto RenderContext::create_context(this RenderContext& self, const Window& window, bool vulkan_validation_layers)
  -> void {
  ZoneScoped;
  vir::set_log_sink(vir_log_sink);

  self.render_device->owns_device = true;
  const auto& build_info = engine_build_info();
  vkb::InstanceBuilder builder;
  builder //
    .set_app_name("Oxylus App")
    .set_engine_name("Oxylus")
    .set_engine_version(build_info.major, build_info.minor, build_info.patch)
    .require_api_version(1, 3, 0)
    .set_app_version(0, 1, 0);

  if (vulkan_validation_layers) {
    OX_LOG_INFO("Enabled vulkan validation layers.");
    builder.request_validation_layers();
  }

  // whatever the video driver needs, including VK_EXT_headless_surface under SDL's offscreen driver
  for (const auto* extension : window.get_vulkan_instance_extensions()) {
    builder.enable_extension(extension);
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

  auto inst_ret = builder.build();
  if (!inst_ret) {
    OX_LOG_ERROR(
      "Couldn't initialize the instance! Make sure your GPU drivers are up to date and it supports Vulkan 1.3"
    );
  }

  self.render_device->instance = inst_ret.value();
  auto instance = self.render_device->instance.instance;
  vkb::PhysicalDeviceSelector selector{self.render_device->instance};
  self.render_device->surface = window.get_surface(instance);
  selector //
    .set_surface(self.render_device->surface)
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
    self.render_device->physical = phys_ret.value();
    self.device_name = phys_ret.value().name;
    self.device_api_version = phys_ret.value().properties.apiVersion;
  }

  self.physical_device = self.render_device->physical.physical_device;

  const auto vk10_supported = query_device_features(self.render_device->instance, self.physical_device);
  if (!vk10_supported.shaderStorageImageExtendedFormats) {
    OX_LOG_FATAL(
      "The selected device does not support shaderStorageImageExtendedFormats, which is required by the renderer."
    );
  }

  constexpr auto sampled_color = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
  constexpr auto sampled_storage = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
  constexpr auto sampled_storage_color = sampled_storage | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
  const auto require_format = [&](vir::Format format, VkFormatFeatureFlags features, std::string_view name) {
    if (!supports_optimal_format_features(self.render_device->instance, self.physical_device, format, features)) {
      OX_LOG_FATAL("The selected device does not support the required sampled/color/storage usage for {}.", name);
    }
  };
  require_format(vir::Format::RGBA8Snorm, sampled_color, "R8G8B8A8_SNORM normals");
  require_format(vir::Format::RG8Unorm, sampled_storage_color, "R8G8_UNORM material and shadow targets");
  require_format(vir::Format::R8Unorm, sampled_storage, "R8_UNORM ambient occlusion");
  require_format(
    vir::Format::B10G11R11UfloatPack32,
    sampled_storage_color,
    "B10G11R11_UFLOAT_PACK32 HDR and bloom targets"
  );

  const auto vk12_supported = query_device_feature<VkPhysicalDeviceVulkan12Features>(
    self.render_device->instance,
    self.physical_device,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES
  );
  if (!vk12_supported.drawIndirectCount) {
    OX_LOG_FATAL("The selected device does not support drawIndirectCount, which is required by the renderer.");
  }
  VkPhysicalDeviceMeshShaderFeaturesEXT mesh_shader_features = {};
  mesh_shader_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
  if (self.render_device->physical.is_extension_present(VK_EXT_MESH_SHADER_EXTENSION_NAME)) {
    const auto supported = query_device_feature<VkPhysicalDeviceMeshShaderFeaturesEXT>(
      self.render_device->instance,
      self.physical_device,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT
    );
    if (supported.meshShader && supported.taskShader) {
      self.render_device->physical.enable_extension_if_present(VK_EXT_MESH_SHADER_EXTENSION_NAME);
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
    self.render_device->physical.is_extension_present(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
    self.render_device->physical.is_extension_present(VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
    self.render_device->physical.is_extension_present(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME)
  ) {
    const auto as_supported = query_device_feature<VkPhysicalDeviceAccelerationStructureFeaturesKHR>(
      self.render_device->instance,
      self.physical_device,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR
    );
    const auto ray_query_supported = query_device_feature<VkPhysicalDeviceRayQueryFeaturesKHR>(
      self.render_device->instance,
      self.physical_device,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR
    );
    if (as_supported.accelerationStructure && ray_query_supported.rayQuery) {
      self.render_device->physical.enable_extension_if_present(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
      self.render_device->physical.enable_extension_if_present(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
      self.render_device->physical.enable_extension_if_present(VK_KHR_RAY_QUERY_EXTENSION_NAME);
      acceleration_structure_features.accelerationStructure = true;
      ray_query_features.rayQuery = true;
      self.features |= RenderContext::Feature::RayTracing;

      if (self.render_device->physical.is_extension_present(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME)) {
        const auto pipeline_supported = query_device_feature<VkPhysicalDeviceRayTracingPipelineFeaturesKHR>(
          self.render_device->instance,
          self.physical_device,
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR
        );
        if (pipeline_supported.rayTracingPipeline) {
          self.render_device->physical.enable_extension_if_present(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
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

  vkb::DeviceBuilder device_builder{self.render_device->physical};

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
  // every cooked texture is block compressed, so a device without it can't show any of them
  vk10_features.features.textureCompressionBC = vk10_supported.textureCompressionBC;
  if (!vk10_features.features.textureCompressionBC) {
    OX_LOG_ERROR("{} has no BC texture support, cooked textures won't display.", self.device_name);
  }

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
  // shader features
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
  vk13_features.dynamicRendering = true;
  vk13_features.shaderDemoteToHelperInvocation = true;

  VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT image_atomic_int64_features = {};
  image_atomic_int64_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT;
  image_atomic_int64_features.shaderImageInt64Atomics = true;

  device_builder //
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

  self.render_device->device = dev_ret.value();
  self.device = self.render_device->device.device;
  auto vir_context = vir::Context::create({
    .instance = instance,
    .physical_device = self.physical_device,
    .device = self.device,
    .get_instance_proc_addr = self.render_device->instance.fp_vkGetInstanceProcAddr,
    .api_version = VK_API_VERSION_1_3,
  });
  if (!vir_context) {
    OX_LOG_FATAL("failed to create vir context ({})", std::to_underlying(vir_context.error()));
  }
  const auto family = self.render_device->device.get_queue_index(vkb::QueueType::graphics);
  if (!family) {
    OX_LOG_FATAL("failed to find graphics queue: {}", family.error().message());
  }
  const auto queue = (*vir_context)->create_command_queue(*family, vir::DomainFlag::Graphics);
  if (!queue) {
    OX_LOG_FATAL("failed to create graphics timeline ({})", std::to_underlying(queue.error()));
  }
  const auto initialized = self.initialize_graph(std::move(*vir_context));
  if (!initialized) {
    OX_LOG_FATAL("failed to initialize graph ({})", std::to_underlying(initialized.error()));
  }
  self.set_vsync(self.context_cvar.cvar_vsync.as_bool());

  const auto properties = query_device_property<VkPhysicalDeviceDescriptorIndexingProperties>(
    self.render_device->instance,
    self.physical_device,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES
  );
  const auto requested = static_cast<u32>(std::max(self.context_cvar.cvar_bindless_descriptor_count.get(), 1));
  auto counts = std::array{
    std::min(
      {requested,
       properties.maxDescriptorSetUpdateAfterBindSamplers,
       properties.maxPerStageDescriptorUpdateAfterBindSamplers}
    ),
    std::min(
      {requested,
       properties.maxDescriptorSetUpdateAfterBindSampledImages,
       properties.maxPerStageDescriptorUpdateAfterBindSampledImages}
    ),
    std::min(
      {requested,
       properties.maxDescriptorSetUpdateAfterBindStorageImages,
       properties.maxPerStageDescriptorUpdateAfterBindStorageImages}
    ),
  };
  const auto limit = std::min(
    properties.maxUpdateAfterBindDescriptorsInAllPools,
    properties.maxPerStageUpdateAfterBindResources
  );
  const auto total = u64(counts[0]) + counts[1] + counts[2];
  if (total > limit) {
    for (auto& count : counts) {
      count = static_cast<u32>(u64(count) * limit / total);
    }
  }
  if (std::ranges::any_of(counts, [](u32 count) -> bool { return count == 0; })) {
    OX_LOG_FATAL("device cannot provide bindless descriptor arrays");
  }
  const auto bindings = std::array{
    VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_SAMPLER, counts[0], VK_SHADER_STAGE_ALL, nullptr},
    VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, counts[1], VK_SHADER_STAGE_ALL, nullptr},
    VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, counts[2], VK_SHADER_STAGE_ALL, nullptr},
  };
  constexpr auto flags = VkDescriptorBindingFlags(
    VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT
  );
  const auto binding_flags = std::array{flags, flags, flags};
  auto descriptor_set = self.create_persistent_descriptor_set(bindings, binding_flags);
  if (!descriptor_set) {
    OX_LOG_FATAL("failed to create bindless descriptors ({})", std::to_underlying(descriptor_set.error()));
  }
  self.resources->descriptor_set = std::move(*descriptor_set);
  self.tracy_profiler = std::make_unique<TracyProfiler>();
  self.tracy_profiler->init_for_vulkan(self);
  self.graph->callbacks = self.tracy_profiler->callbacks();
  const auto subscribed = App::get_event_system().subscribe<WindowResizeEvent>(
    [&self](const WindowResizeEvent&) -> void { self.handle_resize(); }
  );
  if (!subscribed) {
    OX_LOG_ERROR("failed to subscribe to window resize");
  }
  OX_LOG_INFO("vir context initialized on {}", self.device_name);
}

auto RenderContext::initialize_graph(this RenderContext& self, std::unique_ptr<vir::Context> new_context)
  -> std::expected<void, VkResult> {
  if (self.context || !new_context || self.num_inflight_frames == 0 || !new_context->queue(vir::DomainFlag::Graphics)) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }
  self.device = new_context->device;
  self.physical_device = new_context->physical_device;
  self.graphics_queue = new_context->queue(vir::DomainFlag::Graphics)->handle;
  self.context = std::move(new_context);
  self.persistent = std::make_unique<vir::PersistentAllocator>(*self.context);
  self.super_frame = std::make_unique<vir::SuperFrameAllocator>(*self.persistent, self.num_inflight_frames);
  self.frame = &self.super_frame->current();
  self.graph = std::make_unique<vir::RenderGraph>(*self.context, *self.persistent);
  return {};
}

auto RenderContext::destroy_context(this RenderContext& self) -> void {
  ZoneScoped;
  if (!self.context) {
    return;
  }
  self.wait();
  self.graph.reset();
  self.tracy_profiler.reset();
  self.frame = nullptr;
  self.super_frame.reset();
  self.seal_retirements({});
  self.collect_retired();
  const auto destroy_pool = [](auto& pool, auto&& destroy) -> void {
    for (usize index = 0; index < pool.capacity(); index++) {
      if (const auto* value = pool.slot_from_index(index)) {
        destroy(*value);
      }
    }
    pool.reset();
  };
  destroy_pool(self.resources->image_views, [&](const vir::ImageView& view) -> void {
    self.persistent->deallocate_image_view(view.handle);
  });
  destroy_pool(self.resources->samplers, [&](const vir::Sampler& sampler) -> void {
    self.persistent->deallocate_sampler(sampler.handle);
  });
  self.resources->cached_samplers.clear();
  destroy_pool(self.resources->images, [&](const vir::Image& image) -> void {
    self.persistent->deallocate_image(image);
  });
  destroy_pool(self.resources->acceleration_structures, [&](const GPUAccelerationStructure& structure) -> void {
    self.context->fn.vkDestroyAccelerationStructureKHR(self.device, structure.handle, nullptr);
  });
  destroy_pool(self.resources->buffers, [&](const vir::Buffer& buffer) -> void {
    self.persistent->deallocate_buffer(buffer);
  });
  self.resources->pipelines.reset();
  self.resources->pipeline_names.clear();
  destroy_swapchain(self, self.resources->swapchain);
  auto& descriptor = self.resources->descriptor_set;
  if (descriptor.pool != VK_NULL_HANDLE) {
    self.context->fn.vkDestroyDescriptorPool(self.device, descriptor.pool, nullptr);
  }
  if (descriptor.layout != VK_NULL_HANDLE) {
    self.context->fn.vkDestroyDescriptorSetLayout(self.device, descriptor.layout, nullptr);
  }
  descriptor = {};
  if (self.render_device->swapchain.swapchain != VK_NULL_HANDLE) {
    vkb::destroy_swapchain(self.render_device->swapchain);
    self.render_device->swapchain = {};
  }
  self.persistent.reset();
  self.context.reset();
  if (self.render_device->owns_device) {
    vkb::destroy_surface(self.render_device->instance, self.render_device->surface);
    vkb::destroy_device(self.render_device->device);
    vkb::destroy_instance(self.render_device->instance);
    *self.render_device = {};
  }
  self.device = VK_NULL_HANDLE;
  self.physical_device = VK_NULL_HANDLE;
  self.graphics_queue = VK_NULL_HANDLE;
}

auto RenderContext::recreate_swapchain(this RenderContext& self, vir::Extent2D extent)
  -> std::expected<void, VkResult> {
  ZoneScoped;
  if (extent.width == 0 || extent.height == 0) {
    return std::unexpected(VK_NOT_READY);
  }
  auto frame_lock = std::unique_lock(self.frame_mutex);
  auto* queue = self.context->queue(vir::DomainFlag::Graphics);
  auto queue_lock = std::unique_lock(queue->submit_mutex);
  const auto idle = self.context->fn.vkDeviceWaitIdle(self.device);
  if (idle != VK_SUCCESS) {
    return std::unexpected(idle);
  }
  auto builder = vkb::SwapchainBuilder(self.render_device->device, self.render_device->surface);
  builder.set_desired_min_image_count(self.num_inflight_frames)
    .set_desired_extent(extent.width, extent.height)
    .set_desired_format({static_cast<VkFormat>(vir::Format::RGBA8Srgb), VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
    .add_fallback_format({static_cast<VkFormat>(vir::Format::BGRA8Srgb), VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
    .set_image_usage_flags(
      static_cast<VkImageUsageFlags>(vir::ImageUsageFlags::ColorAttachment | vir::ImageUsageFlags::TransferDst)
    )
    .set_desired_present_mode(self.present_mode);
  if (self.resources->swapchain.handle != VK_NULL_HANDLE) {
    builder.set_old_swapchain(self.resources->swapchain.handle);
  }
  auto created = builder.build();
  if (!created) {
    OX_LOG_ERROR("failed to create swapchain: {}", created.error().message());
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }
  const auto images = created->get_images();
  const auto views = created->get_image_views();
  if (!images || !views || images->size() != views->size()) {
    if (views) {
      for (const auto view : *views) {
        self.persistent->deallocate_image_view(view);
      }
    }
    vkb::destroy_swapchain(*created);
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }
  auto next = vir::SwapChain{.handle = created->swapchain, .surface = self.render_device->surface};
  for (usize index = 0; index < images->size(); index++) {
    const auto image = vir::Image::from_info(
      (*images)[index],
      {
        .extent = {created->extent.width, created->extent.height, 1},
        .format = static_cast<vir::Format>(created->image_format),
        .usage = vir::ImageUsageFlags::ColorAttachment | vir::ImageUsageFlags::TransferDst,
      }
    );
    auto attachment = vir::ImageAttachment::from_image(image, VK_IMAGE_LAYOUT_UNDEFINED);
    attachment.image_view = (*views)[index];
    next.attachments.push_back(attachment);
    auto stack = memory::ScopedStack{};
    self.context->set_name(VK_OBJECT_TYPE_IMAGE, image.handle, stack.format("swapchain image {}", index));
  }
  for (usize index = 0; index < images->size(); index++) {
    const auto semaphore = self.persistent->allocate_binary_semaphore();
    if (!semaphore) {
      destroy_swapchain(self, next);
      vkb::destroy_swapchain(*created);
      return std::unexpected(semaphore.error());
    }
    next.semaphores.push_back(*semaphore);
  }
  destroy_swapchain(self, self.resources->swapchain);
  if (self.render_device->swapchain.swapchain != VK_NULL_HANDLE) {
    vkb::destroy_swapchain(self.render_device->swapchain);
  }
  self.render_device->swapchain = *created;
  next.generation = self.resources->swapchain_generation + 1;
  self.resources->swapchain = std::move(next);
  self.resources->swapchain_generation += 1;
  return {};
}

auto RenderContext::handle_resize(this RenderContext& self) -> void {
  const auto size = App::get_window().get_size_in_pixels();
  const auto resized = self.recreate_swapchain(
    {static_cast<u32>(std::max(size.x, 0)), static_cast<u32>(std::max(size.y, 0))}
  );
  if (!resized && resized.error() != VK_NOT_READY) {
    OX_LOG_ERROR("failed to resize swapchain ({})", std::to_underlying(resized.error()));
  }
}

auto RenderContext::new_frame(this RenderContext& self) -> std::expected<SwapchainInfo, VkResult> {
  ZoneScoped;
  if (!self.super_frame) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }
  const auto wanted_mode = self.context_cvar.cvar_vsync.as_bool() ? VK_PRESENT_MODE_FIFO_KHR
                                                                  : VK_PRESENT_MODE_IMMEDIATE_KHR;
  const auto changed = wanted_mode != self.present_mode;
  self.present_mode = wanted_mode;
  if (
    self.render_device->surface != VK_NULL_HANDLE && (changed || self.resources->swapchain.handle == VK_NULL_HANDLE)
  ) {
    const auto size = App::get_window().get_size_in_pixels();
    const auto resized = self.recreate_swapchain(
      {static_cast<u32>(std::max(size.x, 0)), static_cast<u32>(std::max(size.y, 0))}
    );
    if (!resized) {
      return std::unexpected(resized.error());
    }
  }
  auto lock = std::unique_lock(self.frame_mutex);
  const auto next = self.super_frame->next_frame();
  if (!next) {
    return std::unexpected(next.error());
  }
  self.frame = *next;
  self.current_frame = static_cast<u32>(self.super_frame->frame_counter % self.num_inflight_frames);
  self.num_frames += 1;
  self.collect_retired();
  return self.swapchain_info();
}

auto RenderContext::swapchain_info(this const RenderContext& self) -> SwapchainInfo {
  return {self.resources->swapchain.format(), self.resources->swapchain.extent(), self.resources->swapchain_generation};
}

auto RenderContext::acquire_next_image(this RenderContext& self, vir::Module& module) -> vir::ValueID {
  auto lock = std::shared_lock(self.frame_mutex);
  OX_ASSERT(self.resources->swapchain.handle != VK_NULL_HANDLE, "swapchain has not been created");
  return module.acquire_next_image(self.resources->swapchain);
}

auto RenderContext::set_vsync(this RenderContext& self, bool enable) -> void {
  self.context_cvar.cvar_vsync.set(enable ? 1 : 0);
}

auto RenderContext::is_vsync(this const RenderContext& self) -> bool {
  return self.present_mode == VK_PRESENT_MODE_FIFO_KHR;
}

auto RenderContext::compile(this RenderContext& self, vir::Module&& module) -> std::expected<vir::Program, VkResult> {
  ZoneScoped;
  memory::ScopedStack stack;
  auto lock = std::shared_lock(self.frame_mutex);
  if (!self.graph) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }
  for (const auto& variable : module.variables) {
    if (
      const auto* swapchain = std::get_if<vir::AcquiredSwapchain>(&variable.value);
      swapchain && (swapchain->handle != self.resources->swapchain.handle ||
                    (swapchain->generation && swapchain->generation != self.resources->swapchain_generation))
    ) {
      return std::unexpected(VK_ERROR_OUT_OF_DATE_KHR);
    }
  }
  auto roots = stack.alloc<vir::ValueID>(module.instructions.size());
  auto count = 0_sz;
  for (usize index = 0; index < module.instructions.size(); index++) {
    if (std::holds_alternative<vir::ir::Export>(module.instructions[index])) {
      roots[count++] = static_cast<vir::ValueID>(index);
    }
  }
  auto program = module.compile_all(self.graph->pipeline_bindings(), roots.first(count));
  if (!program) {
    return std::unexpected(program.error());
  }
  if (program_acquires_swapchain(*program)) {
    program->presentation_generation = self.resources->swapchain_generation;
  }
  return program;
}

auto RenderContext::execute(this RenderContext& self, const vir::Program& program)
  -> std::expected<vir::SubmitTicket, VkResult> {
  ZoneScoped;
  auto lock = std::shared_lock(self.frame_mutex);
  if (!self.frame) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }
  if (program_acquires_swapchain(program) && program.presentation_generation != self.resources->swapchain_generation) {
    return std::unexpected(VK_ERROR_OUT_OF_DATE_KHR);
  }
  return self.graph->execute(program, *self.frame);
}

auto RenderContext::execute(this RenderContext& self, const vir::Program& program, vir::FrameAllocator& allocator)
  -> std::expected<vir::SubmitTicket, VkResult> {
  auto lock = std::shared_lock(self.frame_mutex);
  if (!self.graph || allocator.upstream != self.persistent.get()) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }
  if (program_acquires_swapchain(program) && program.presentation_generation != self.resources->swapchain_generation) {
    return std::unexpected(VK_ERROR_OUT_OF_DATE_KHR);
  }
  return self.graph->execute(program, allocator);
}

auto RenderContext::execute_blocking(this RenderContext& self, vir::Module&& module)
  -> std::expected<vir::SubmitTicket, VkResult> {
  ZoneScoped;
  const auto program = self.compile(std::move(module));
  if (!program) {
    return std::unexpected(program.error());
  }
  return self.execute_blocking(*program);
}

auto RenderContext::execute_blocking(this RenderContext& self, const vir::Program& program)
  -> std::expected<vir::SubmitTicket, VkResult> {
  ZoneScoped;
  auto allocator = vir::FrameAllocator(*self.persistent);
  const auto ticket = self.execute(program, allocator);
  if (!ticket) {
    return std::unexpected(ticket.error());
  }
  const auto waited = self.wait(*ticket);
  if (!waited) {
    return std::unexpected(waited.error());
  }
  return ticket;
}

auto RenderContext::allocate_staging(this RenderContext& self, std::span<const u8> data)
  -> std::expected<vir::Buffer, VkResult> {
  ZoneScoped;
  if (data.empty()) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }

  auto buffer = self.persistent->allocate_buffer({
    .size = data.size_bytes(),
    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    .location = vir::MemoryLocation::CpuToGpu,
    .alignment = 16,
    .name = "staging",
  });
  if (!buffer) {
    return std::unexpected(buffer.error());
  }

  const auto written = buffer->write_bytes(0, data);
  const auto flushed = written ? self.persistent->flush_buffer(*buffer) : std::expected<void, VkResult>{};
  if (!written || !flushed) {
    self.persistent->deallocate_buffer(*buffer);
    return std::unexpected(!written ? written.error() : flushed.error());
  }

  return buffer;
}

auto RenderContext::submit_upload(
  this RenderContext& self, vir::Module&& module, std::vector<vir::Buffer> staging, UploadBatch* batch
) -> std::expected<vir::SubmitTicket, VkResult> {
  ZoneScoped;
  const auto free_staging = [&] {
    for (const auto& buffer : staging) {
      self.persistent->deallocate_buffer(buffer);
    }
  };

  auto program = self.compile(std::move(module));
  if (!program) {
    free_staging();
    return std::unexpected(program.error());
  }

  auto allocator = std::make_unique<vir::FrameAllocator>(*self.persistent);
  const auto ticket = self.execute(*program, *allocator);
  if (!ticket) {
    allocator.reset();
    free_staging();
    return std::unexpected(ticket.error());
  }

  if (batch) {
    batch->add_upload({.allocator = std::move(allocator), .staging = std::move(staging), .ticket = *ticket});
    return ticket;
  }

  const auto waited = self.wait(*ticket);
  allocator.reset();
  free_staging();
  if (!waited) {
    return std::unexpected(waited.error());
  }

  return ticket;
}

auto RenderContext::wait(this const RenderContext& self, vir::SubmitTicket ticket) -> std::expected<void, VkResult> {
  return self.context->wait(ticket);
}

auto RenderContext::is_retired(this const RenderContext& self, vir::SubmitTicket ticket) -> bool {
  return self.context->is_complete(ticket);
}

auto RenderContext::wait(this const RenderContext& self) -> void {
  auto* queue = self.context->queue(vir::DomainFlag::Graphics);
  auto lock = std::unique_lock(queue->submit_mutex);
  self.context->wait_idle();
}

auto RenderContext::create_persistent_descriptor_set(
  this RenderContext& self,
  std::span<const VkDescriptorSetLayoutBinding> bindings,
  std::span<const VkDescriptorBindingFlags> binding_flags
) -> std::expected<DescriptorSet, VkResult> {
  ZoneScoped;
  if (bindings.empty() || bindings.size() != binding_flags.size()) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }
  auto stack = memory::ScopedStack{};
  auto sizes = stack.alloc<VkDescriptorPoolSize>(bindings.size());
  for (usize index = 0; index < sizes.size(); index++) {
    sizes[index] = {bindings[index].descriptorType, bindings[index].descriptorCount};
  }
  const auto pool_info = VkDescriptorPoolCreateInfo{
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
    .pNext = nullptr,
    .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
    .maxSets = 1,
    .poolSizeCount = static_cast<u32>(sizes.size()),
    .pPoolSizes = sizes.data(),
  };
  auto descriptor = DescriptorSet{};
  const auto& fn = self.context->fn;
  auto result = fn.vkCreateDescriptorPool(self.device, &pool_info, nullptr, &descriptor.pool);
  if (result != VK_SUCCESS) {
    return std::unexpected(result);
  }
  const auto flags = VkDescriptorSetLayoutBindingFlagsCreateInfo{
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
    .pNext = nullptr,
    .bindingCount = static_cast<u32>(binding_flags.size()),
    .pBindingFlags = binding_flags.data(),
  };
  const auto layout_info = VkDescriptorSetLayoutCreateInfo{
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
    .pNext = &flags,
    .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
    .bindingCount = static_cast<u32>(bindings.size()),
    .pBindings = bindings.data(),
  };
  result = fn.vkCreateDescriptorSetLayout(self.device, &layout_info, nullptr, &descriptor.layout);
  if (result != VK_SUCCESS) {
    fn.vkDestroyDescriptorPool(self.device, descriptor.pool, nullptr);
    return std::unexpected(result);
  }
  const auto allocate = VkDescriptorSetAllocateInfo{
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
    .pNext = nullptr,
    .descriptorPool = descriptor.pool,
    .descriptorSetCount = 1,
    .pSetLayouts = &descriptor.layout,
  };
  result = fn.vkAllocateDescriptorSets(self.device, &allocate, &descriptor.set);
  if (result != VK_SUCCESS) {
    fn.vkDestroyDescriptorSetLayout(self.device, descriptor.layout, nullptr);
    fn.vkDestroyDescriptorPool(self.device, descriptor.pool, nullptr);
    return std::unexpected(result);
  }
  descriptor.bindings.assign(bindings.begin(), bindings.end());
  descriptor.flags.assign(binding_flags.begin(), binding_flags.end());
  return descriptor;
}

auto RenderContext::commit_descriptor_set(this RenderContext& self, std::span<const VkWriteDescriptorSet> writes)
  -> void {
  auto lock = std::unique_lock(self.descriptor_mutex);
  self.context->fn.vkUpdateDescriptorSets(self.device, static_cast<u32>(writes.size()), writes.data(), 0, nullptr);
}

auto RenderContext::bindless_set(this const RenderContext& self) -> vir::BindlessDescriptorSet {
  const auto& descriptor = self.resources->descriptor_set;
  return {.index = 1, .layout = descriptor.layout, .set = descriptor.set};
}

auto RenderContext::create_pipeline(this RenderContext& self, const ShaderPipelineData& pipeline_data) -> bool {
  ZoneScoped;
  if (const auto missing = missing_shader_feature(self.features, pipeline_data.required_features); !missing.empty()) {
    OX_LOG_INFO("skipped pipeline {}, device has no {} support", pipeline_data.module_name, missing);
    return true;
  }
  if (pipeline_data.entry_points.empty() || pipeline_data.module_name.empty()) {
    return false;
  }
  auto lock = std::unique_lock(self.resources->pipeline_mutex);
  if (self.resources->pipeline_names.contains(pipeline_data.module_name)) {
    OX_LOG_ERROR("pipeline {} is already declared", pipeline_data.module_name);
    return false;
  }
  auto bindless = option<vir::BindlessDescriptorSet>{};
  if (pipeline_data.required_features & ShaderFeatureFlag::Bindless) {
    bindless = self.bindless_set();
  }
  auto shaders = std::vector<vir::ShaderCode>{};
  for (const auto& entry : pipeline_data.entry_points) {
    shaders.push_back({.spirv = entry.spirv, .layout = entry.layout});
  }
  const auto is_ray_tracing = std::ranges::any_of(
    pipeline_data.entry_points,
    [](const ShaderEntryPointData& entry) -> bool { return entry.shader_stage == ShaderStage::RayGeneration; }
  );
  auto declared = std::expected<vir::PipelineID, VkResult>(std::unexpected(VK_ERROR_INITIALIZATION_FAILED));
  if (is_ray_tracing) {
    declared = self.graph->declare_ray_tracing_pipeline(
      {.shaders = std::move(shaders), .bindless = bindless, .name = pipeline_data.module_name}
    );
  } else if (
    pipeline_data.entry_points.size() == 1 && pipeline_data.entry_points[0].shader_stage == ShaderStage::Compute
  ) {
    declared = self.graph->declare_compute_pipeline(
      {.shader = std::move(shaders[0]), .bindless = bindless, .name = pipeline_data.module_name}
    );
  } else {
    declared = self.graph->declare_graphics_pipeline(
      {.shaders = std::move(shaders), .bindless = bindless, .name = pipeline_data.module_name}
    );
  }
  if (!declared) {
    OX_LOG_ERROR("failed to declare pipeline {} ({})", pipeline_data.module_name, std::to_underlying(declared.error()));
    return false;
  }
  const auto id = self.resources->pipelines.create_slot(std::move(*declared));
  self.resources->pipeline_names.emplace(pipeline_data.module_name, id);
  return true;
}

auto RenderContext::pipeline(this const RenderContext& self, std::string_view name) -> vir::PipelineID {
  auto lock = std::shared_lock(self.resources->pipeline_mutex);
  const auto it = self.resources->pipeline_names.find(name);
  return it == self.resources->pipeline_names.end() ? vir::PipelineID::Invalid : self.pipeline(it->second);
}

auto RenderContext::pipeline(this const RenderContext& self, PipelineID id) -> vir::PipelineID {
  return self.resources->pipelines.copy_slot(id).value_or(vir::PipelineID::Invalid);
}

auto RenderContext::load_shader_pack(this RenderContext& self, const AssetFile& pack) -> void {
  ZoneScoped;

  for (const auto& entry : pack.entries) {
    const auto* pipeline_data = std::get_if<ShaderPipelineData>(&entry.data);
    if (!pipeline_data) {
      continue;
    }

    self.create_pipeline(*pipeline_data);
  }
}

auto RenderContext::use_mesh_shaders(this const RenderContext& self) -> bool {
  return (self.features & Feature::MeshShaders) && self.context_cvar.cvar_mesh_shaders.as_bool();
}

auto RenderContext::use_ray_tracing(this const RenderContext& self) -> bool {
  return (self.features & Feature::RayTracing) && self.context_cvar.cvar_ray_tracing.as_bool() && self.context &&
         self.context->fn.vkGetAccelerationStructureDeviceAddressKHR != nullptr;
}

auto RenderContext::use_ray_tracing_pipeline(this const RenderContext& self) -> bool {
  return self.use_ray_tracing() && (self.features & Feature::RayTracingPipeline);
}

auto RenderContext::as_scratch_alignment(this const RenderContext& self) -> u64 {
  return self.context->acceleration_structure_scratch_alignment;
}
auto RenderContext::allocate_buffer(this RenderContext& self, const vir::BufferInfo& info)
  -> std::expected<BufferID, VkResult> {
  ZoneScoped;
  auto value = self.persistent->allocate_buffer(info);
  if (!value) {
    return std::unexpected(value.error());
  }
  return self.resources->buffers.create_slot(std::move(*value));
}

auto RenderContext::destroy_buffer(this RenderContext& self, BufferID id) -> void {
  retire_resource(self, self.resources->buffers, id);
}

auto RenderContext::buffer(this const RenderContext& self, BufferID id) -> vir::Buffer {
  return live_resource(*self.resources, self.resources->buffers, id);
}

auto RenderContext::allocate_frame_buffer(this RenderContext& self, const vir::BufferInfo& info)
  -> std::expected<vir::Buffer, VkResult> {
  auto lock = std::unique_lock(self.frame_mutex);
  if (!self.frame) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }
  return self.frame->allocate_buffer(info);
}

auto RenderContext::upload_frame_buffer(this RenderContext& self, std::span<const u8> data, std::string_view name)
  -> std::expected<vir::Buffer, VkResult> {
  ZoneScoped;
  auto buffer = self.allocate_frame_buffer({
    .size = std::max<u64>(data.size_bytes(), 16),
    .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
             VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
             VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    .location = vir::MemoryLocation::CpuToGpu,
    .alignment = 16,
    .name = std::string(name),
  });
  if (!buffer) {
    return std::unexpected(buffer.error());
  }

  if (!data.empty()) {
    if (const auto written = buffer->write_bytes(0, data); !written) {
      return std::unexpected(written.error());
    }
  }

  if (const auto flushed = self.persistent->flush_buffer(*buffer); !flushed) {
    return std::unexpected(flushed.error());
  }

  return buffer;
}

auto RenderContext::resize_buffer(this RenderContext& self, BufferID& id, u64 size, std::string_view name)
  -> std::expected<bool, VkResult> {
  ZoneScoped;
  const auto current = id != BufferID::Invalid ? self.buffer(id).size : 0_u64;
  if (id != BufferID::Invalid && size <= current) {
    return false;
  }

  auto grown = self.allocate_buffer({
    .size = std::max({size, current * 2, 16_u64}),
    .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
             VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    .location = vir::MemoryLocation::GpuOnly,
    .name = std::string(name),
  });
  if (!grown) {
    return std::unexpected(grown.error());
  }

  if (id != BufferID::Invalid) {
    self.destroy_buffer(id);
  }

  id = *grown;
  return true;
}

auto RenderContext::staging_buffer(
  this RenderContext& self, vir::FrameAllocator& allocator, std::span<const u8> data, u64 alignment
) -> std::expected<vir::Buffer, VkResult> {
  ZoneScoped;
  if (allocator.upstream != self.persistent.get() || data.empty()) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }
  auto value = allocator.allocate_buffer({
    .size = data.size_bytes(),
    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    .location = vir::MemoryLocation::CpuToGpu,
    .alignment = alignment,
  });
  if (!value) {
    return std::unexpected(value.error());
  }
  if (const auto written = value->write_bytes(0, data); !written) {
    return std::unexpected(written.error());
  }
  if (const auto flushed = self.persistent->flush_buffer(*value); !flushed) {
    return std::unexpected(flushed.error());
  }
  return value;
}

auto RenderContext::allocate_image(this RenderContext& self, const vir::ImageInfo& info)
  -> std::expected<ImageID, VkResult> {
  ZoneScoped;
  auto value = self.persistent->allocate_image(info);
  if (!value) {
    return std::unexpected(value.error());
  }
  return self.resources->images.create_slot(std::move(*value));
}

auto RenderContext::destroy_image(this RenderContext& self, ImageID id) -> void {
  retire_resource(self, self.resources->images, id);
}

auto RenderContext::image(this const RenderContext& self, ImageID id) -> vir::Image {
  return live_resource(*self.resources, self.resources->images, id);
}

auto RenderContext::allocate_image_view(this RenderContext& self, const vir::ImageViewInfo& info, bool write_bindless)
  -> std::expected<ImageViewID, VkResult> {
  ZoneScoped;
  const auto usage_info = VkImageViewUsageCreateInfo{
    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
    .pNext = nullptr,
    .usage = static_cast<VkImageUsageFlags>(info.usage),
  };
  const auto create_info = VkImageViewCreateInfo{
    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
    .pNext = vir::is_empty(info.usage) ? nullptr : &usage_info,
    .flags = static_cast<VkImageViewCreateFlags>(info.flags),
    .image = info.image,
    .viewType = static_cast<VkImageViewType>(info.view_type),
    .format = static_cast<VkFormat>(info.format),
    .components = std::bit_cast<VkComponentMapping>(info.components),
    .subresourceRange = info.range,
  };
  auto handle = VkImageView{VK_NULL_HANDLE};
  if (
    const auto result = self.context->fn.vkCreateImageView(self.device, &create_info, nullptr, &handle);
    result != VK_SUCCESS
  ) {
    return std::unexpected(result);
  }
  auto id = self.resources->image_views.create_slot(vir::ImageView{handle});
  const auto index = SlotMap_decode_id(id).index;
  const auto& descriptor = self.resources->descriptor_set;
  // bounds apply to deferred writes too, upload batches write into the same arrays
  if (
    (vir::intersects(info.usage, vir::ImageUsageFlags::Sampled) &&
     index >= descriptor_capacity(descriptor, DescriptorTable_SampledImageIndex)) ||
    (vir::intersects(info.usage, vir::ImageUsageFlags::Storage) &&
     index >= descriptor_capacity(descriptor, DescriptorTable_StorageImageIndex))
  ) {
    self.resources->image_views.destroy_slot(id);
    self.persistent->deallocate_image_view(handle);
    return std::unexpected(VK_ERROR_OUT_OF_POOL_MEMORY);
  }
  self.context->set_name(VK_OBJECT_TYPE_IMAGE_VIEW, handle, info.name);
  if (!write_bindless) {
    return id;
  }
  const auto sampled = VkDescriptorImageInfo{VK_NULL_HANDLE, handle, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL};
  const auto storage = VkDescriptorImageInfo{VK_NULL_HANDLE, handle, VK_IMAGE_LAYOUT_GENERAL};
  auto writes = std::array<VkWriteDescriptorSet, 2>{};
  auto count = 0_sz;
  for (const auto [usage, binding, type, image_info] : {
         std::tuple{
           vir::ImageUsageFlags::Sampled,
           DescriptorTable_SampledImageIndex,
           VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
           &sampled
         },
         std::tuple{
           vir::ImageUsageFlags::Storage,
           DescriptorTable_StorageImageIndex,
           VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
           &storage
         },
       }) {
    if (vir::intersects(info.usage, usage)) {
      writes[count++] = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = nullptr,
        .dstSet = descriptor.set,
        .dstBinding = binding,
        .dstArrayElement = index,
        .descriptorCount = 1,
        .descriptorType = type,
        .pImageInfo = image_info,
        .pBufferInfo = nullptr,
        .pTexelBufferView = nullptr,
      };
    }
  }
  if (count) {
    self.commit_descriptor_set(std::span(writes).first(count));
  }
  return id;
}

auto RenderContext::destroy_image_view(this RenderContext& self, ImageViewID id) -> void {
  retire_resource(self, self.resources->image_views, id);
}

auto RenderContext::image_view(this const RenderContext& self, ImageViewID id) -> vir::ImageView {
  return live_resource(*self.resources, self.resources->image_views, id);
}

auto RenderContext::allocate_sampler(this RenderContext& self, const vir::SamplerInfo& info, bool write_bindless)
  -> std::expected<SamplerID, VkResult> {
  ZoneScoped;
  auto value = self.persistent->allocate_sampler(info);
  if (!value) {
    return std::unexpected(value.error());
  }
  const auto id = self.resources->samplers.create_slot(vir::Sampler{*value});
  const auto index = SlotMap_decode_id(id).index;
  const auto& descriptor = self.resources->descriptor_set;
  if (index >= descriptor_capacity(descriptor, DescriptorTable_SamplerIndex)) {
    self.resources->samplers.destroy_slot(id);
    self.persistent->deallocate_sampler(*value);
    return std::unexpected(VK_ERROR_OUT_OF_POOL_MEMORY);
  }
  if (write_bindless) {
    const auto image_info = VkDescriptorImageInfo{*value, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    const auto write = VkWriteDescriptorSet{
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .pNext = nullptr,
      .dstSet = descriptor.set,
      .dstBinding = DescriptorTable_SamplerIndex,
      .dstArrayElement = index,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
      .pImageInfo = &image_info,
      .pBufferInfo = nullptr,
      .pTexelBufferView = nullptr,
    };
    self.commit_descriptor_set(std::span(&write, 1));
  }
  return id;
}

auto RenderContext::destroy_sampler(this RenderContext& self, SamplerID id) -> void {
  retire_resource(self, self.resources->samplers, id);
}

auto RenderContext::sampler(this const RenderContext& self, SamplerID id) -> vir::Sampler {
  return live_resource(*self.resources, self.resources->samplers, id);
}

auto RenderContext::allocate_acceleration_structure(
  this RenderContext& self, VkAccelerationStructureTypeKHR type, u64 size, std::string_view name
) -> std::expected<AccelerationStructureID, VkResult> {
  ZoneScoped;

  // VkAccelerationStructureCreateInfoKHR::offset must be a multiple of 256 by spec, not by a limit
  const auto buffer_id = self.allocate_buffer({
    .size = size,
    .usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    .location = vir::MemoryLocation::GpuOnly,
    .alignment = 256,
    .name = std::string(name),
  });
  if (!buffer_id) {
    return std::unexpected(buffer_id.error());
  }

  const auto buffer = self.buffer(*buffer_id);
  const auto create_info = VkAccelerationStructureCreateInfoKHR{
    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
    .pNext = nullptr,
    .createFlags = 0,
    .buffer = buffer.handle,
    .offset = buffer.offset,
    .size = size,
    .type = type,
    .deviceAddress = 0,
  };
  auto handle = VkAccelerationStructureKHR{VK_NULL_HANDLE};
  if (
    const auto result = self.context->fn.vkCreateAccelerationStructureKHR(self.device, &create_info, nullptr, &handle);
    result != VK_SUCCESS
  ) {
    self.destroy_buffer(*buffer_id);
    return std::unexpected(result);
  }

  return self.resources->acceleration_structures.create_slot({
    .handle = handle,
    .buffer = *buffer_id,
    .device_address = self.get_accel_structure_device_address(handle),
    .size = size,
  });
}

auto RenderContext::create_acceleration_structure_in(
  this RenderContext& self, VkAccelerationStructureTypeKHR type, BufferID buffer_id, u64 offset, u64 size
) -> std::expected<AccelerationStructureID, VkResult> {
  ZoneScoped;

  const auto buffer = self.buffer(buffer_id);
  const auto create_info = VkAccelerationStructureCreateInfoKHR{
    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
    .pNext = nullptr,
    .createFlags = 0,
    .buffer = buffer.handle,
    .offset = buffer.offset + offset,
    .size = size,
    .type = type,
    .deviceAddress = 0,
  };
  auto handle = VkAccelerationStructureKHR{VK_NULL_HANDLE};
  if (
    const auto result = self.context->fn.vkCreateAccelerationStructureKHR(self.device, &create_info, nullptr, &handle);
    result != VK_SUCCESS
  ) {
    return std::unexpected(result);
  }

  return self.resources->acceleration_structures.create_slot({
    .handle = handle,
    .buffer = BufferID::Invalid,
    .device_address = self.get_accel_structure_device_address(handle),
    .size = size,
  });
}

auto RenderContext::destroy_acceleration_structure(this RenderContext& self, AccelerationStructureID id) -> void {
  const auto structure = self.resources->acceleration_structures.copy_slot(id);
  if (!structure) {
    return;
  }

  retire_resource(self, self.resources->acceleration_structures, id);
  if (structure->buffer != BufferID::Invalid) {
    self.destroy_buffer(structure->buffer);
  }
}

auto RenderContext::acceleration_structure(this const RenderContext& self, AccelerationStructureID id)
  -> GPUAccelerationStructure {
  return live_resource(*self.resources, self.resources->acceleration_structures, id);
}

auto RenderContext::seal_retirements(this RenderContext& self, vir::SubmitTicket frame_ticket) -> void {
  auto lock = std::unique_lock(self.resources->retirement_mutex);
  for (auto& resource : self.resources->retired) {
    if (!resource.is_sealed) {
      resource.ticket = frame_ticket;
      resource.is_sealed = true;
    }
  }
}

auto RenderContext::cached_sampler(this RenderContext& self, const vir::SamplerInfo& info) -> SamplerID {
  {
    auto lock = std::shared_lock(self.resources->sampler_cache_mutex);
    const auto it = std::ranges::find(
      self.resources->cached_samplers,
      info,
      &std::pair<vir::SamplerInfo, SamplerID>::first
    );
    if (it != self.resources->cached_samplers.end()) {
      return it->second;
    }
  }

  auto lock = std::unique_lock(self.resources->sampler_cache_mutex);
  const auto it = std::ranges::find(
    self.resources->cached_samplers,
    info,
    &std::pair<vir::SamplerInfo, SamplerID>::first
  );
  if (it != self.resources->cached_samplers.end()) {
    return it->second;
  }

  const auto id = self.allocate_sampler(info);
  if (!id) {
    OX_LOG_ERROR("failed to create a cached sampler ({})", std::to_underlying(id.error()));
    return SamplerID::Invalid;
  }

  self.resources->cached_samplers.emplace_back(info, *id);
  return *id;
}

auto RenderContext::cached_sampler_handle(this RenderContext& self, const vir::SamplerInfo& info) -> VkSampler {
  return self.sampler(self.cached_sampler(info)).handle;
}

auto RenderContext::collect_retired(this RenderContext& self) -> void {
  ZoneScoped;
  auto lock = std::unique_lock(self.resources->retirement_mutex);
  std::erase_if(self.resources->retired, [&self](const RetiredRenderResource& resource) -> bool {
    if (!resource.is_sealed || !self.is_retired(resource.ticket)) {
      return false;
    }
    destroy_retired_resource(self, resource);
    std::visit(
      [&self](auto id) -> void {
        using ID = decltype(id);
        if constexpr (std::is_same_v<ID, BufferID>) {
          self.resources->buffers.destroy_slot(id);
        } else if constexpr (std::is_same_v<ID, ImageID>) {
          self.resources->images.destroy_slot(id);
        } else if constexpr (std::is_same_v<ID, ImageViewID>) {
          self.resources->image_views.destroy_slot(id);
        } else if constexpr (std::is_same_v<ID, SamplerID>) {
          self.resources->samplers.destroy_slot(id);
        } else {
          self.resources->acceleration_structures.destroy_slot(id);
        }
      },
      resource.id
    );
    return true;
  });
}

auto RenderContext::get_accel_structure_device_address(
  this const RenderContext& self, VkAccelerationStructureKHR handle
) -> u64 {
  const auto info = VkAccelerationStructureDeviceAddressInfoKHR{
    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
    .pNext = nullptr,
    .accelerationStructure = handle,
  };
  return self.context->fn.vkGetAccelerationStructureDeviceAddressKHR(self.device, &info);
}

auto RenderContext::create_query_pool(this const RenderContext& self, VkQueryType type, u32 count) -> VkQueryPool {
  const auto info = VkQueryPoolCreateInfo{
    .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
    .pNext = nullptr,
    .flags = 0,
    .queryType = type,
    .queryCount = count,
    .pipelineStatistics = 0,
  };
  auto pool = VkQueryPool{VK_NULL_HANDLE};
  if (self.context->fn.vkCreateQueryPool(self.device, &info, nullptr, &pool) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  self.context->fn.vkResetQueryPool(self.device, pool, 0, count);
  return pool;
}

auto RenderContext::destroy_query_pool(this const RenderContext& self, VkQueryPool pool) -> void {
  if (pool != VK_NULL_HANDLE) {
    self.context->fn.vkDestroyQueryPool(self.device, pool, nullptr);
  }
}

auto RenderContext::read_query_pool(this const RenderContext& self, VkQueryPool pool, std::span<u64> results) -> bool {
  if (pool == VK_NULL_HANDLE || results.empty()) {
    return false;
  }
  return self.context->fn.vkGetQueryPoolResults(
           self.device,
           pool,
           0,
           static_cast<u32>(results.size()),
           results.size_bytes(),
           results.data(),
           sizeof(u64),
           VK_QUERY_RESULT_64_BIT
         ) == VK_SUCCESS;
}

} // namespace ox
