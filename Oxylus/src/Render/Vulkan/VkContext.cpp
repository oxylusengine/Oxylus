#include "Render/Vulkan/VkContext.hpp"

#include <vuk/ImageAttachment.hpp>
#include <vuk/RenderGraph.hpp>
#include <vuk/runtime/CommandBuffer.hpp>
#include <vuk/runtime/ThisThreadExecutor.hpp>
#include <vuk/runtime/vk/Allocator.hpp>
#include <vuk/runtime/vk/AllocatorHelpers.hpp>
#include <vuk/runtime/vk/PipelineInstance.hpp>
#include <vuk/runtime/vk/Query.hpp>

#include "Render/RendererConfig.hpp"
#include "Render/Window.hpp"
#include "Utils/Profiler.hpp"

namespace ox {
static VkBool32 debug_callback(const VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                               VkDebugUtilsMessageTypeFlagsEXT messageType,
                               const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
                               void* pUserData) {
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
    OX_LOG_FATAL("{}", debug_message.str());
  }
  return VK_FALSE;
}

vuk::Swapchain make_swapchain(vuk::Allocator& allocator,
                              vkb::Device& vkbdevice,
                              VkSurfaceKHR surface,
                              option<vuk::Swapchain> old_swapchain,
                              vuk::PresentModeKHR present_mode,
                              u32 frame_count) {
  vkb::SwapchainBuilder swb(vkbdevice, surface);
  swb.set_desired_min_image_count(frame_count)
      .set_desired_format(
          vuk::SurfaceFormatKHR{.format = vuk::Format::eR8G8B8A8Srgb, .colorSpace = vuk::ColorSpaceKHR::eSrgbNonlinear})
      .add_fallback_format(
          vuk::SurfaceFormatKHR{.format = vuk::Format::eB8G8R8A8Srgb, .colorSpace = vuk::ColorSpaceKHR::eSrgbNonlinear})
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

VkContext::~VkContext() { runtime->wait_idle(); }

auto VkContext::handle_resize(u32 width, u32 height) -> void {
  wait();

  if (width == 0 && height == 0) {
    suspend = true;
  } else {
    swapchain = make_swapchain(
        *superframe_allocator, vkb_device, surface, std::move(swapchain), present_mode, num_inflight_frames);
  }
}

auto VkContext::set_vsync(bool enable) -> void {
  const auto set_present_mode = enable ? vuk::PresentModeKHR::eFifo : vuk::PresentModeKHR::eImmediate;
  present_mode = set_present_mode;
}

auto VkContext::is_vsync() const -> bool { return present_mode == vuk::PresentModeKHR::eFifo; }

auto VkContext::create_context(this VkContext& self, const Window& window, bool vulkan_validation_layers) -> void {
  ZoneScoped;
  vkb::InstanceBuilder builder;
  builder //
      .set_app_name("Oxylus App")
      .set_engine_name("Oxylus")
      .require_api_version(1, 3, 0)
      .set_app_version(0, 1, 0);

  if (vulkan_validation_layers) {
    OX_LOG_INFO("Enabled vulkan validation layers.");
    builder.request_validation_layers().set_debug_callback(
        [](const VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
           const VkDebugUtilsMessageTypeFlagsEXT messageType,
           const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
           void* pUserData) -> VkBool32 {
          return debug_callback(messageSeverity, messageType, pCallbackData, pUserData);
        });
  }

  std::vector<const c8*> instance_extensions;
  instance_extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
  instance_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
  builder.enable_extensions(instance_extensions);

  auto inst_ret = builder.build();
  if (!inst_ret) {
    OX_LOG_ERROR(
        "Couldn't initialize the instance! Make sure your GPU drivers are up to date and it supports Vulkan 1.3");
  }

  self.vkb_instance = inst_ret.value();
  auto instance = self.vkb_instance.instance;
  vkb::PhysicalDeviceSelector selector{self.vkb_instance};
  self.surface = window.get_surface(instance);
  selector //
      .set_surface(self.surface)
      .prefer_gpu_device_type(vkb::PreferredDeviceType::discrete)
      .set_minimum_version(1, 3);

  std::vector<const c8*> device_extensions;
  device_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  device_extensions.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
  device_extensions.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
  device_extensions.push_back(VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME);
  // device_extensions.push_back(VK_KHR_MAINTENANCE_8_EXTENSION_NAME);
  selector.add_required_extensions(device_extensions);

  if (auto phys_ret = selector.select(); !phys_ret) {
    OX_LOG_ERROR("{}", phys_ret.full_error().type.message());
  } else {
    self.vkbphysical_device = phys_ret.value();
    self.device_name = phys_ret.value().name;
  }

  self.physical_device = self.vkbphysical_device.physical_device;
  vkb::DeviceBuilder device_builder{self.vkbphysical_device};

  VkPhysicalDeviceFeatures2 vk10_features{};
  vk10_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  vk10_features.features.shaderInt64 = true;
  vk10_features.features.vertexPipelineStoresAndAtomics = true;
  vk10_features.features.depthClamp = true;
  vk10_features.features.fillModeNonSolid = true;
  vk10_features.features.multiViewport = true;
  vk10_features.features.samplerAnisotropy = true;
  vk10_features.features.multiDrawIndirect = true;
  vk10_features.features.fragmentStoresAndAtomics = true;

  VkPhysicalDeviceVulkan11Features vk11_features{};
  vk11_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
  vk11_features.shaderDrawParameters = true;
  vk11_features.variablePointers = true;
  vk11_features.variablePointersStorageBuffer = true;

  VkPhysicalDeviceVulkan12Features vk12_features{};
  vk12_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
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
  // Shader features
  vk12_features.vulkanMemoryModel = true;
  vk12_features.storageBuffer8BitAccess = true;
  vk12_features.scalarBlockLayout = true;
  vk12_features.shaderInt8 = true;
  vk12_features.vulkanMemoryModelDeviceScope = true;
  vk12_features.shaderSubgroupExtendedTypes = true;
  vk12_features.samplerFilterMinmax = true;

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
      .add_pNext(&image_atomic_int64_features)
      .add_pNext(&vk10_features);

  auto dev_ret = device_builder.build();
  if (!dev_ret) {
    OX_LOG_ERROR("Couldn't create device");
  }

  self.vkb_device = dev_ret.value();
  self.graphics_queue = self.vkb_device.get_queue(vkb::QueueType::graphics).value();
  u32 graphics_queue_family_index = self.vkb_device.get_queue_index(vkb::QueueType::graphics).value();
  self.transfer_queue = self.vkb_device.get_queue(vkb::QueueType::transfer).value();
  auto transfer_queue_family_index = self.vkb_device.get_queue_index(vkb::QueueType::transfer).value();
  self.device = self.vkb_device.device;
  vuk::FunctionPointers fps;
  fps.vkGetInstanceProcAddr = self.vkb_instance.fp_vkGetInstanceProcAddr;
  fps.vkGetDeviceProcAddr = self.vkb_instance.fp_vkGetDeviceProcAddr;
  fps.load_pfns(instance, self.device, true);
  std::vector<std::unique_ptr<vuk::Executor>> executors;

  executors.push_back(create_vkqueue_executor(
      fps, self.device, self.graphics_queue, graphics_queue_family_index, vuk::DomainFlagBits::eGraphicsQueue));
  executors.push_back(create_vkqueue_executor(
      fps, self.device, self.transfer_queue, transfer_queue_family_index, vuk::DomainFlagBits::eTransferQueue));
  executors.push_back(std::make_unique<vuk::ThisThreadExecutor>());

  self.runtime.emplace(
      vuk::RuntimeCreateParameters{instance, self.device, self.physical_device, std::move(executors), fps});

  self.set_vsync(static_cast<bool>(RendererCVar::cvar_vsync.get()));

  self.superframe_resource.emplace(*self.runtime, self.num_inflight_frames);
  self.superframe_allocator.emplace(*self.superframe_resource);

  auto& frame_resource = self.superframe_resource->get_next_frame();
  self.frame_allocator.emplace(frame_resource);

  self.runtime->set_shader_target_version(VK_API_VERSION_1_3);

  self.shader_compiler = SlangCompiler::create().value();

  self.tracy_profiler = std::make_shared<TracyProfiler>();
  self.tracy_profiler->init_for_vulkan(&self);

  u32 instanceVersion = VK_API_VERSION_1_0;
  auto FN_vkEnumerateInstanceVersion = PFN_vkEnumerateInstanceVersion(
      fps.vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
  if (FN_vkEnumerateInstanceVersion) {
    FN_vkEnumerateInstanceVersion(&instanceVersion);
  }

  const u32 major = VK_VERSION_MAJOR(instanceVersion);
  const u32 minor = VK_VERSION_MINOR(instanceVersion);
  const u32 patch = VK_VERSION_PATCH(instanceVersion);

  OX_LOG_INFO("Vulkan context initialized using device: {} with Vulkan Version: {}.{}.{}",
              self.device_name,
              major,
              minor,
              patch);
}

auto VkContext::new_frame(this VkContext& self) -> vuk::Value<vuk::ImageAttachment> {
  ZoneScoped;

  if (self.frame_allocator) {
    self.frame_allocator.reset();
  }

  auto& frame_resource = self.superframe_resource->get_next_frame();
  self.frame_allocator.emplace(frame_resource);
  self.runtime->next_frame();

  if (!self.swapchain.has_value()) {
    self.swapchain = make_swapchain(
        *self.superframe_allocator, self.vkb_device, self.surface, {}, self.present_mode, self.num_inflight_frames);
  }
  auto acquired_swapchain = vuk::acquire_swapchain(*self.swapchain);
  auto acquired_image = vuk::acquire_next_image("present_image", std::move(acquired_swapchain));

  return acquired_image;
}

auto VkContext::end_frame(this VkContext& self, vuk::Value<vuk::ImageAttachment> target_) -> void {
  ZoneScoped;

  auto entire_thing = vuk::enqueue_presentation(std::move(target_));
  vuk::ProfilingCallbacks cbs = self.tracy_profiler->setup_vuk_callback();
  try {
  entire_thing.submit(*self.frame_allocator, self.compiler, {.graph_label = {}, .callbacks = cbs});
  } catch (vuk::Exception &) {
    // TODO: Actually handle this
  }

  self.current_frame = (self.current_frame + 1) % self.num_inflight_frames;
  self.num_frames = self.runtime->get_frame_count();
}

auto VkContext::wait(this VkContext& self) -> void {
  ZoneScoped;

  OX_LOG_INFO("Device wait idle triggered!");
  self.runtime->wait_idle();
}

auto VkContext::wait_on(vuk::UntypedValue&& fut) -> void {
  ZoneScoped;

  thread_local vuk::Compiler _compiler;
  fut.wait(frame_allocator.value(), _compiler);
}

auto VkContext::wait_on_rg(vuk::Value<vuk::ImageAttachment>&& fut, bool frame) -> vuk::ImageAttachment {
  ZoneScoped;

  auto& allocator = superframe_allocator.value();
  if (frame && frame_allocator.has_value())
    allocator = frame_allocator.value();

  thread_local vuk::Compiler _compiler;
  return *fut.get(allocator, _compiler);
}

auto VkContext::allocate_buffer(vuk::MemoryUsage usage, u64 size, u64 alignment) -> vuk::Unique<vuk::Buffer> {
  return *vuk::allocate_buffer(frame_allocator.value(), {.mem_usage = usage, .size = size, .alignment = alignment});
}

auto VkContext::allocate_buffer_super(vuk::MemoryUsage usage, u64 size, u64 alignment) -> vuk::Unique<vuk::Buffer> {
  return *vuk::allocate_buffer(superframe_allocator.value(),
                               {.mem_usage = usage, .size = size, .alignment = alignment});
}

auto
VkContext::alloc_transient_buffer_raw(vuk::MemoryUsage usage, usize size, usize alignment, vuk::source_location LOC)
    -> vuk::Buffer {
  ZoneScoped;

  std::unique_lock _(mutex);

  auto buffer = *vuk::allocate_buffer(
      frame_allocator.value(), {.mem_usage = usage, .size = size, .alignment = alignment}, LOC);
  return *buffer;
}

auto VkContext::alloc_transient_buffer(vuk::MemoryUsage usage, usize size, usize alignment, vuk::source_location LOC)
    -> vuk::Value<vuk::Buffer> {
  ZoneScoped;

  auto buffer = alloc_transient_buffer_raw(usage, size, alignment, LOC);
  return vuk::acquire_buf("transient buffer", buffer, vuk::Access::eNone, LOC);
}

auto VkContext::upload_staging(vuk::Value<vuk::Buffer>&& src, vuk::Value<vuk::Buffer>&& dst, vuk::source_location LOC)
    -> vuk::Value<vuk::Buffer> {
  ZoneScoped;

  auto upload_pass = vuk::make_pass(
      "upload staging",
      [](vuk::CommandBuffer& cmd_list,
         VUK_BA(vuk::Access::eTransferRead) src_ba,
         VUK_BA(vuk::Access::eTransferWrite) dst_ba) {
        cmd_list.copy_buffer(src_ba, dst_ba);
        return dst_ba;
      },
      vuk::DomainFlagBits::eAny,
      LOC);

  return upload_pass(std::move(src), std::move(dst));
}

auto
VkContext::upload_staging(vuk::Value<vuk::Buffer>&& src, vuk::Buffer& dst, u64 dst_offset, vuk::source_location LOC)
    -> vuk::Value<vuk::Buffer> {
  ZoneScoped;

  auto dst_buffer = vuk::discard_buf("dst", dst.subrange(dst_offset, src->size), LOC);
  return upload_staging(std::move(src), std::move(dst_buffer), LOC);
}

auto VkContext::upload_staging(void* data,
                               u64 data_size,
                               vuk::Value<vuk::Buffer>&& dst,
                               u64 dst_offset,
                               vuk::source_location LOC) -> vuk::Value<vuk::Buffer> {
  ZoneScoped;

  auto cpu_buffer = alloc_transient_buffer(vuk::MemoryUsage::eCPUonly, data_size, 8, LOC);
  std::memcpy(cpu_buffer->mapped_ptr, data, data_size);

  auto dst_buffer = vuk::discard_buf("dst", dst->subrange(dst_offset, cpu_buffer->size), LOC);
  return upload_staging(std::move(cpu_buffer), std::move(dst_buffer), LOC);
}

auto VkContext::upload_staging(void* data, u64 data_size, vuk::Buffer& dst, u64 dst_offset, vuk::source_location LOC)
    -> vuk::Value<vuk::Buffer> {
  ZoneScoped;

  auto cpu_buffer = alloc_transient_buffer(vuk::MemoryUsage::eCPUonly, data_size, 8, LOC);
  std::memcpy(cpu_buffer->mapped_ptr, data, data_size);

  auto dst_buffer = vuk::discard_buf("dst", dst.subrange(dst_offset, cpu_buffer->size), LOC);
  return upload_staging(std::move(cpu_buffer), std::move(dst_buffer), LOC);
}

auto VkContext::scratch_buffer(const void* data, u64 size, usize alignment, vuk::source_location LOC)
    -> vuk::Value<vuk::Buffer> {
  ZoneScoped;

#define SCRATCH_BUFFER_USE_BAR
#ifndef SCRATCH_BUFFER_USE_BAR
  auto cpu_buffer = alloc_transient_buffer(vuk::MemoryUsage::eCPUonly, size, alignment, LOC);
  std::memcpy(cpu_buffer->mapped_ptr, data, size);
  auto gpu_buffer = alloc_transient_buffer(vuk::MemoryUsage::eGPUonly, size, alignment, LOC);

  auto upload_pass = vuk::make_pass(
      "scratch_buffer",
      [](vuk::CommandBuffer& cmd_list,
         VUK_BA(vuk::Access::eTransferRead) src,
         VUK_BA(vuk::Access::eTransferWrite) dst) {
        cmd_list.copy_buffer(src, dst);
        return dst;
      },
      vuk::DomainFlagBits::eAny,
      LOC);

  return upload_pass(std::move(cpu_buffer), std::move(gpu_buffer));
#else
  auto buffer = alloc_transient_buffer(vuk::MemoryUsage::eGPUtoCPU, size, alignment, LOC);
  std::memcpy(buffer->mapped_ptr, data, size);
  return buffer;
#endif
}
} // namespace ox
