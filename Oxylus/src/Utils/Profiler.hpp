#pragma once

#include <vuk/Executor.hpp>
#include <vuk/Types.hpp>
#include <vuk/runtime/CommandBuffer.hpp>
#include <vuk/runtime/vk/VkQueueExecutor.hpp>
#include <vuk/runtime/vk/VkRuntime.hpp>

#define TRACY_VK_USE_SYMBOL_TABLE

#include <tracy/Tracy.hpp>
#include <tracy/TracyVulkan.hpp>


namespace ox {
class VkContext;

class TracyProfiler {
public:
  TracyProfiler() = default;
  ~TracyProfiler() { destroy_context(); }
  void init_for_vulkan(this TracyProfiler& self, VkContext* context);
  vuk::ProfilingCallbacks setup_vuk_callback();
  void destroy_context();

private:
#if TRACY_ENABLE
  std::vector<tracy::VkCtx*> contexts;
#endif
  // command buffer and pool for Tracy to do init & collect

  vuk::Unique<vuk::CommandPool> tracy_cpool;
  vuk::Unique<vuk::CommandBufferAllocation> tracy_cbufai;
  std::vector<vuk::Executor*> executors;
};
} // namespace ox
