#include "Render/Graph/Context.hpp"

#include <algorithm>
#include <vk_mem_alloc.h>

#include "Memory/Stack.hpp"
#include "Utils/Log.hpp"

namespace ox::vir {
auto CommandBuffer::memory_barrier_info(Access src, Access dst) -> VkMemoryBarrier2 {
  return {
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
    .pNext = nullptr,
    .srcStageMask = to_stage_flags(src),
    .srcAccessMask = to_access_flags(src),
    .dstStageMask = to_stage_flags(dst),
    .dstAccessMask = to_access_flags(dst),
  };
}

auto CommandBuffer::image_barrier_info(
  VkImage image,
  Access src,
  Access dst,
  VkImageLayout old_layout,
  VkImageLayout new_layout,
  const VkImageSubresourceRange& range
) -> VkImageMemoryBarrier2 {
  return {
    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
    .pNext = nullptr,
    .srcStageMask = to_stage_flags(src),
    .srcAccessMask = to_access_flags(src),
    .dstStageMask = to_stage_flags(dst),
    .dstAccessMask = to_access_flags(dst),
    .oldLayout = old_layout,
    .newLayout = new_layout,
    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .image = image,
    .subresourceRange = range,
  };
}

auto CommandBuffer::begin(this const CommandBuffer& self) -> VkResult {
  const auto begin_info = VkCommandBufferBeginInfo{
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .pNext = nullptr,
    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    .pInheritanceInfo = nullptr,
  };
  return self.fn->vkBeginCommandBuffer(self.handle, &begin_info);
}

auto CommandBuffer::end(this const CommandBuffer& self) -> VkResult { return self.fn->vkEndCommandBuffer(self.handle); }

auto CommandBuffer::pipeline_barrier(
  this const CommandBuffer& self,
  std::span<const VkMemoryBarrier2> memory,
  std::span<const VkImageMemoryBarrier2> images
) -> void {
  const auto dependency_info = VkDependencyInfo{
    .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
    .pNext = nullptr,
    .dependencyFlags = 0,
    .memoryBarrierCount = static_cast<u32>(memory.size()),
    .pMemoryBarriers = memory.data(),
    .bufferMemoryBarrierCount = 0,
    .pBufferMemoryBarriers = nullptr,
    .imageMemoryBarrierCount = static_cast<u32>(images.size()),
    .pImageMemoryBarriers = images.data(),
  };
  self.fn->vkCmdPipelineBarrier2(self.handle, &dependency_info);
}

auto CommandBuffer::begin_label(this const CommandBuffer& self, const c8* name, const f32 (&color)[4]) -> void {
  if (!self.fn->vkCmdBeginDebugUtilsLabelEXT) {
    return;
  }

  auto label = VkDebugUtilsLabelEXT{
    .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
    .pNext = nullptr,
    .pLabelName = name,
    .color = {color[0], color[1], color[2], color[3]},
  };
  self.fn->vkCmdBeginDebugUtilsLabelEXT(self.handle, &label);
}

auto CommandBuffer::end_label(this const CommandBuffer& self) -> void {
  if (self.fn->vkCmdEndDebugUtilsLabelEXT) {
    self.fn->vkCmdEndDebugUtilsLabelEXT(self.handle);
  }
}

auto Context::create(const ContextCreateInfo& info) -> std::expected<std::unique_ptr<Context>, VkResult> {
  ZoneScoped;

  auto context = std::make_unique<Context>();
  context->instance = info.instance;
  context->physical_device = info.physical_device;
  context->device = info.device;
  if (!context->fn.load(info.get_instance_proc_addr, info.instance, info.device)) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }

  auto push_descriptor_properties = VkPhysicalDevicePushDescriptorPropertiesKHR{
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR,
    .pNext = nullptr,
    .maxPushDescriptors = 0,
  };
  auto ray_tracing_properties = VkPhysicalDeviceRayTracingPipelinePropertiesKHR{};
  ray_tracing_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
  auto acceleration_properties = VkPhysicalDeviceAccelerationStructurePropertiesKHR{};
  acceleration_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
  acceleration_properties.pNext = &push_descriptor_properties;
  ray_tracing_properties.pNext = &acceleration_properties;
  auto properties = VkPhysicalDeviceProperties2{
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
    .pNext = &ray_tracing_properties,
    .properties = {},
  };
  context->fn.vkGetPhysicalDeviceProperties2(info.physical_device, &properties);
  context->limits = properties.properties.limits;
  context->acceleration_structure_scratch_alignment = std::max(
    acceleration_properties.minAccelerationStructureScratchOffsetAlignment,
    1_u32
  );
  if (context->fn.vkCmdPushDescriptorSetKHR) {
    context->max_push_descriptors = push_descriptor_properties.maxPushDescriptors;
  }

