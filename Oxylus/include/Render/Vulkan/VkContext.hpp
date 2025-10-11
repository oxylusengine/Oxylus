#pragma once

#include <VkBootstrap.h>
#include <vuk/RenderGraph.hpp>
#include <vuk/Value.hpp>
#include <vuk/runtime/vk/DeviceFrameResource.hpp>
#include <vuk/runtime/vk/VkRuntime.hpp>

#include "Core/Base.hpp"
#include "Core/Option.hpp"
#include "Memory/SlotMap.hpp"
#include "Render/Slang/Compiler.hpp"

namespace ox {
struct Window;
class TracyProfiler;

enum class BufferID : u64 { Invalid = ~0_u64 };
enum class ImageID : u64 { Invalid = ~0_u64 };
enum class ImageViewID : u64 { Invalid = ~0_u64 };
enum class SamplerID : u64 { Invalid = ~0_u64 };
enum class PipelineID : u64 { Invalid = ~0_u64 };

enum : u32 {
  DescriptorTable_SamplerIndex = 0,
  DescriptorTable_SampledImageIndex,
  DescriptorTable_StorageImageIndex,
};

class VkContext {
public:
  struct Resources {
    SlotMap<vuk::Buffer, BufferID> buffers = {};
    SlotMap<vuk::Image, ImageID> images = {};
    SlotMap<vuk::ImageView, ImageViewID> image_views = {};
    SlotMap<vuk::Sampler, SamplerID> samplers = {};
    SlotMap<vuk::PipelineBaseInfo*, PipelineID> pipelines = {};
    vuk::PersistentDescriptorSet descriptor_set = {};
  };

  Resources resources = {};

  VkDevice device = nullptr;
  VkPhysicalDevice physical_device = nullptr;
  vkb::PhysicalDevice vkbphysical_device;
  VkQueue graphics_queue = nullptr;
  VkQueue transfer_queue = nullptr;
  option<vuk::Runtime> runtime;

  option<vuk::DeviceSuperFrameResource> superframe_resource;
  option<vuk::Allocator> superframe_allocator = nullopt;
  option<vuk::Allocator> frame_allocator = nullopt;
  plf::colony<std::pair<u64, vuk::Buffer>> tracked_buffers = {};
  std::shared_mutex pending_image_buffers_mutex = {};

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
  ~VkContext() = default;

  auto create_context(this VkContext& self, const Window& window, bool vulkan_validation_layers) -> void;
  auto destroy_context(this VkContext& self) -> void;

  auto new_frame(this VkContext& self) -> vuk::Value<vuk::ImageAttachment>;
  auto end_frame(this VkContext& self, vuk::Value<vuk::ImageAttachment> target) -> void;

  auto handle_resize(u32 width, u32 height) -> void;
  auto set_vsync(bool enable) -> void;
  bool is_vsync() const;

  auto wait(this VkContext& self) -> void;
  auto wait_on(vuk::UntypedValue&& fut) -> void;
  auto wait_on_rg(vuk::Value<vuk::ImageAttachment>&& fut, bool frame) -> vuk::ImageAttachment;

  auto create_persistent_descriptor_set(
    this VkContext&,
    u32 set_index,
    std::span<VkDescriptorSetLayoutBinding> bindings,
    std::span<VkDescriptorBindingFlags> binding_flags
  ) -> vuk::PersistentDescriptorSet;
  auto commit_descriptor_set(this VkContext&, std::span<VkWriteDescriptorSet> writes) -> void;

  auto allocate_image(const vuk::ImageAttachment& image_attachment) -> ImageID;
  auto destroy_image(const ImageID id) -> void;
  auto image(const ImageID id) -> vuk::Image;

  auto allocate_image_view(const vuk::ImageAttachment& image_attachment) -> ImageViewID;
  auto destroy_image_view(const ImageViewID id) -> void;
  auto image_view(const ImageViewID id) -> vuk::ImageView;

  auto allocate_sampler(const vuk::SamplerCreateInfo& sampler_info) -> SamplerID;
  auto destroy_sampler(const SamplerID id) -> void;
  auto sampler(const SamplerID id) -> vuk::Sampler;

  auto get_max_viewport_count() const -> uint32_t { return vkbphysical_device.properties.limits.maxViewports; }
  auto get_descriptor_set() -> vuk::PersistentDescriptorSet& { return resources.descriptor_set; }

  auto resize_buffer(vuk::Unique<vuk::Buffer>&& buffer, vuk::MemoryUsage usage, u64 new_size)
    -> vuk::Unique<vuk::Buffer>;

  [[nodiscard]]
  auto allocate_buffer_super(vuk::MemoryUsage usage, u64 size, u64 alignment = 8) -> vuk::Unique<vuk::Buffer>;

  [[nodiscard]]
  auto alloc_image_buffer(vuk::Format format, vuk::Extent3D extent, OX_THISCALL) noexcept -> vuk::Value<vuk::Buffer>;

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
    return upload_staging(reinterpret_cast<void*>(span.data()), span.size_bytes(), dst, dst_offset, LOC);
  }

  template <typename T>
  [[nodiscard]]
  auto upload_staging(std::span<T> span, vuk::Value<vuk::Buffer>&& dst, u64 dst_offset = 0, OX_THISCALL)
    -> vuk::Value<vuk::Buffer> {
    return upload_staging(reinterpret_cast<void*>(span.data()), span.size_bytes(), std::move(dst), dst_offset, LOC);
  }

  template <typename T>
  [[nodiscard]]
  auto scratch_buffer(const T& val = {}, usize alignment = 8, OX_THISCALL) -> vuk::Value<vuk::Buffer> {
    return scratch_buffer(&val, sizeof(T), alignment, LOC);
  }

  template <typename T>
  [[nodiscard]]
  auto scratch_buffer(const std::span<T>& val = {}, usize alignment = 8, OX_THISCALL) -> vuk::Value<vuk::Buffer> {
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
