#pragma once

#include <glm/vec2.hpp>
#include <vir/Types.hpp>

#include "Asset/Texture.hpp"
#include "Utils/Log.hpp"

namespace ox {
// the image a panel renders a scene into and then shows through ImGui, which samples it by bindless
// index, replaced when the panel resizes, the old one retires once the frames reading it are done
struct SceneTarget {
  Texture texture = {};

  auto ensure(this SceneTarget& self, glm::uvec2 size, vir::Format format) -> const Texture& {
    const auto extent = vir::Extent3D{std::max(size.x, 1u), std::max(size.y, 1u), 1};
    const auto current = self.texture.get_extent();
    if (
      !self.texture || current.width != extent.width || current.height != extent.height ||
      self.texture.get_format() != format
    ) {
      self.texture = Texture::create({
        .format = format,
        .extent = extent,
        .usage = vir::ImageUsageFlags::ColorAttachment | vir::ImageUsageFlags::Sampled |
                 vir::ImageUsageFlags::TransferSrc | vir::ImageUsageFlags::TransferDst,
      });
      const auto cleared = self.texture.clear(
        vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 1.0f),
        vir::Access::FragmentSampled
      );
      if (!cleared) {
        OX_LOG_FATAL("failed to initialize the panel target ({})", std::to_underlying(cleared.error()));
      }
    }

    return self.texture;
  }
};
} // namespace ox
