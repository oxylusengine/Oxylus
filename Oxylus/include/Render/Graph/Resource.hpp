#pragma once

#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <type_traits>
#include <vector>
#include <vulkan/vulkan_core.h>

#include "Core/Option.hpp"
#include "Render/Graph/Fwd.hpp"
#include "Render/Graph/Types.hpp"

namespace ox::vir {
enum class MemoryLocation : u8 {
  Unknown,
  GpuOnly,
  CpuToGpu,
  GpuToCpu,
};

struct BufferInfo {
  u64 size = 0;
  VkBufferUsageFlags usage = 0;
  MemoryLocation location = MemoryLocation::GpuOnly;
  u64 alignment = 1;
  std::string name = {};
};

struct Buffer {
  VkBuffer handle = VK_NULL_HANDLE;
  u64 offset = 0;
  u64 size = 0;
  VkDeviceAddress device_address = 0;
  u8* mapped = nullptr;
  u64 allocation_id = 0;

  auto is_null(this const Buffer& self) -> bool { return self.handle == VK_NULL_HANDLE; }
  auto is_mapped(this const Buffer& self) -> bool { return self.mapped != nullptr; }

  // a narrower view into the same backing buffer, carrying the base offset along
  auto slice(this const Buffer& self, u64 relative_offset, u64 slice_size, u64 allocation_id) -> option<Buffer>;
  auto subrange(this const Buffer& self, u64 relative_offset, u64 slice_size) -> option<Buffer>;
  auto checked_offset(this const Buffer& self, u64 relative) -> std::expected<u64, VkResult>;
  auto mapped_span(this const Buffer& self) -> std::span<u8>;
  auto write_bytes(this const Buffer& self, u64 write_offset, std::span<const u8> bytes)
    -> std::expected<void, VkResult>;

  template <typename T>
  auto write(this const Buffer& self, u64 write_offset, std::span<const T> data) -> std::expected<void, VkResult> {
    static_assert(std::is_trivially_copyable_v<T>);
    return self.write_bytes(write_offset, std::span(reinterpret_cast<const u8*>(data.data()), data.size_bytes()));
  }

  auto operator==(const Buffer& other) const -> bool {
    return handle == other.handle && offset == other.offset && size == other.size &&
           allocation_id == other.allocation_id;
  }
};

struct ImageInfo {
  Extent3D extent = {.width = 0, .height = 0, .depth = 1};
  Format format = Format::Undefined;
  ImageUsageFlags usage = ImageUsageFlags::None;
  ImageType image_type = ImageType::e2D;
  // left at max to follow the image type and layer count
  ImageViewType view_type = ImageViewType::Auto;
  ImageCreateFlags flags = ImageCreateFlags::None;
  u32 mip_levels = 1;
  u32 array_layers = 1;
  Samples samples = Samples::e1;
  MemoryLocation location = MemoryLocation::GpuOnly;
  // extra formats a mutable-format image may be viewed as
  std::vector<Format> view_formats = {};
  std::string name = {};

  static auto color_target(Extent2D extent, Format format) -> ImageInfo;
  static auto depth_target(Extent2D extent, Format format) -> ImageInfo;
  static auto texture(Extent2D extent, Format format) -> ImageInfo;

  auto subresource_range(this const ImageInfo& self) -> VkImageSubresourceRange;
  auto default_view_type(this const ImageInfo& self) -> ImageViewType;
};

struct BufferImageCopy {
  u64 buffer_offset = 0;
  u32 buffer_row_length = 0;
  u32 buffer_image_height = 0;
  VkOffset3D image_offset = {};
  Extent3D image_extent = {.width = 0, .height = 0, .depth = 1};
  u32 mip_level = 0;
  u32 base_layer = 0;
  // zero copies every layer of the view
  u32 layer_count = 0;
  // zero uses the view's aspect, depth/stencil copies select one explicitly
  VkImageAspectFlags aspect = 0;

  static auto whole(Extent3D extent) -> BufferImageCopy;
  static auto region(VkOffset2D offset, Extent2D extent) -> BufferImageCopy;

  auto is_empty(this const BufferImageCopy& self) -> bool;
  auto operator==(const BufferImageCopy& other) const -> bool;
};

struct BufferCopy {
  u64 src_offset = 0;
  u64 dst_offset = 0;
  u64 size = VK_WHOLE_SIZE;

  auto operator==(const BufferCopy&) const -> bool = default;
};

struct QueryPool {
  VkQueryPool handle = VK_NULL_HANDLE;
  VkQueryType type = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
  u32 count = 0;
};

// mip and layer indices are relative to each attachment's view
struct ImageCopy {
  u32 src_mip = 0;
  u32 dst_mip = 0;
  u32 src_layer = 0;
  u32 dst_layer = 0;
  // zero copies every shared layer remaining in the views
  u32 layer_count = 0;
  VkOffset3D src_offset = {};
  VkOffset3D dst_offset = {};
  Extent3D extent = {};

  auto is_empty(this const ImageCopy& self) -> bool;
};

struct Image {
  VkImage handle = VK_NULL_HANDLE;
  Extent3D extent = {};
  Format format = Format::Undefined;
  Samples samples = Samples::e1;
  ImageType image_type = ImageType::e2D;
  u32 mip_levels = 1;
  u32 array_layers = 1;
  ImageUsageFlags usage = ImageUsageFlags::None;

