#include "Render/Graph/Transfer.hpp"

#include <algorithm>
#include <bit>
#include <limits>

#include "Render/Graph/Types.hpp"

namespace ox::vir {
static auto checked_product(u64 first, u64 second) -> option<u64> {
  if (second != 0 && first > ~0_u64 / second) {
    return nullopt;
  }
  return first * second;
}

static auto buffer_image_size(const BufferImageCopy& region, Format format, VkImageAspectFlags aspect, u32 layers)
  -> option<u64> {
  const auto block = format_texel_block_extent(format);
  auto bytes = format_texel_block_size(format);
  if (aspect == VK_IMAGE_ASPECT_STENCIL_BIT) {
    bytes = 1;
  } else if (aspect == VK_IMAGE_ASPECT_DEPTH_BIT) {
    bytes = format == Format::D16Unorm || format == Format::D16UnormS8Uint ? 2 : 4;
  }
  if (
    bytes == 0 || (region.buffer_row_length != 0 && region.buffer_row_length % block.width != 0) ||
    (region.buffer_image_height != 0 && region.buffer_image_height % block.height != 0)
  ) {
    return nullopt;
  }
  const auto width = (u64(region.image_extent.width) + block.width - 1) / block.width;
  const auto height = (u64(region.image_extent.height) + block.height - 1) / block.height;
  const auto row_blocks = region.buffer_row_length == 0 ? width : region.buffer_row_length / block.width;
  const auto rows = region.buffer_image_height == 0 ? height : region.buffer_image_height / block.height;
  const auto pitch = checked_product(row_blocks, bytes);
  if (!pitch.has_value()) {
    return nullopt;
  }
  const auto slice = checked_product(*pitch, rows);
  if (!slice.has_value()) {
    return nullopt;
  }
  const auto preceding = checked_product(u64(layers) * region.image_extent.depth - 1, *slice);
  const auto last_rows = checked_product(height - 1, *pitch);
  const auto last_row = width * bytes;
  if (
    !preceding.has_value() || !last_rows.has_value() || *last_rows > ~0_u64 - last_row ||
    *preceding > ~0_u64 - *last_rows - last_row
  ) {
    return nullopt;
  }
  return *preceding + *last_rows + last_row;
}

static auto transfer_offset(const Buffer& buffer, u64 offset, u64 size) -> std::expected<u64, VkResult> {
  if (
    buffer.is_null() || offset > buffer.size || size > buffer.size - offset ||
    offset > std::numeric_limits<u64>::max() - buffer.offset ||
    size > std::numeric_limits<u64>::max() - buffer.offset - offset
  ) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }
  return buffer.offset + offset;
}

static auto mip_extent(const ImageAttachment& image, u32 mip) -> Extent3D {
  const auto level = u64(image.subresource_range.baseMipLevel) + mip;
  if (level >= 32) {
    return {1, 1, 1};
  }
  return {
    std::max(image.extent.width >> level, 1_u32),
    std::max(image.extent.height >> level, 1_u32),
    std::max(image.extent.depth >> level, 1_u32)
  };
}

static auto fits_image(const ImageAttachment& image, u32 mip, u32 layer, u32 layers, VkOffset3D offset, Extent3D extent)
  -> bool {
  const auto& range = image.subresource_range;
  if (
    image.image.is_null() || mip >= range.levelCount || layer >= range.layerCount || layers == 0 ||
    layers > range.layerCount - layer || u64(range.baseMipLevel) + mip >= image.image.mip_levels ||
    u64(range.baseArrayLayer) + layer + layers > image.image.array_layers || offset.x < 0 || offset.y < 0 ||
    offset.z < 0
  ) {
    return false;
  }
  const auto available = mip_extent(image, mip);
  const auto block = format_texel_block_extent(image.image.format);
  return u64(offset.x) + extent.width <= available.width && u64(offset.y) + extent.height <= available.height &&
         u64(offset.z) + extent.depth <= available.depth && offset.x % i32(block.width) == 0 &&
         offset.y % i32(block.height) == 0 &&
         (extent.width % block.width == 0 || u64(offset.x) + extent.width == available.width) &&
         (extent.height % block.height == 0 || u64(offset.y) + extent.height == available.height);
}

auto ImageCopy::is_empty(this const ImageCopy& self) -> bool {
  return self.extent.width == 0 || self.extent.height == 0 || self.extent.depth == 0;
}

auto record_copy_buffer(const CommandBuffer& command, const Buffer& src, const Buffer& dst, const BufferCopy& region)
  -> std::expected<void, VkResult> {
  ZoneScoped;
  if (region.src_offset > src.size || region.dst_offset > dst.size) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }
  const auto size = region.size == VK_WHOLE_SIZE ? std::min(src.size - region.src_offset, dst.size - region.dst_offset)
                                                 : region.size;
  if (size == 0) {
    return {};
  }
  const auto src_offset = transfer_offset(src, region.src_offset, size);
  const auto dst_offset = transfer_offset(dst, region.dst_offset, size);
  if (
    !src_offset || !dst_offset ||
    (src.handle == dst.handle && *src_offset < *dst_offset + size && *dst_offset < *src_offset + size)
  ) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }
  const auto copy = VkBufferCopy{.srcOffset = *src_offset, .dstOffset = *dst_offset, .size = size};
  command.fn->vkCmdCopyBuffer(command.handle, src.handle, dst.handle, 1, &copy);
  return {};
}

