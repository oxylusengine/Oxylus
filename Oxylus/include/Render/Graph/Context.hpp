#pragma once

#include <expected>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <vector>

#include "Render/Graph/Access.hpp"
#include "Render/Graph/Vulkan.hpp"

typedef struct VmaAllocator_T* VmaAllocator;

namespace ox::vir {
struct CommandQueue {
  VkQueue handle = VK_NULL_HANDLE;
  u32 family_index = 0;
  DomainFlag domain = DomainFlag::None;
  VkSemaphore timeline = VK_NULL_HANDLE;
  // serializes recording and submission, timeline values are only written under this lock
  u64 timeline_value = 0;
  u64 pending_query_retirement = 0;
  std::mutex submit_mutex = {};
};

struct CommandBuffer {
  const VulkanFunctions* fn = nullptr;
  VkCommandBuffer handle = VK_NULL_HANDLE;

  static auto memory_barrier_info(Access src, Access dst) -> VkMemoryBarrier2;
  static auto image_barrier_info(
    VkImage image,
    Access src,
    Access dst,
    VkImageLayout old_layout,
    VkImageLayout new_layout,
    const VkImageSubresourceRange& range
  ) -> VkImageMemoryBarrier2;

  auto begin(this const CommandBuffer& self) -> VkResult;
  auto end(this const CommandBuffer& self) -> VkResult;
  auto pipeline_barrier(
    this const CommandBuffer& self,
    std::span<const VkMemoryBarrier2> memory,
    std::span<const VkImageMemoryBarrier2> images
  ) -> void;
  auto begin_label(this const CommandBuffer& self, const c8* name, const f32 (&color)[4]) -> void;
  auto end_label(this const CommandBuffer& self) -> void;
};

struct ContextCreateInfo {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
  u32 api_version = VK_API_VERSION_1_3;
};

struct RayTracingProperties {
  u32 shader_group_handle_size = 0;
  u32 shader_group_base_alignment = 0;
  u32 shader_group_handle_alignment = 0;
  u32 max_shader_group_stride = 0;
  u32 max_ray_recursion_depth = 0;
};

struct Context {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VulkanFunctions fn = {};
  VmaAllocator memory = nullptr;
  VkPhysicalDeviceLimits limits = {};
  RayTracingProperties ray_tracing = {};
  u32 acceleration_structure_scratch_alignment = 1;
  u32 max_push_descriptors = 0;
  std::vector<std::unique_ptr<CommandQueue>> queues = {};

  static auto create(const ContextCreateInfo& info) -> std::expected<std::unique_ptr<Context>, VkResult>;

  Context() = default;
  Context(const Context&) = delete;
  Context(Context&&) = delete;
  ~Context();

  auto operator=(const Context&) -> Context& = delete;
  auto operator=(Context&&) -> Context& = delete;

  auto create_command_queue(this Context& self, u32 family_index, DomainFlag domain) -> std::expected<void, VkResult>;
  auto queue(this const Context& self, DomainFlag domain) -> CommandQueue*;
  auto queue(this const Context& self, QueueID id) -> CommandQueue*;
  auto queue_id(this const Context& self, DomainFlag domain) -> QueueID;
  auto has_debug_utils(this const Context& self) -> bool;
  auto has_push_descriptors(this const Context& self) -> bool;
  auto set_name(this const Context& self, VkObjectType type, u64 handle, std::string_view name) -> void;

  template <typename T>
  auto set_name(this const Context& self, VkObjectType type, T handle, std::string_view name) -> void {
    self.set_name(type, reinterpret_cast<u64>(handle), name);
  }

  // waits on the ticket's queue timeline without taking its execution lock
  auto wait(this const Context& self, SubmitTicket ticket) -> std::expected<void, VkResult>;
  auto is_complete(this const Context& self, SubmitTicket ticket) -> bool;
  auto wait_idle(this const Context& self) -> void;
};
} // namespace ox::vir
