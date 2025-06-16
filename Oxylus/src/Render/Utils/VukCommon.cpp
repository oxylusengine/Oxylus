#include "VukCommon.hpp"

#include <vuk/RenderGraph.hpp>
#include <vuk/runtime/CommandBuffer.hpp>

namespace vuk {
vuk::Value<vuk::ImageAttachment> generate_mips(vuk::Value<vuk::ImageAttachment> image,
                                               uint32_t mip_count) {
  auto ia = image.mip(0);

  for (uint32_t mip_level = 1; mip_level < mip_count; mip_level++) {
    auto pass = vuk::make_pass(fmt::format("mip_{}", mip_level).c_str(),
                               [mip_level](vuk::CommandBuffer& command_buffer,
                                           VUK_IA(vuk::eTransferRead) src,
                                           VUK_IA(vuk::eTransferWrite) dst) {
      ImageBlit blit;
      const auto extent = src->extent;

      blit.srcSubresource.aspectMask = format_to_aspect(src->format);
      blit.srcSubresource.baseArrayLayer = src->base_layer;
      blit.srcSubresource.layerCount = src->layer_count;
      blit.srcSubresource.mipLevel = mip_level - 1;
      blit.srcOffsets[0] = Offset3D{0};
      blit.srcOffsets[1] = Offset3D{std::max(static_cast<int32_t>(extent.width) >> (mip_level - 1), 1),
                                    std::max(static_cast<int32_t>(extent.height) >> (mip_level - 1), 1),
                                    std::max(static_cast<int32_t>(extent.depth) >> (mip_level - 1), 1)};
      blit.dstSubresource = blit.srcSubresource;
      blit.dstSubresource.mipLevel = mip_level;
      blit.dstOffsets[0] = Offset3D{0};
      blit.dstOffsets[1] = Offset3D{std::max(static_cast<int32_t>(extent.width) >> (mip_level), 1),
                                    std::max(static_cast<int32_t>(extent.height) >> (mip_level), 1),
                                    std::max(static_cast<int32_t>(extent.depth) >> (mip_level), 1)};
      command_buffer.blit_image(src, dst, blit, Filter::eLinear);

      return dst;
    });

    ia = pass(std::move(ia), image.mip(mip_level));
  }

  return image;
}
} // namespace vuk
