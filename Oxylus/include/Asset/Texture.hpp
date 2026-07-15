#pragma once

#include <vuk/ImageAttachment.hpp>
#include <vuk/RenderGraph.hpp>
#include <vuk/Value.hpp>
#include <vuk/runtime/vk/PipelineInstance.hpp>
#include <vuk/runtime/vk/Query.hpp>

#include "Core/Types.hpp"
#include "Render/RenderContext.hpp"

using Preset = vuk::ImageAttachment::Preset;

namespace ox {
enum class TextureID : u64 { Invalid = std::numeric_limits<u64>::max() };

struct TextureCreateInfo {
  vuk::Format format = vuk::Format::eR8G8B8A8Srgb;
  vuk::Extent3D extent = {};
  u32 layer_count = 1;
  u32 level_count = 1;
  vuk::ImageCreateFlags image_flags = {};
  vuk::ImageType image_type = vuk::ImageType::e2D;
  vuk::ImageTiling tiling = vuk::ImageTiling::eOptimal;
  vuk::ImageUsageFlags usage = {};
  vuk::ImageViewCreateFlags image_view_flags = {};
  vuk::ImageViewType view_type = vuk::ImageViewType::eInfer;
  vuk::SamplerCreateInfo sampler_info = {
    .magFilter = vuk::Filter::eLinear,
    .minFilter = vuk::Filter::eLinear,
    .mipmapMode = vuk::SamplerMipmapMode::eLinear,
    .addressModeU = vuk::SamplerAddressMode::eRepeat,
    .addressModeV = vuk::SamplerAddressMode::eRepeat,
    .addressModeW = vuk::SamplerAddressMode::eRepeat,
  };
};

struct TextureView {
  vuk::ImageAttachment attachment = {};
  ImageViewID image_view_id = ImageViewID::Invalid;

  operator bool() const { return image_view_id != ImageViewID::Invalid; }

  auto acquire(this const TextureView& self, std::string_view name, vuk::Access last_access, OX_THISCALL)
    -> vuk::Value<vuk::ImageAttachment>;
  auto discard(this const TextureView& self, std::string_view name, OX_THISCALL) -> vuk::Value<vuk::ImageAttachment>;

  auto get_extent() const -> const vuk::Extent3D& { return attachment.extent; }
  auto get_format() const -> vuk::Format { return attachment.format; }
  auto get_view_id() const -> ImageViewID { return image_view_id; }
};

class Texture {
  vuk::ImageAttachment attachment = {};
  ImageID image_id = ImageID::Invalid;
  ImageViewID image_view_id = ImageViewID::Invalid;
  SamplerID sampler_id = SamplerID::Invalid;

  Texture(
    vuk::ImageAttachment attachment_, ImageID image_id_, ImageViewID image_view_id_, SamplerID sampler_id_
  ) noexcept;

  auto get_name(OX_THISCALL) const -> vuk::Name;

public:
  Texture() = default;
  ~Texture() noexcept;
  Texture(const Texture&) = delete;
  Texture& operator=(const Texture&) = delete;

  Texture(Texture&&) noexcept;
  Texture& operator=(Texture&&) noexcept;

  static auto create(const TextureCreateInfo& info) -> Texture;
  auto destroy(this Texture&) -> void;

  auto acquire(this const Texture&, std::string_view name, vuk::Access last_access, OX_THISCALL)
    -> vuk::Value<vuk::ImageAttachment>;
  auto discard(this const Texture&, std::string_view name, OX_THISCALL) -> vuk::Value<vuk::ImageAttachment>;

  auto set_name(std::string_view name, OX_THISCALL) -> void;

  auto view(this const Texture& self) -> TextureView;
  auto get_image() const -> const vuk::Image;
  auto get_image_view() const -> const vuk::ImageView;
  auto get_extent() const -> const vuk::Extent3D&;
  auto get_format() const -> vuk::Format;
  auto get_image_id() const -> ImageID;
  auto get_view_id() const -> ImageViewID;
  auto get_image_index() const -> u32;
  auto get_view_index() const -> u32;
  auto get_sampler_id() const -> SamplerID;
  auto get_sampler_index() const -> u32;

  operator bool() const { return image_id != ImageID::Invalid; }

  static auto calculate_mip_count(const vuk::Extent3D& extent) -> u32 {
    return static_cast<u32>(log2f(static_cast<f32>(std::max(std::max(extent.width, extent.height), extent.depth)))) + 1;
  }
};
} // namespace ox
