#pragma once

#include <Render/Slang/Compiler.hpp>
#include <VkBootstrap.h>
#include <vuk/RenderGraph.hpp>
#include <vuk/Value.hpp>
#include <vuk/runtime/vk/DeviceFrameResource.hpp>
#include <vuk/runtime/vk/VkRuntime.hpp>

#include "Core/Option.hpp"

namespace ox {
struct Window;

class VkContext {
public:
  VkDevice device = nullptr;
  VkPhysicalDevice physical_device = nullptr;
  vkb::PhysicalDevice vkbphysical_device;
  VkQueue graphics_queue = nullptr;
  VkQueue transfer_queue = nullptr;
  std::optional<vuk::Runtime> runtime;

  option<vuk::DeviceSuperFrameResource> superframe_resource;
  option<vuk::Allocator> superframe_allocator = nullopt;
  option<vuk::Allocator> frame_allocator = nullopt;

  bool suspend = false;
  vuk::PresentModeKHR present_mode = vuk::PresentModeKHR::eFifo;
  std::optional<vuk::Swapchain> swapchain;
  VkSurfaceKHR surface;
  vkb::Instance vkb_instance;
  vkb::Device vkb_device;
  u32 num_inflight_frames = vkb::SwapchainBuilder::BufferMode::TRIPLE_BUFFERING;
  u64 num_frames = 0;
  u32 current_frame = 0;
  std::shared_ptr<TracyProfiler> tracy_profiler = {};
  vuk::Compiler compiler = {};
  SlangCompiler shader_compiler = {};

  std::string device_name = {};

  VkContext() = default;
  ~VkContext();

  auto create_context(this VkContext& self, const Window& window, bool vulkan_validation_layers) -> void;

  auto new_frame(this VkContext& self) -> vuk::Value<vuk::ImageAttachment>;

  auto end_frame(this VkContext& self, vuk::Value<vuk::ImageAttachment> target) -> void;

  auto handle_resize(u32 width, u32 height) -> void;
  auto set_vsync(bool enable) -> void;

  bool is_vsync() const;

  auto get_max_viewport_count() const -> uint32_t  { return vkbphysical_device.properties.limits.maxViewports; }

  auto wait(this VkContext& self) -> void;

  auto wait_on(vuk::UntypedValue&& fut) -> void;

  auto wait_on_rg(vuk::Value<vuk::ImageAttachment>&& fut, bool frame) -> vuk::ImageAttachment;

  [[nodiscard]]
  auto allocate_buffer(vuk::MemoryUsage usage, u64 size, u64 alignment = 8) -> vuk::Unique<vuk::Buffer>;

  [[nodiscard]]
  auto allocate_buffer_super(vuk::MemoryUsage usage, u64 size, u64 alignment = 8) -> vuk::Unique<vuk::Buffer>;

  [[nodiscard]]
  auto alloc_transient_buffer_raw(vuk::MemoryUsage usage, usize size, usize alignment = 8, OX_THISCALL) -> vuk::Buffer;

  [[nodiscard]]
  auto alloc_transient_buffer(vuk::MemoryUsage usage, usize size, usize alignment = 8, OX_THISCALL)
      -> vuk::Value<vuk::Buffer>;

  [[nodiscard]]
  auto upload_staging(vuk::Value<vuk::Buffer>&& src, vuk::Value<vuk::Buffer>&& dst, OX_THISCALL)
      -> vuk::Value<vuk::Buffer>;

  [[nodiscard]]
  auto upload_staging(vuk::Value<vuk::Buffer>&& src, vuk::Buffer& dst, u64 dst_offset = 0, OX_THISCALL)
      -> vuk::Value<vuk::Buffer>;

  [[nodiscard]]
  auto upload_staging(void* data, u64 data_size, vuk::Value<vuk::Buffer>&& dst, u64 dst_offset = 0, OX_THISCALL)
      -> vuk::Value<vuk::Buffer>;

  [[nodiscard]]
  auto upload_staging(void* data, u64 data_size, vuk::Buffer& dst, u64 dst_offset = 0, OX_THISCALL)
      -> vuk::Value<vuk::Buffer>;

  template <typename T>
  [[nodiscard]]
  auto upload_staging(std::span<T> span, vuk::Buffer& dst, u64 dst_offset = 0, OX_THISCALL) -> vuk::Value<vuk::Buffer> {
    ZoneScoped;

    return upload_staging(reinterpret_cast<void*>(span.data()), span.size_bytes(), dst, dst_offset, LOC);
  }

  template <typename T>
  [[nodiscard]]
  auto upload_staging(std::span<T> span, vuk::Value<vuk::Buffer>&& dst, u64 dst_offset = 0, OX_THISCALL)
      -> vuk::Value<vuk::Buffer> {
    ZoneScoped;

    return upload_staging(reinterpret_cast<void*>(span.data()), span.size_bytes(), std::move(dst), dst_offset, LOC);
  }

  template <typename T>
  [[nodiscard]]
  auto scratch_buffer(const T& val = {}, usize alignment = 8, OX_THISCALL) -> vuk::Value<vuk::Buffer> {
    ZoneScoped;

    return scratch_buffer(&val, sizeof(T), alignment, LOC);
  }

  template <typename T>
  [[nodiscard]]
  auto scratch_buffer(const std::span<T>& val = {}, usize alignment = 8, OX_THISCALL) -> vuk::Value<vuk::Buffer> {
    ZoneScoped;
    if (val.empty())
      return vuk::Value<vuk::Buffer>{};

    return scratch_buffer(val.data(), sizeof(T) * val.size(), alignment, LOC);
  }

private:
  [[nodiscard]]
  auto scratch_buffer(const void* data, u64 size, usize alignment, OX_THISCALL) -> vuk::Value<vuk::Buffer>;

  mutable std::shared_mutex mutex = {};
};
} // namespace ox
