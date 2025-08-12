#pragma once

#include <vuk/ImageAttachment.hpp>
#include <vuk/RenderGraph.hpp>
#include <vuk/Value.hpp>
#include <vuk/runtime/vk/PipelineInstance.hpp>
#include <vuk/runtime/vk/Query.hpp>

#include "Core/Option.hpp"
#include "Core/Types.hpp"
#include "Render/Vulkan/VkContext.hpp"

using Preset = vuk::ImageAttachment::Preset;

namespace ox {
struct TextureLoadInfo {
  Preset preset = Preset::eMap2D;
  vuk::Format format = vuk::Format::eR8G8B8A8Srgb;
  enum class MimeType { Generic, KTX } mime = MimeType::Generic;
  option<std::vector<u8>> bytes = ox::nullopt;
  option<void*> loaded_data = ox::nullopt;
  option<vuk::Extent3D> extent = ox::nullopt;
};

enum class TextureID : u64 { Invalid = std::numeric_limits<u64>::max() };
class Texture {
public:
  Texture() = default;
  Texture(const std::string& name) : name_(name) {}

  Texture& operator=(Texture&& other) noexcept {
    if (this != &other) {
      image_id = std::move(other.image_id);
      image_view_id = std::move(other.image_view_id);
      attachment_ = std::move(other.attachment_);
      name_ = std::move(other.name_);
    }
    return *this;
  }

  Texture(Texture&& other) noexcept { *this = std::move(other); }

  ~Texture() = default;

  auto create(const std::string& path,
              const TextureLoadInfo& load_info,
              const std::source_location& loc = std::source_location::current()) -> void;

  auto destroy() -> void;

  auto attachment() const -> vuk::ImageAttachment { return attachment_; }
  auto acquire(vuk::Name name = {}, vuk::Access last_access = vuk::Access::eFragmentSampled) const
      -> vuk::Value<vuk::ImageAttachment>;
  auto discard(vuk::Name name = {}) const -> vuk::Value<vuk::ImageAttachment>;

  auto get_image() const -> const vuk::Image;
  auto get_view() const -> const vuk::ImageView;
  auto get_extent() const -> const vuk::Extent3D& { return attachment_.extent; }
  auto get_format() const -> vuk::Format { return attachment_.format; }

  auto get_name() -> const vuk::Name& { return name_; }
  auto set_name(std::string_view name, const std::source_location& loc = std::source_location::current()) -> void;

  auto get_image_id() const -> u64 { return (u64)image_id; }
  auto get_view_id() const -> u64 { return (u64)image_view_id; }

  operator bool() const { return image_id != ImageID::Invalid; }

  static auto load_stb_image(const std::string& filename,
                             uint32_t* width = nullptr,
                             uint32_t* height = nullptr,
                             uint32_t* bits = nullptr,
                             bool srgb = true) -> std::unique_ptr<u8[]>;

  static auto load_stb_image_from_memory(void* buffer,
                                         size_t len,
                                         uint32_t* width = nullptr,
                                         uint32_t* height = nullptr,
                                         uint32_t* bits = nullptr,
                                         bool flipY = false,
                                         bool srgb = true) -> std::unique_ptr<u8[]>;

  static auto get_magenta_texture(uint32_t width, uint32_t height, uint32_t channels) -> u8*;

  static auto convert_to_four_channels(uint32_t width, uint32_t height, const u8* three_channel_data) -> uint8_t*;

  static auto get_mip_count(const vuk::Extent3D extent) -> uint32_t {
    return static_cast<uint32_t>(
               log2f(static_cast<float>(std::max(std::max(extent.width, extent.height), extent.depth)))) +
           1;
  }

private:
  vuk::ImageAttachment attachment_ = {};
  ImageID image_id;
  ImageViewID image_view_id;
  vuk::Name name_ = {};
};
} // namespace ox
