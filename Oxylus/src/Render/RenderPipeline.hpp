#pragma once
#include <vuk/Value.hpp>

namespace ox {
class Scene;
class RenderPipeline {
public:
  struct RenderInfo {
    vuk::Extent3D extent;
    vuk::Format format;
    option<glm::uvec2> picking_texel = nullopt;
  };

  RenderPipeline() = default;
  virtual ~RenderPipeline() = default;

  virtual auto init(VkContext& vk_context) -> void = 0;
  virtual auto deinit() -> void = 0;

  [[nodiscard]]
  virtual auto on_render(VkContext& vk_context, const RenderInfo& render_info) -> vuk::Value<vuk::ImageAttachment> = 0;

  virtual auto on_update(Scene* scene) -> void = 0;
};
} // namespace ox
