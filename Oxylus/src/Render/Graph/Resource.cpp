#include "Render/Graph/Resource.hpp"

#include <algorithm>

#include "Render/Graph/Types.hpp"
#include "Utils/Log.hpp"

namespace ox::vir {
auto Buffer::slice(this const Buffer& self, u64 relative_offset, u64 slice_size, u64 allocation_id) -> option<Buffer> {
  if (relative_offset > self.size || slice_size > self.size - relative_offset) {
    return nullopt;
  }

  return Buffer{
    .handle = self.handle,
    .offset = self.offset + relative_offset,
    .size = slice_size,
    .device_address = self.device_address == 0 ? 0 : self.device_address + relative_offset,
    .mapped = self.mapped ? self.mapped + relative_offset : nullptr,
    .allocation_id = allocation_id,
  };
}

auto Buffer::subrange(this const Buffer& self, u64 relative_offset, u64 slice_size) -> option<Buffer> {
  return self.slice(relative_offset, slice_size, self.allocation_id);
}

auto Buffer::checked_offset(this const Buffer& self, u64 relative) -> std::expected<u64, VkResult> {
  if (relative > self.size) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }

  return self.offset + relative;
}

auto Buffer::mapped_span(this const Buffer& self) -> std::span<u8> {
  if (!self.mapped) {
    return {};
  }

  return {self.mapped, static_cast<usize>(self.size)};
}

auto Buffer::write_bytes(this const Buffer& self, u64 write_offset, std::span<const u8> bytes)
  -> std::expected<void, VkResult> {
  if (!self.mapped) {
    OX_LOG_ERROR("cannot write to a buffer that is not host visible");
    return std::unexpected(VK_ERROR_MEMORY_MAP_FAILED);
  }

  if (write_offset > self.size || bytes.size() > self.size - write_offset) {
    OX_LOG_ERROR(
      "write of {} bytes at {} runs past the end of a {} byte buffer",
      bytes.size(),
      write_offset,
      self.size
    );
    return std::unexpected(VK_ERROR_MEMORY_MAP_FAILED);
  }

  std::memcpy(self.mapped + write_offset, bytes.data(), bytes.size());
  return {};
}

auto ImageInfo::color_target(Extent2D extent, Format format) -> ImageInfo {
  return {
    .extent = {.width = extent.width, .height = extent.height, .depth = 1},
    .format = format,
    .usage = ImageUsageFlags::ColorAttachment,
  };
}

auto ImageInfo::depth_target(Extent2D extent, Format format) -> ImageInfo {
  return {
    .extent = {.width = extent.width, .height = extent.height, .depth = 1},
    .format = format,
    .usage = ImageUsageFlags::DepthStencilAttachment,
  };
}

auto ImageInfo::texture(Extent2D extent, Format format) -> ImageInfo {
  return {
    .extent = {.width = extent.width, .height = extent.height, .depth = 1},
    .format = format,
    .usage = ImageUsageFlags::Sampled | ImageUsageFlags::TransferDst,
  };
}

auto ImageInfo::subresource_range(this const ImageInfo& self) -> VkImageSubresourceRange {
  return {
    .aspectMask = aspect_mask(self.format),
    .baseMipLevel = 0,
    .levelCount = self.mip_levels,
    .baseArrayLayer = 0,
    .layerCount = self.array_layers,
  };
}

auto ImageInfo::default_view_type(this const ImageInfo& self) -> ImageViewType {
  if (self.view_type != ImageViewType::Auto) {
    return self.view_type;
  }

  switch (self.image_type) {
    case ImageType::e1D: return self.array_layers > 1 ? ImageViewType::e1DArray : ImageViewType::e1D;
    case ImageType::e3D: return ImageViewType::e3D;
    default            : return self.array_layers > 1 ? ImageViewType::e2DArray : ImageViewType::e2D;
  }
}

auto BufferImageCopy::whole(Extent3D extent) -> BufferImageCopy { return {.image_extent = extent}; }

auto BufferImageCopy::region(VkOffset2D offset, Extent2D extent) -> BufferImageCopy {
  return {
    .image_offset = {.x = offset.x, .y = offset.y, .z = 0},
    .image_extent = {.width = extent.width, .height = extent.height, .depth = 1},
  };
}

auto BufferImageCopy::is_empty(this const BufferImageCopy& self) -> bool {
  return self.image_extent.width == 0 || self.image_extent.height == 0 || self.image_extent.depth == 0;
}

auto BufferImageCopy::operator==(const BufferImageCopy& other) const -> bool {
  return buffer_offset == other.buffer_offset && buffer_row_length == other.buffer_row_length &&
         buffer_image_height == other.buffer_image_height && image_offset.x == other.image_offset.x &&
         image_offset.y == other.image_offset.y && image_offset.z == other.image_offset.z &&
         image_extent.width == other.image_extent.width && image_extent.height == other.image_extent.height &&
         image_extent.depth == other.image_extent.depth && mip_level == other.mip_level &&
         base_layer == other.base_layer && layer_count == other.layer_count && aspect == other.aspect;
}

