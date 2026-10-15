#pragma once

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <source_location>
#include <span>
#include <string_view>
#include <variant>

#include "Core/Base.hpp"
#include "Core/Option.hpp"
#include "Core/Types.hpp"
#include "Render/Fwd.hpp"
#include "Render/Graph/Access.hpp"
#include "Render/Graph/Resource.hpp"
#include "Render/Graph/Types.hpp"

namespace ox {
struct UploadBatch;
struct TextureData;

enum class TextureID : u64 { Invalid = std::numeric_limits<u64>::max() };

using TextureDataSource = std::variant<std::filesystem::path, std::span<const u8>>;

inline auto default_texture_sampler() -> vir::SamplerInfo {
  return vir::SamplerInfo::linear().with_address_mode(VK_SAMPLER_ADDRESS_MODE_REPEAT);
}

struct TextureCreateInfo {
  vir::Format format = vir::Format::RGBA8Srgb;
  vir::Extent3D extent = {};
  u32 layer_count = 1;
  u32 level_count = 1;
  vir::ImageCreateFlags image_flags = vir::ImageCreateFlags::None;
  vir::ImageType image_type = vir::ImageType::e2D;
  vir::ImageUsageFlags usage = vir::ImageUsageFlags::None;
  vir::ImageViewType view_type = vir::ImageViewType::e2D;
  vir::ComponentMapping components = {};
  vir::SamplerInfo sampler_info = default_texture_sampler();
  UploadBatch* batch = nullptr;
};

struct TextureLoadInfo {
  TextureDataSource source = {};
  option<u32> level_count = nullopt;
  bool is_srgb = true;
  option<u32> target_width = nullopt;
  option<u32> target_height = nullopt;
  vir::SamplerInfo sampler_info = default_texture_sampler();
  UploadBatch* batch = nullptr;
};

struct TextureView {
  vir::Extent3D extent = {};
  vir::Format format = vir::Format::Undefined;
  ImageViewID image_view_id = ImageViewID::Invalid;

  operator bool() const { return image_view_id != ImageViewID::Invalid; }

  auto get_extent() const -> vir::Extent3D { return extent; }
  auto get_format() const -> vir::Format { return format; }
  auto get_view_id() const -> ImageViewID { return image_view_id; }
};

class Texture {
  vir::ImageAttachment image = {};
  ImageID image_id = ImageID::Invalid;
  ImageViewID image_view_id = ImageViewID::Invalid;
  SamplerID sampler_id = SamplerID::Invalid;

  Texture(
    const vir::ImageAttachment& image_, ImageID image_id_, ImageViewID image_view_id_, SamplerID sampler_id_
  ) noexcept;

public:
  Texture() = default;
  ~Texture() noexcept;
  Texture(const Texture&) = delete;
  Texture& operator=(const Texture&) = delete;

  Texture(Texture&&) noexcept;
  Texture& operator=(Texture&&) noexcept;

  static auto create(const TextureCreateInfo& info, OX_THISCALL) -> Texture;
  static auto create(const TextureLoadInfo& info, OX_THISCALL) -> Texture;
  static auto create(const TextureData& data, const TextureLoadInfo& info, OX_THISCALL) -> Texture;
  auto destroy(this Texture&) -> void;

  // brings the texture into a module, waiting on whatever last read it at `rests_at`
  auto import(this const Texture& self, vir::Module& module, vir::Access rests_at) -> vir::ValueID;
  // the same, with its contents discarded, for a module that overwrites every texel it reads
  auto import_discarded(this const Texture& self, vir::Module& module, vir::Access rests_at) -> vir::ValueID;
  auto clear(this const Texture& self, vir::ClearValue value, vir::Access rests_at) -> std::expected<void, VkResult>;
  auto attachment_info(this const Texture& self) -> const vir::ImageAttachment& { return self.image; }

  auto upload_mips(
    this Texture&,
    std::span<const std::span<const u8>> per_mip_pixels,
    vir::Access release_as,
    bool generate_remaining = false,
    UploadBatch* batch = nullptr
  ) -> void;
  auto upload(this Texture&, std::span<const u8> pixels, vir::Access release_as, bool generate_remaining = false)
    -> void;

  auto set_name(std::string_view name, OX_THISCALL) -> void;

  auto view(this const Texture& self) -> TextureView;
  auto get_extent() const -> vir::Extent3D { return image.extent; }
  auto get_format() const -> vir::Format { return image.format; }
  auto get_level_count() const -> u32 { return image.subresource_range.levelCount; }
  auto is_srgb() const -> bool;
  auto get_image_id() const -> ImageID;
  auto get_view_id() const -> ImageViewID;
  auto get_image_index() const -> u32;
  auto get_view_index() const -> u32;
  auto get_sampler_id() const -> SamplerID;
  auto get_sampler_index() const -> u32;

  operator bool() const { return image_id != ImageID::Invalid; }

  static auto calculate_mip_count(const vir::Extent3D& extent) -> u32 {
    return static_cast<u32>(log2f(static_cast<f32>(std::max(std::max(extent.width, extent.height), extent.depth)))) + 1;
  }
};

} // namespace ox
