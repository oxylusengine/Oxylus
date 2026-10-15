#pragma once

#include <vulkan/vulkan_core.h>

#define OX_VK_INSTANCE_FUNCTIONS(X)                                                                                    \
  X(vkGetDeviceProcAddr)                                                                                               \
  X(vkGetPhysicalDeviceProperties)                                                                                     \
  X(vkGetPhysicalDeviceProperties2)                                                                                    \
  X(vkGetPhysicalDeviceMemoryProperties)                                                                               \
  X(vkGetPhysicalDeviceMemoryProperties2)                                                                              \
  X(vkGetPhysicalDeviceFormatProperties)                                                                               \
  X(vkGetPhysicalDeviceFeatures2)                                                                                      \
  X(vkDestroySurfaceKHR)

#define OX_VK_DEVICE_FUNCTIONS(X)                                                                                      \
  X(vkDeviceWaitIdle)                                                                                                  \
  X(vkGetDeviceQueue)                                                                                                  \
  X(vkQueueSubmit2)                                                                                                    \
  X(vkQueueWaitIdle)                                                                                                   \
  X(vkQueuePresentKHR)                                                                                                 \
  X(vkAcquireNextImageKHR)                                                                                             \
  X(vkGetSwapchainImagesKHR)                                                                                           \
  X(vkCreateSemaphore)                                                                                                 \
  X(vkDestroySemaphore)                                                                                                \
  X(vkWaitSemaphores)                                                                                                  \
  X(vkGetSemaphoreCounterValue)                                                                                        \
  X(vkCreateCommandPool)                                                                                               \
  X(vkDestroyCommandPool)                                                                                              \
  X(vkResetCommandPool)                                                                                                \
  X(vkAllocateCommandBuffers)                                                                                          \
  X(vkFreeCommandBuffers)                                                                                              \
  X(vkBeginCommandBuffer)                                                                                              \
  X(vkEndCommandBuffer)                                                                                                \
  X(vkResetCommandBuffer)                                                                                              \
  X(vkCmdPipelineBarrier2)                                                                                             \
  X(vkCmdBeginRendering)                                                                                               \
  X(vkCmdEndRendering)                                                                                                 \
  X(vkCmdSetViewport)                                                                                                  \
  X(vkCmdSetScissor)                                                                                                   \
  X(vkCmdBindPipeline)                                                                                                 \
  X(vkCmdBindDescriptorSets)                                                                                           \
  X(vkCmdPushConstants)                                                                                                \
  X(vkCmdBindVertexBuffers)                                                                                            \
  X(vkCmdBindIndexBuffer)                                                                                              \
  X(vkCmdDraw)                                                                                                         \
  X(vkCmdDrawIndexed)                                                                                                  \
  X(vkCmdDrawIndirect)                                                                                                 \
  X(vkCmdDrawIndexedIndirect)                                                                                          \
  X(vkCmdDrawIndirectCount)                                                                                            \
  X(vkCmdDrawIndexedIndirectCount)                                                                                     \
  X(vkCmdDispatch)                                                                                                     \
  X(vkCmdDispatchIndirect)                                                                                             \
  X(vkCmdBlitImage)                                                                                                    \
  X(vkCmdCopyBuffer)                                                                                                   \
  X(vkCmdCopyImage)                                                                                                    \
  X(vkCmdCopyBufferToImage)                                                                                            \
  X(vkCmdCopyImageToBuffer)                                                                                            \
  X(vkCmdFillBuffer)                                                                                                   \
  X(vkCmdUpdateBuffer)                                                                                                 \
  X(vkCmdClearColorImage)                                                                                              \
  X(vkCmdClearDepthStencilImage)                                                                                       \
  X(vkCmdResolveImage)                                                                                                 \
  X(vkCmdWriteTimestamp2)                                                                                              \
  X(vkCmdResetQueryPool)                                                                                               \
  X(vkCreateBuffer)                                                                                                    \
  X(vkDestroyBuffer)                                                                                                   \
  X(vkCreateImage)                                                                                                     \
  X(vkDestroyImage)                                                                                                    \
  X(vkCreateImageView)                                                                                                 \
  X(vkDestroyImageView)                                                                                                \
  X(vkCreateSampler)                                                                                                   \
  X(vkDestroySampler)                                                                                                  \
  X(vkGetBufferDeviceAddress)                                                                                          \
  X(vkAllocateMemory)                                                                                                  \
  X(vkFreeMemory)                                                                                                      \
  X(vkMapMemory)                                                                                                       \
  X(vkUnmapMemory)                                                                                                     \
  X(vkFlushMappedMemoryRanges)                                                                                         \
  X(vkInvalidateMappedMemoryRanges)                                                                                    \
  X(vkBindBufferMemory)                                                                                                \
  X(vkBindImageMemory)                                                                                                 \
  X(vkBindBufferMemory2)                                                                                               \
  X(vkBindImageMemory2)                                                                                                \
  X(vkGetBufferMemoryRequirements)                                                                                     \
  X(vkGetImageMemoryRequirements)                                                                                      \
  X(vkGetBufferMemoryRequirements2)                                                                                    \
  X(vkGetImageMemoryRequirements2)                                                                                     \
  X(vkGetDeviceBufferMemoryRequirements)                                                                               \
  X(vkGetDeviceImageMemoryRequirements)                                                                                \
  X(vkCreateDescriptorPool)                                                                                            \
  X(vkDestroyDescriptorPool)                                                                                           \
  X(vkAllocateDescriptorSets)                                                                                          \
  X(vkUpdateDescriptorSets)                                                                                            \
  X(vkCreateDescriptorSetLayout)                                                                                       \
  X(vkDestroyDescriptorSetLayout)                                                                                      \
  X(vkCreatePipelineLayout)                                                                                            \
  X(vkDestroyPipelineLayout)                                                                                           \
  X(vkCreateShaderModule)                                                                                              \
  X(vkDestroyShaderModule)                                                                                             \
  X(vkCreateGraphicsPipelines)                                                                                         \
  X(vkCreateComputePipelines)                                                                                          \
  X(vkDestroyPipeline)                                                                                                 \
  X(vkCreateQueryPool)                                                                                                 \
  X(vkDestroyQueryPool)                                                                                                \
  X(vkResetQueryPool)                                                                                                  \
  X(vkGetQueryPoolResults)