auto Image::from_info(VkImage handle, const ImageInfo& info) -> Image {
  return {
    .handle = handle,
    .extent = info.extent,
    .format = info.format,
    .samples = info.samples,
    .image_type = info.image_type,
    .mip_levels = info.mip_levels,
    .array_layers = info.array_layers,
    .usage = info.usage,
  };
}

auto Image::imported(VkImage handle, Format format, Extent3D extent, Samples samples) -> Image {
  return {
    .handle = handle,
    .extent = extent,
    .format = format,
    .samples = samples,
  };
}

auto Image::subresource_range(this const Image& self) -> VkImageSubresourceRange {
  return {
    .aspectMask = aspect_mask(self.format),
    .baseMipLevel = 0,
    .levelCount = self.mip_levels,
    .baseArrayLayer = 0,
    .layerCount = self.array_layers,
  };
}

auto aspect_mask(Format format) -> VkImageAspectFlags {
  switch (format) {
    case Format::D16Unorm        :
    case Format::X8D24UnormPack32:
    case Format::D32Sfloat       : return VK_IMAGE_ASPECT_DEPTH_BIT;
    case Format::S8Uint          : return VK_IMAGE_ASPECT_STENCIL_BIT;
    case Format::D16UnormS8Uint  :
    case Format::D24UnormS8Uint  :
    case Format::D32SfloatS8Uint : return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default                      : return VK_IMAGE_ASPECT_COLOR_BIT;
  }
}

auto format_texel_block_size(Format format) -> u32 {
  switch (format) {
    case Format::RG4UnormPack8         :
    case Format::R8Unorm               :
    case Format::R8Snorm               :
    case Format::R8Uint                :
    case Format::R8Sint                :
    case Format::R8Srgb                :
    case Format::S8Uint                : return 1;
    case Format::RGBA4UnormPack16      :
    case Format::BGRA4UnormPack16      :
    case Format::R5G6B5UnormPack16     :
    case Format::B5G6R5UnormPack16     :
    case Format::R5G5B5A1UnormPack16   :
    case Format::B5G5R5A1UnormPack16   :
    case Format::A1R5G5B5UnormPack16   :
    case Format::RG8Unorm              :
    case Format::RG8Snorm              :
    case Format::RG8Uint               :
    case Format::RG8Sint               :
    case Format::RG8Srgb               :
    case Format::R16Unorm              :
    case Format::R16Snorm              :
    case Format::R16Uint               :
    case Format::R16Sint               :
    case Format::R16Sfloat             :
    case Format::D16Unorm              : return 2;
    case Format::RGB8Unorm             :
    case Format::RGB8Srgb              :
    case Format::BGR8Unorm             :
    case Format::BGR8Srgb              :
    case Format::D16UnormS8Uint        : return 3;
    case Format::RGBA8Unorm            :
    case Format::RGBA8Snorm            :
    case Format::RGBA8Uint             :
    case Format::RGBA8Sint             :
    case Format::RGBA8Srgb             :
    case Format::BGRA8Unorm            :
    case Format::BGRA8Snorm            :
    case Format::BGRA8Uint             :
    case Format::BGRA8Sint             :
    case Format::BGRA8Srgb             :
    case Format::ABGR8UnormPack32      :
    case Format::ABGR8SrgbPack32       :
    case Format::A2R10G10B10UnormPack32:
    case Format::A2B10G10R10UnormPack32:
    case Format::A2B10G10R10UintPack32 :
    case Format::RG16Unorm             :
    case Format::RG16Snorm             :
    case Format::RG16Uint              :
    case Format::RG16Sint              :
    case Format::RG16Sfloat            :
    case Format::R32Uint               :
    case Format::R32Sint               :
    case Format::R32Sfloat             :
    case Format::B10G11R11UfloatPack32 :
    case Format::E5B9G9R9UfloatPack32  :
    case Format::X8D24UnormPack32      :
    case Format::D32Sfloat             :
    case Format::D24UnormS8Uint        : return 4;
    case Format::D32SfloatS8Uint       : return 5;
    case Format::RGB16Unorm            :
    case Format::RGB16Sfloat           : return 6;
    case Format::RGBA16Unorm           :
    case Format::RGBA16Snorm           :
    case Format::RGBA16Uint            :
    case Format::RGBA16Sint            :
    case Format::RGBA16Sfloat          :
    case Format::RG32Uint              :
    case Format::RG32Sint              :
    case Format::RG32Sfloat            :
    case Format::R64Uint               :
    case Format::R64Sint               :
    case Format::R64Sfloat             : return 8;
    case Format::RGB32Uint             :
    case Format::RGB32Sint             :
    case Format::RGB32Sfloat           : return 12;
    case Format::RGBA32Uint            :
    case Format::RGBA32Sint            :
    case Format::RGBA32Sfloat          :
    case Format::RG64Uint              :
    case Format::RG64Sint              :
    case Format::RG64Sfloat            : return 16;
    case Format::BC1RGBUnormBlock      :
    case Format::BC1RGBSrgbBlock       :
    case Format::BC1RGBAUnormBlock     :
    case Format::BC1RGBASrgbBlock      :
    case Format::BC4UnormBlock         :
    case Format::BC4SnormBlock         :
    case Format::ETC2RGB8UnormBlock    :
    case Format::ETC2RGB8SrgbBlock     :
    case Format::ETC2R8G8B8A1UnormBlock:
    case Format::ETC2R8G8B8A1SrgbBlock :
    case Format::EACR11UnormBlock      :
    case Format::EACR11SnormBlock      : return 8;
    case Format::BC2UnormBlock         :
    case Format::BC2SrgbBlock          :
    case Format::BC3UnormBlock         :
    case Format::BC3SrgbBlock          :
    case Format::BC5UnormBlock         :
    case Format::BC5SnormBlock         :
    case Format::BC6HUfloatBlock       :
    case Format::BC6HSfloatBlock       :
    case Format::BC7UnormBlock         :
    case Format::BC7SrgbBlock          :
    case Format::ETC2RGBA8UnormBlock   :
    case Format::ETC2RGBA8SrgbBlock    :
    case Format::EACRG11UnormBlock     :
    case Format::EACRG11SnormBlock     :
    case Format::ASTC4x4UnormBlock     :
    case Format::ASTC4x4SrgbBlock      : return 16;
    default: OX_LOG_WARN("texel block size of format {} is unknown", std::to_underlying(format)); return 0;
  }
}