auto record_fill_buffer(const CommandBuffer& command, const Buffer& buffer, u32 data, u64 offset, u64 size)
  -> std::expected<void, VkResult> {
  ZoneScoped;
  if (offset > buffer.size) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }
  const auto bytes = size == VK_WHOLE_SIZE ? (buffer.size - offset) & ~3_u64 : size;
  if (bytes == 0) {
    return {};
  }
  const auto absolute = transfer_offset(buffer, offset, bytes);
  if (!absolute || *absolute % 4 != 0 || bytes % 4 != 0) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }
  command.fn->vkCmdFillBuffer(command.handle, buffer.handle, *absolute, bytes, data);
  return {};
}

auto record_update_buffer(const CommandBuffer& command, const Buffer& buffer, std::span<const u8> data, u64 offset)
  -> std::expected<void, VkResult> {
  ZoneScoped;
  if (data.empty()) {
    return {};
  }
  const auto absolute = transfer_offset(buffer, offset, data.size());
  if (!absolute || *absolute % 4 != 0 || data.size() % 4 != 0 || data.size() > 65536) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }
  command.fn->vkCmdUpdateBuffer(command.handle, buffer.handle, *absolute, data.size(), data.data());
  return {};
}

auto record_copy_image(
  const CommandBuffer& command,
  const ImageAttachment& src,
  const ImageAttachment& dst,
  option<ImageCopy> region,
  bool resolve
) -> std::expected<void, VkResult> {
  ZoneScoped;
  auto copy = region.value_or(ImageCopy{});
  if (!region.has_value()) {
    const auto src_extent = src.view_extent();
    const auto dst_extent = dst.view_extent();
    copy.extent = {
      std::min(src_extent.width, dst_extent.width),
      std::min(src_extent.height, dst_extent.height),
      std::min(src_extent.depth, dst_extent.depth)
    };
  }
  if (copy.is_empty()) {
    return {};
  }
  if (copy.src_layer >= src.subresource_range.layerCount || copy.dst_layer >= dst.subresource_range.layerCount) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }
  const auto layers = copy.layer_count == 0 ? std::min(
                                                src.subresource_range.layerCount - copy.src_layer,
                                                dst.subresource_range.layerCount - copy.dst_layer
                                              )
                                            : copy.layer_count;
  const auto src_block = format_texel_block_extent(src.image.format);
  const auto dst_block = format_texel_block_extent(dst.image.format);
  const auto src_aspect = src.subresource_range.aspectMask;
  const auto dst_aspect = dst.subresource_range.aspectMask;
  if (
    !fits_image(src, copy.src_mip, copy.src_layer, layers, copy.src_offset, copy.extent) ||
    !fits_image(dst, copy.dst_mip, copy.dst_layer, layers, copy.dst_offset, copy.extent) || src_aspect == 0 ||
    src_aspect != dst_aspect ||
    (!resolve && (src.samples != dst.samples || format_texel_block_size(src.image.format) == 0 ||
                  format_texel_block_size(src.image.format) != format_texel_block_size(dst.image.format) ||
                  src_block.width != dst_block.width || src_block.height != dst_block.height ||
                  ((src_aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0 &&
                   src.image.format != dst.image.format))) ||
    (resolve && (src.samples == Samples::e1 || dst.samples != Samples::e1 || src.image.format != dst.image.format ||
                 src.subresource_range.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
                 dst.subresource_range.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT))
  ) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }
  const auto src_layout = src.layout == VK_IMAGE_LAYOUT_GENERAL ? VK_IMAGE_LAYOUT_GENERAL
                                                                : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  const auto dst_layout = dst.layout == VK_IMAGE_LAYOUT_GENERAL ? VK_IMAGE_LAYOUT_GENERAL
                                                                : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  const auto source = VkImageSubresourceLayers{
    .aspectMask = src.subresource_range.aspectMask,
    .mipLevel = src.subresource_range.baseMipLevel + copy.src_mip,
    .baseArrayLayer = src.subresource_range.baseArrayLayer + copy.src_layer,
    .layerCount = layers
  };
  const auto destination = VkImageSubresourceLayers{
    .aspectMask = dst.subresource_range.aspectMask,
    .mipLevel = dst.subresource_range.baseMipLevel + copy.dst_mip,
    .baseArrayLayer = dst.subresource_range.baseArrayLayer + copy.dst_layer,
    .layerCount = layers
  };
  const auto same_subresource = src.image.handle == dst.image.handle && source.mipLevel == destination.mipLevel &&
                                u64(source.baseArrayLayer) < u64(destination.baseArrayLayer) + layers &&
                                u64(destination.baseArrayLayer) < u64(source.baseArrayLayer) + layers;
  if (
    same_subresource && (src_layout != VK_IMAGE_LAYOUT_GENERAL || dst_layout != VK_IMAGE_LAYOUT_GENERAL ||
                         (u64(copy.src_offset.x) < u64(copy.dst_offset.x) + copy.extent.width &&
                          u64(copy.dst_offset.x) < u64(copy.src_offset.x) + copy.extent.width &&
                          u64(copy.src_offset.y) < u64(copy.dst_offset.y) + copy.extent.height &&
                          u64(copy.dst_offset.y) < u64(copy.src_offset.y) + copy.extent.height &&
                          u64(copy.src_offset.z) < u64(copy.dst_offset.z) + copy.extent.depth &&
                          u64(copy.dst_offset.z) < u64(copy.src_offset.z) + copy.extent.depth))
  ) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }
  if (resolve) {
    const auto resolved = VkImageResolve{
      .srcSubresource = source,
      .srcOffset = copy.src_offset,
      .dstSubresource = destination,
      .dstOffset = copy.dst_offset,
      .extent = std::bit_cast<VkExtent3D>(copy.extent)
    };
    command.fn
      ->vkCmdResolveImage(command.handle, src.image.handle, src_layout, dst.image.handle, dst_layout, 1, &resolved);
  } else {
    const auto copied = VkImageCopy{
      .srcSubresource = source,
      .srcOffset = copy.src_offset,
      .dstSubresource = destination,
      .dstOffset = copy.dst_offset,
      .extent = std::bit_cast<VkExtent3D>(copy.extent)
    };
    command.fn->vkCmdCopyImage(command.handle, src.image.handle, src_layout, dst.image.handle, dst_layout, 1, &copied);
  }
  return {};
}

