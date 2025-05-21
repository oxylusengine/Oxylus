#pragma once

#include "Asset/Mesh.hpp"
#include "Render/RenderPipeline.hpp"
namespace ox {
class ThumbnailRenderPipeline : public RenderPipeline {
public:
  ThumbnailRenderPipeline() = default;
  ~ThumbnailRenderPipeline() override = default;

  auto init(VkContext& vk_context) -> void override;
  auto shutdown() -> void override;

  auto on_render(VkContext& vk_context, const RenderInfo& render_info) -> vuk::Value<vuk::ImageAttachment> override;

  auto on_update(Scene* scene) -> void override;

  auto set_mesh(this ThumbnailRenderPipeline& self, Mesh* mesh) -> void;
  auto set_name(this ThumbnailRenderPipeline& self, const std::string& name) -> void;

private:
  std::string thumbnail_name = "thumb";
  Mesh* mesh = nullptr;
};
} // namespace ox