// extension entry points, null when the device doesn't enable them
#define OX_VK_OPTIONAL_DEVICE_FUNCTIONS(X)                                                                             \
  X(vkCmdBeginDebugUtilsLabelEXT)                                                                                      \
  X(vkCmdEndDebugUtilsLabelEXT)                                                                                        \
  X(vkSetDebugUtilsObjectNameEXT)                                                                                      \
  X(vkCmdPushDescriptorSetKHR)                                                                                         \
  X(vkCmdDrawMeshTasksEXT)                                                                                             \
  X(vkCmdDrawMeshTasksIndirectEXT)                                                                                     \
  X(vkCmdDrawMeshTasksIndirectCountEXT)                                                                                \
  X(vkCreateRayTracingPipelinesKHR)                                                                                    \
  X(vkGetRayTracingShaderGroupHandlesKHR)                                                                              \
  X(vkCmdTraceRaysKHR)                                                                                                 \
  X(vkCreateAccelerationStructureKHR)                                                                                  \
  X(vkDestroyAccelerationStructureKHR)                                                                                 \
  X(vkGetAccelerationStructureBuildSizesKHR)                                                                           \
  X(vkGetAccelerationStructureDeviceAddressKHR)                                                                        \
  X(vkCmdBuildAccelerationStructuresKHR)                                                                               \
  X(vkCmdWriteAccelerationStructuresPropertiesKHR)                                                                     \
  X(vkCmdCopyAccelerationStructureKHR)

namespace ox::vir {
struct VulkanFunctions {
#define OX_VK_DECLARE(name) PFN_##name name = nullptr;
  PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
  OX_VK_INSTANCE_FUNCTIONS(OX_VK_DECLARE)
  OX_VK_DEVICE_FUNCTIONS(OX_VK_DECLARE)
  OX_VK_OPTIONAL_DEVICE_FUNCTIONS(OX_VK_DECLARE)
#undef OX_VK_DECLARE

  auto load(
    this VulkanFunctions& self, PFN_vkGetInstanceProcAddr get_instance_proc, VkInstance instance, VkDevice device
  ) -> bool;
};
} // namespace ox::vir