auto record_copy_buffer_image(
  const CommandBuffer& command,
  const Buffer& buffer,
  const ImageAttachment& image,
  option<BufferImageCopy> region,
  bool upload
) -> std::expected<void, VkResult> {
  ZoneScoped;
  const auto copied = region.value_or(BufferImageCopy::whole(image.view_extent()));
  if (copied.is_empty()) {
    return {};
  }
  const auto& range = image.subresource_range;
  if (copied.base_layer >= range.layerCount || image.samples != Samples::e1) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }
  const auto layers = copied.layer_count == 0 ? range.layerCount - copied.base_layer : copied.layer_count;
  const auto aspect = copied.aspect == 0 ? range.aspectMask : copied.aspect;
  const auto size = buffer_image_size(copied, image.image.format, aspect, layers);
  if (!size.has_value()) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }
  const auto offset = transfer_offset(buffer, copied.buffer_offset, *size);
  const auto block = format_texel_block_extent(image.image.format);
  const auto alignment = aspect == VK_IMAGE_ASPECT_DEPTH_BIT || aspect == VK_IMAGE_ASPECT_STENCIL_BIT
                           ? 4_u32
                           : format_texel_block_size(image.image.format);
  if (
    !offset || buffer.is_null() || aspect == 0 || (aspect & (aspect - 1)) != 0 ||
    (aspect & range.aspectMask) != aspect || *offset % alignment != 0 ||
    copied.image_offset.x % i32(block.width) != 0 || copied.image_offset.y % i32(block.height) != 0 ||
    !fits_image(image, copied.mip_level, copied.base_layer, layers, copied.image_offset, copied.image_extent) ||
    (copied.buffer_row_length != 0 && copied.buffer_row_length < copied.image_extent.width) ||
    (copied.buffer_image_height != 0 && copied.buffer_image_height < copied.image_extent.height)
  ) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }
  const auto copy = VkBufferImageCopy{
    .bufferOffset = *offset,
    .bufferRowLength = copied.buffer_row_length,
    .bufferImageHeight = copied.buffer_image_height,
    .imageSubresource =
      {.aspectMask = aspect,
       .mipLevel = range.baseMipLevel + copied.mip_level,
       .baseArrayLayer = range.baseArrayLayer + copied.base_layer,
       .layerCount = layers},
    .imageOffset = copied.image_offset,
    .imageExtent = std::bit_cast<VkExtent3D>(copied.image_extent)
  };
  const auto layout = image.layout == VK_IMAGE_LAYOUT_GENERAL ? VK_IMAGE_LAYOUT_GENERAL
                      : upload                                ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                                                              : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  if (upload) {
    command.fn->vkCmdCopyBufferToImage(command.handle, buffer.handle, image.image.handle, layout, 1, &copy);
  } else {
    command.fn->vkCmdCopyImageToBuffer(command.handle, image.image.handle, layout, buffer.handle, 1, &copy);
  }
  return {};
}
} // namespace ox::vir