  if (context->fn.vkCmdTraceRaysKHR) {
    context->ray_tracing = {
      .shader_group_handle_size = ray_tracing_properties.shaderGroupHandleSize,
      .shader_group_base_alignment = ray_tracing_properties.shaderGroupBaseAlignment,
      .shader_group_handle_alignment = ray_tracing_properties.shaderGroupHandleAlignment,
      .max_shader_group_stride = ray_tracing_properties.maxShaderGroupStride,
      .max_ray_recursion_depth = ray_tracing_properties.maxRayRecursionDepth,
    };
  }

  const auto& fn = context->fn;
  auto vma_functions = VmaVulkanFunctions{
    .vkGetInstanceProcAddr = fn.vkGetInstanceProcAddr,
    .vkGetDeviceProcAddr = fn.vkGetDeviceProcAddr,
    .vkGetPhysicalDeviceProperties = fn.vkGetPhysicalDeviceProperties,
    .vkGetPhysicalDeviceMemoryProperties = fn.vkGetPhysicalDeviceMemoryProperties,
    .vkAllocateMemory = fn.vkAllocateMemory,
    .vkFreeMemory = fn.vkFreeMemory,
    .vkMapMemory = fn.vkMapMemory,
    .vkUnmapMemory = fn.vkUnmapMemory,
    .vkFlushMappedMemoryRanges = fn.vkFlushMappedMemoryRanges,
    .vkInvalidateMappedMemoryRanges = fn.vkInvalidateMappedMemoryRanges,
    .vkBindBufferMemory = fn.vkBindBufferMemory,
    .vkBindImageMemory = fn.vkBindImageMemory,
    .vkGetBufferMemoryRequirements = fn.vkGetBufferMemoryRequirements,
    .vkGetImageMemoryRequirements = fn.vkGetImageMemoryRequirements,
    .vkCreateBuffer = fn.vkCreateBuffer,
    .vkDestroyBuffer = fn.vkDestroyBuffer,
    .vkCreateImage = fn.vkCreateImage,
    .vkDestroyImage = fn.vkDestroyImage,
    .vkCmdCopyBuffer = fn.vkCmdCopyBuffer,
    .vkGetBufferMemoryRequirements2KHR = fn.vkGetBufferMemoryRequirements2,
    .vkGetImageMemoryRequirements2KHR = fn.vkGetImageMemoryRequirements2,
    .vkBindBufferMemory2KHR = fn.vkBindBufferMemory2,
    .vkBindImageMemory2KHR = fn.vkBindImageMemory2,
    .vkGetPhysicalDeviceMemoryProperties2KHR = fn.vkGetPhysicalDeviceMemoryProperties2,
    .vkGetDeviceBufferMemoryRequirements = fn.vkGetDeviceBufferMemoryRequirements,
    .vkGetDeviceImageMemoryRequirements = fn.vkGetDeviceImageMemoryRequirements,
  };

  const auto allocator_info = VmaAllocatorCreateInfo{
    .flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
    .physicalDevice = info.physical_device,
    .device = info.device,
    .preferredLargeHeapBlockSize = 0,
    .pAllocationCallbacks = nullptr,
    .pDeviceMemoryCallbacks = nullptr,
    .pHeapSizeLimit = nullptr,
    .pVulkanFunctions = &vma_functions,
    .instance = info.instance,
    .vulkanApiVersion = info.api_version,
    .pTypeExternalMemoryHandleTypes = nullptr,
  };
  if (const auto result = vmaCreateAllocator(&allocator_info, &context->memory); result != VK_SUCCESS) {
    OX_LOG_ERROR("failed to create the memory allocator ({})", std::to_underlying(result));
    return std::unexpected(result);
  }

  return context;
}

