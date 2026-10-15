#pragma once

#include "Render/Graph/Context.hpp"
#include "Render/Graph/Resource.hpp"

namespace ox::vir {
auto record_copy_buffer(const CommandBuffer& command, const Buffer& src, const Buffer& dst, const BufferCopy& region)
  -> std::expected<void, VkResult>;
auto record_fill_buffer(const CommandBuffer& command, const Buffer& buffer, u32 data, u64 offset, u64 size)
  -> std::expected<void, VkResult>;
auto record_update_buffer(const CommandBuffer& command, const Buffer& buffer, std::span<const u8> data, u64 offset)
  -> std::expected<void, VkResult>;
auto record_copy_image(
  const CommandBuffer& command,
  const ImageAttachment& src,
  const ImageAttachment& dst,
  option<ImageCopy> region,
  bool resolve
) -> std::expected<void, VkResult>;
auto record_copy_buffer_image(
  const CommandBuffer& command,
  const Buffer& buffer,
  const ImageAttachment& image,
  option<BufferImageCopy> region,
  bool upload
) -> std::expected<void, VkResult>;
} // namespace ox::vir
