#pragma once

#include "Asset/Model.hpp"
#include "Asset/Texture.hpp"

namespace ox {
class VkContext;

class ThumbnailRenderer {
public:
  ThumbnailRenderer() = default;
  ~ThumbnailRenderer() = default;

  auto init(VkContext& vk_context) -> void;
  auto deinit() -> void;

  auto render(VkContext& vk_context, const vuk::Extent3D extent, vuk::Format format) -> vuk::Value<vuk::ImageAttachment>;

  auto reset() -> void;
  auto set_model(this ThumbnailRenderer& self, Model* mesh) -> void;
  auto set_name(this ThumbnailRenderer& self, const std::string& name) -> void;

  auto get_final_image() -> std::unique_ptr<Texture>& { return _final_image; }

private:
  std::unique_ptr<Texture> _final_image = nullptr;

  std::string thumbnail_name = "thumb";
  Model* mesh = nullptr;
};
} // namespace ox