  static auto from_info(VkImage handle, const ImageInfo& info) -> Image;
  static auto imported(VkImage handle, Format format, Extent3D extent, Samples samples) -> Image;

  auto is_null(this const Image& self) -> bool { return self.handle == VK_NULL_HANDLE; }
  auto subresource_range(this const Image& self) -> VkImageSubresourceRange;

  auto operator==(const Image& other) const -> bool { return handle == other.handle; }
};

auto aspect_mask(Format format) -> VkImageAspectFlags;
auto format_texel_block_size(Format format) -> u32;
auto format_texel_block_extent(Format format) -> Extent2D;
auto image_byte_size(Format format, Extent3D extent) -> u64;

struct ImageAttachment {
  Image image = {};
  VkImageView image_view = VK_NULL_HANDLE;
  ImageViewType view_type = ImageViewType::e2D;
  Format format = Format::Undefined;
  Extent3D extent = {};
  Samples samples = Samples::e1;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageSubresourceRange subresource_range = {};

  static auto from_image(const Image& image, VkImageLayout layout) -> ImageAttachment;

  // the extent of the first mip the view covers
  auto view_extent(this const ImageAttachment& self) -> Extent3D;
};

struct SamplerInfo {
  VkFilter mag_filter = VK_FILTER_LINEAR;
  VkFilter min_filter = VK_FILTER_LINEAR;
  VkSamplerMipmapMode mipmap_mode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  VkSamplerAddressMode address_mode_u = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VkSamplerAddressMode address_mode_v = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VkSamplerAddressMode address_mode_w = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  f32 mip_lod_bias = 0.0f;
  f32 max_anisotropy = 0.0f;
  VkCompareOp compare_op = VK_COMPARE_OP_NEVER;
  f32 min_lod = 0.0f;
  f32 max_lod = VK_LOD_CLAMP_NONE;
  VkBorderColor border_color = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  // reduction modes need VkSamplerReductionModeCreateInfo, left out when weighted average
  VkSamplerReductionMode reduction_mode = VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE;

  static auto linear() -> SamplerInfo;
  static auto nearest() -> SamplerInfo;

  auto with_address_mode(this SamplerInfo self, VkSamplerAddressMode mode) -> SamplerInfo;
  auto operator==(const SamplerInfo&) const -> bool = default;
};

struct Sampler {
  VkSampler handle = VK_NULL_HANDLE;
};

struct ImageViewInfo {
  VkImage image = VK_NULL_HANDLE;
  Format format = Format::Undefined;
  ImageViewType view_type = ImageViewType::e2D;
  VkImageSubresourceRange range = {};
  ComponentMapping components = {};
  ImageViewCreateFlags flags = ImageViewCreateFlags::None;
  ImageUsageFlags usage = ImageUsageFlags::None;
  std::string name = {};
};

struct ImageView {
  VkImageView handle = VK_NULL_HANDLE;
};

struct ClearValue {
  VkClearValue value = {};

  static constexpr auto rgba_f32(f32 r, f32 g, f32 b, f32 a) -> ClearValue {
    return {.value = {.color = {.float32 = {r, g, b, a}}}};
  }

  static constexpr auto rgba_u32(u32 r, u32 g, u32 b, u32 a) -> ClearValue {
    return {.value = {.color = {.uint32 = {r, g, b, a}}}};
  }

  static constexpr auto rgba_i32(i32 r, i32 g, i32 b, i32 a) -> ClearValue {
    return {.value = {.color = {.int32 = {r, g, b, a}}}};
  }

  static constexpr auto depth(f32 depth_value) -> ClearValue { return depth_stencil(depth_value, 0); }

  static constexpr auto depth_stencil(f32 depth_value, u32 stencil) -> ClearValue {
    return {.value = {.depthStencil = {.depth = depth_value, .stencil = stencil}}};
  }

  auto operator==(const ClearValue& other) const -> bool {
    return std::memcmp(&value, &other.value, sizeof(VkClearValue)) == 0;
  }
};

namespace clear {
constexpr auto BLACK = ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 1.0f);
constexpr auto WHITE = ClearValue::rgba_f32(1.0f, 1.0f, 1.0f, 1.0f);
constexpr auto TRANSPARENT = ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 0.0f);
constexpr auto DEPTH_ZERO = ClearValue::depth(0.0f);
constexpr auto DEPTH_ONE = ClearValue::depth(1.0f);
} // namespace clear

struct SwapChain {
  VkSwapchainKHR handle = VK_NULL_HANDLE;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  // one per image, signaled by the submit that presents it
  std::vector<VkSemaphore> semaphores = {};
  std::vector<ImageAttachment> attachments = {};
  u64 generation = 0;

  auto format(this const SwapChain& self) -> Format;
  auto samples(this const SwapChain& self) -> Samples;
  auto extent(this const SwapChain& self) -> Extent2D;
};

struct AcquiredSwapchain {
  VkSwapchainKHR handle = VK_NULL_HANDLE;
  std::vector<ImageAttachment> attachments = {};
  std::vector<VkSemaphore> present_semaphores = {};
  u64 generation = 0;

  static auto from(const SwapChain& swapchain) -> AcquiredSwapchain;
};
} // namespace ox::vir
