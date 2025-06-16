#include "Profiler.hpp"

#include <vuk/runtime/CommandBuffer.hpp>
#include <vuk/runtime/vk/VkQueueExecutor.hpp>
#include <vuk/runtime/vk/VkRuntime.hpp>

#include "Render/Vulkan/VkContext.hpp"

namespace ox {
void TracyProfiler::init_for_vulkan(this TracyProfiler& self, VkContext* context) {
#if TRACY_ENABLE
  vuk::Runtime& runtime = context->runtime.value();
  vuk::Allocator& allocator = context->superframe_allocator.value();
  auto graphics_queue_executor = static_cast<vuk::QueueExecutor*>(
      runtime.get_executor(vuk::DomainFlagBits::eGraphicsQueue));
  VkCommandPoolCreateInfo cpci{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                               .pNext = nullptr,
                               .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                               .queueFamilyIndex = {}};
  cpci.queueFamilyIndex = graphics_queue_executor->get_queue_family_index();
  self.tracy_cpool = vuk::Unique<vuk::CommandPool>(allocator);
  allocator.allocate_command_pools(std::span{&*self.tracy_cpool, 1}, std::span{&cpci, 1});
  vuk::CommandBufferAllocationCreateInfo ci{.command_pool = *self.tracy_cpool};
  self.tracy_cbufai = vuk::Unique<vuk::CommandBufferAllocation>(allocator);
  allocator.allocate_command_buffers(std::span{&*self.tracy_cbufai, 1}, std::span{&ci, 1});

  auto graphics_queue = graphics_queue_executor->get_underlying();
  self.executors = runtime.get_executors();
  for (size_t i = 0; i < self.executors.size(); i++) {
    auto ctx = TracyVkContextCalibrated(runtime.instance,
                                        runtime.physical_device,
                                        runtime.device,
                                        graphics_queue,
                                        self.tracy_cbufai->command_buffer,
                                        runtime.vkGetInstanceProcAddr,
                                        runtime.vkGetDeviceProcAddr);
    self.contexts.push_back(ctx);
  }
  OX_LOG_INFO("Tracy GPU profiler initialized.");
#else
  return;
#endif
}
vuk::ProfilingCallbacks TracyProfiler::setup_vuk_callback() {
#if TRACY_ENABLE
  vuk::ProfilingCallbacks cbs = {};
  cbs.user_data = this;
  cbs.on_begin_command_buffer = [](void* user_data, vuk::ExecutorTag tag, VkCommandBuffer cbuf) -> void* {
    TracyProfiler& tracy_ctx = *reinterpret_cast<TracyProfiler*>(user_data);
    if ((tag.domain & vuk::DomainFlagBits::eQueueMask) != vuk::DomainFlagBits::eTransferQueue) {
      for (auto& ctx : tracy_ctx.contexts) {
        TracyVkCollect(ctx, cbuf);
      }
    }
    return nullptr;
  };
  // runs whenever entering a new vuk::Pass
  // we start a GPU zone and then keep it open
  cbs.on_begin_pass = [](void* user_data, vuk::Name pass_name, vuk::CommandBuffer& cbuf, vuk::DomainFlagBits domain) {
    TracyProfiler& tracy_ctx = *reinterpret_cast<TracyProfiler*>(user_data);
    void* pass_data = nullptr;
    for (size_t i = 0; i < tracy_ctx.executors.size(); i++) {
      auto& exe = tracy_ctx.executors[i];
      if (exe->tag.domain == domain) {
        pass_data = new char[sizeof(tracy::VkCtxScope)];
        new (pass_data) TracyVkZoneTransient(tracy_ctx.contexts[i], , cbuf.get_underlying(), pass_name.c_str(), true);
        break;
      }
    }

    return pass_data;
  };
  // runs whenever a pass has ended, we end the GPU zone we started
  cbs.on_end_pass = [](void* user_data, void* pass_data, vuk::CommandBuffer&) {
    auto tracy_scope = reinterpret_cast<tracy::VkCtxScope*>(pass_data);
    if (tracy_scope) {
      tracy_scope->~VkCtxScope();
      delete reinterpret_cast<char*>(pass_data);
    }
  };

  return cbs;
#else
  return vuk::ProfilingCallbacks{};
#endif
}

void TracyProfiler::destroy_context() {
#if TRACY_ENABLE
  for (auto ctx : contexts) {
    TracyVkDestroy(ctx);
  }
#endif
  tracy_cbufai.reset();
  tracy_cpool.reset();
}
} // namespace ox