Context::~Context() {
  if (device == VK_NULL_HANDLE) {
    return;
  }

  for (const auto& queue : queues) {
    fn.vkDestroySemaphore(device, queue->timeline, nullptr);
  }

  if (memory) {
    vmaDestroyAllocator(memory);
  }
}

auto Context::create_command_queue(this Context& self, u32 family_index, DomainFlag domain)
  -> std::expected<void, VkResult> {
  auto queue = std::make_unique<CommandQueue>();
  self.fn.vkGetDeviceQueue(self.device, family_index, 0, &queue->handle);
  queue->family_index = family_index;
  queue->domain = domain;

  auto type_info = VkSemaphoreTypeCreateInfo{
    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
    .pNext = nullptr,
    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
    .initialValue = 0,
  };
  const auto semaphore_info = VkSemaphoreCreateInfo{
    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    .pNext = &type_info,
    .flags = 0,
  };
  if (
    const auto result = self.fn.vkCreateSemaphore(self.device, &semaphore_info, nullptr, &queue->timeline);
    result != VK_SUCCESS
  ) {
    return std::unexpected(result);
  }

  self.set_name(VK_OBJECT_TYPE_SEMAPHORE, queue->timeline, "queue timeline");
  self.queues.push_back(std::move(queue));
  return {};
}

auto Context::queue(this const Context& self, DomainFlag domain) -> CommandQueue* {
  for (const auto& queue : self.queues) {
    if (queue->domain == domain) {
      return queue.get();
    }
  }

  return nullptr;
}

auto Context::has_debug_utils(this const Context& self) -> bool {
  return self.fn.vkCmdBeginDebugUtilsLabelEXT != nullptr && self.fn.vkCmdEndDebugUtilsLabelEXT != nullptr;
}

auto Context::has_push_descriptors(this const Context& self) -> bool { return self.max_push_descriptors != 0; }

auto Context::set_name(this const Context& self, VkObjectType type, u64 handle, std::string_view name) -> void {
  if (!self.fn.vkSetDebugUtilsObjectNameEXT || handle == 0 || name.empty()) {
    return;
  }

  // the name has to be null terminated, and views handed in rarely are
  auto stack = memory::ScopedStack{};
  const auto name_info = VkDebugUtilsObjectNameInfoEXT{
    .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
    .pNext = nullptr,
    .objectType = type,
    .objectHandle = handle,
    .pObjectName = stack.null_terminate_cstr(name),
  };
  self.fn.vkSetDebugUtilsObjectNameEXT(self.device, &name_info);
}

auto Context::queue(this const Context& self, QueueID id) -> CommandQueue* {
  const auto index = static_cast<usize>(id);
  return index < self.queues.size() ? self.queues[index].get() : nullptr;
}

auto Context::queue_id(this const Context& self, DomainFlag domain) -> QueueID {
  const auto* found = self.queue(domain);
  for (usize index = 0; index < self.queues.size(); index++) {
    if (self.queues[index].get() == found) {
      OX_ASSERT(index < std::to_underlying(QueueID::Invalid), "exhausted queue ids");
      return static_cast<QueueID>(index);
    }
  }
  return QueueID::Invalid;
}

auto Context::wait(this const Context& self, SubmitTicket ticket) -> std::expected<void, VkResult> {
  if (ticket.value == 0) {
    return {};
  }

  const auto* graphics = self.queue(ticket.queue);
  if (!graphics) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }

  const auto wait_info = VkSemaphoreWaitInfo{
    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
    .pNext = nullptr,
    .flags = 0,
    .semaphoreCount = 1,
    .pSemaphores = &graphics->timeline,
    .pValues = &ticket.value,
  };
  if (const auto result = self.fn.vkWaitSemaphores(self.device, &wait_info, ~0_u64); result != VK_SUCCESS) {
    return std::unexpected(result);
  }

  return {};
}

auto Context::is_complete(this const Context& self, SubmitTicket ticket) -> bool {
  if (ticket.value == 0) {
    return true;
  }

  const auto* graphics = self.queue(ticket.queue);
  if (!graphics) {
    return false;
  }

  u64 reached = 0;
  if (self.fn.vkGetSemaphoreCounterValue(self.device, graphics->timeline, &reached) != VK_SUCCESS) {
    return false;
  }

  return reached >= ticket.value;
}

auto Context::wait_idle(this const Context& self) -> void { self.fn.vkDeviceWaitIdle(self.device); }
} // namespace ox::vir
