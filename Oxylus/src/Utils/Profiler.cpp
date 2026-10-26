#include "Utils/Profiler.hpp"

#include <new>
#include <tracy/Tracy.hpp>
#include <vulkan/vulkan_core.h>

#define TRACY_VK_USE_SYMBOL_TABLE
#include <tracy/TracyVulkan.hpp>
#include <vir/RenderGraph.hpp>

#include "Render/RenderContext.hpp"
#include "Utils/Log.hpp"

namespace ox {
struct TracyProfilerState {
#if defined(TRACY_ENABLE)
  vir::Context* context = nullptr;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer command = VK_NULL_HANDLE;
  tracy::VkCtx* tracy = nullptr;
  alignas(tracy::VkCtxScope) u8 scope_storage[sizeof(tracy::VkCtxScope)] = {};
  tracy::VkCtxScope* scope = nullptr;
#endif
};

TracyProfiler::TracyProfiler() : state(std::make_unique<TracyProfilerState>()) {}
TracyProfiler::~TracyProfiler() { destroy_context(); }

auto TracyProfiler::init_for_vulkan(this TracyProfiler& self, RenderContext& render_context) -> void {
#if defined(TRACY_ENABLE)
  self.destroy_context();
  auto& context = *render_context.context;
  auto* graphics = context.queue(vir::DomainFlag::Graphics);
  auto lock = std::unique_lock(graphics->submit_mutex);
  self.state->context = &context;
  const auto pool_info = VkCommandPoolCreateInfo{
    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
    .pNext = nullptr,
    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
    .queueFamilyIndex = graphics->family_index,
  };
  if (context.fn.vkCreateCommandPool(context.device, &pool_info, nullptr, &self.state->pool) != VK_SUCCESS) {
    OX_LOG_ERROR("failed to create Tracy command pool");
    return;
  }
  const auto buffer_info = VkCommandBufferAllocateInfo{
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .pNext = nullptr,
    .commandPool = self.state->pool,
    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    .commandBufferCount = 1,
  };
  if (context.fn.vkAllocateCommandBuffers(context.device, &buffer_info, &self.state->command) != VK_SUCCESS) {
    self.destroy_context();
    OX_LOG_ERROR("failed to allocate Tracy command buffer");
    return;
  }
  self.state->tracy = TracyVkContextCalibrated(
    context.instance,
    context.physical_device,
    context.device,
    graphics->handle,
    self.state->command,
    context.fn.vkGetInstanceProcAddr,
    context.fn.vkGetDeviceProcAddr
  );
#else
  (void)self;
  (void)render_context;
#endif
}

auto TracyProfiler::callbacks(this TracyProfiler& self) -> vir::RenderGraphCallbacks {
#if defined(TRACY_ENABLE)
  if (!self.state->tracy) {
    return {};
  }
  return {
    .user = self.state.get(),
    .on_begin_command_buffer = [](void* user, VkCommandBuffer command) -> void {
      auto& state = *static_cast<TracyProfilerState*>(user);
      TracyVkCollect(state.tracy, command);
    },
    .on_begin_pass = [](void* user, std::string_view name, VkCommandBuffer command) -> void* {
      auto& state = *static_cast<TracyProfilerState*>(user);
      OX_ASSERT(!state.scope, "GPU pass profiling must not nest");
      state.scope = new (state.scope_storage) tracy::VkCtxScope(
        state.tracy,
        __LINE__,
        __FILE__,
        sizeof(__FILE__) - 1,
        "render pass",
        sizeof("render pass") - 1,
        name.data(),
        name.size(),
        command,
        true
      );
      return state.scope;
    },
    .on_end_pass = [](void* user, void* pass, VkCommandBuffer) -> void {
      auto& state = *static_cast<TracyProfilerState*>(user);
      if (pass) {
        static_cast<tracy::VkCtxScope*>(pass)->~VkCtxScope();
        state.scope = nullptr;
      }
    },
  };
#else
  (void)self;
  return {};
#endif
}

auto TracyProfiler::destroy_context(this TracyProfiler& self) -> void {
#if defined(TRACY_ENABLE)
  if (self.state->scope) {
    self.state->scope->~VkCtxScope();
    self.state->scope = nullptr;
  }
  if (self.state->tracy) {
    TracyVkDestroy(self.state->tracy);
    self.state->tracy = nullptr;
  }
  if (self.state->pool) {
    self.state->context->fn.vkDestroyCommandPool(self.state->context->device, self.state->pool, nullptr);
    self.state->pool = VK_NULL_HANDLE;
    self.state->command = VK_NULL_HANDLE;
  }
  self.state->context = nullptr;
#else
  (void)self;
#endif
}
} // namespace ox