auto format_texel_block_extent(Format format) -> Extent2D {
  if (format >= Format::BC1RGBUnormBlock && format <= Format::ASTC4x4SrgbBlock) {
    return {.width = 4, .height = 4};
  }

  return {.width = 1, .height = 1};
}

auto image_byte_size(Format format, Extent3D extent) -> u64 {
  const auto block = format_texel_block_extent(format);
  const auto blocks_x = (extent.width + block.width - 1) / block.width;
  const auto blocks_y = (extent.height + block.height - 1) / block.height;
  return static_cast<u64>(blocks_x) * blocks_y * std::max(extent.depth, 1_u32) * format_texel_block_size(format);
}

auto ImageAttachment::from_image(const Image& image, VkImageLayout layout) -> ImageAttachment {
  auto view_type = ImageViewType::e2D;
  if (image.image_type == ImageType::e3D) {
    view_type = ImageViewType::e3D;
  } else if (image.array_layers > 1) {
    view_type = ImageViewType::e2DArray;
  }

  return {
    .image = image,
    .view_type = view_type,
    .format = image.format,
    .extent = image.extent,
    .samples = image.samples,
    .layout = layout,
    .subresource_range = image.subresource_range(),
  };
}

auto ImageAttachment::view_extent(this const ImageAttachment& self) -> Extent3D {
  const auto mip = self.subresource_range.baseMipLevel;
  return {
    .width = std::max(self.extent.width >> mip, 1_u32),
    .height = std::max(self.extent.height >> mip, 1_u32),
    .depth = std::max(self.extent.depth >> mip, 1_u32),
  };
}

auto SamplerInfo::linear() -> SamplerInfo { return {}; }

auto SamplerInfo::nearest() -> SamplerInfo {
  return {
    .mag_filter = VK_FILTER_NEAREST,
    .min_filter = VK_FILTER_NEAREST,
    .mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
  };
}

auto SamplerInfo::with_address_mode(this SamplerInfo self, VkSamplerAddressMode mode) -> SamplerInfo {
  self.address_mode_u = mode;
  self.address_mode_v = mode;
  self.address_mode_w = mode;
  return self;
}

auto SwapChain::format(this const SwapChain& self) -> Format {
  return self.attachments.empty() ? Format::Undefined : self.attachments.front().format;
}

auto SwapChain::samples(this const SwapChain& self) -> Samples {
  return self.attachments.empty() ? Samples::e1 : self.attachments.front().samples;
}

auto SwapChain::extent(this const SwapChain& self) -> Extent2D {
  if (self.attachments.empty()) {
    return {};
  }

  const auto& extent = self.attachments.front().extent;
  return {.width = extent.width, .height = extent.height};
}

auto AcquiredSwapchain::from(const SwapChain& swapchain) -> AcquiredSwapchain {
  return {
    .handle = swapchain.handle,
    .attachments = swapchain.attachments,
    .present_semaphores = swapchain.semaphores,
    .generation = swapchain.generation,
  };
}
} // namespace ox::vir
