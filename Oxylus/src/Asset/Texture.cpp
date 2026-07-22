#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION

#include "Asset/Texture.hpp"

#include <ankerl/svector.h>
#include <ktx.h>
#include <stb_image.h>
#include <stb_image_resize2.h>
#include <stb_image_write.h>
#include <vuk/RenderGraph.hpp>
#include <vuk/runtime/vk/AllocatorHelpers.hpp>
#include <vuk/vsl/Core.hpp>

#include "Core/App.hpp"
#include "Memory/Stack.hpp"
#include "OS/File.hpp"
#include "Render/Utils/DDS.hpp"

namespace ox {
struct ProcessedTexture {
  vuk::Format format = {};
  vuk::Extent3D extent = {};
  ankerl::svector<vuk::Value<vuk::Buffer>, 12> buffers = {};
};

auto default_resource_name(OX_CALLSTACK) -> vuk::Name {
  ZoneScoped;
  memory::ScopedStack stack;

  auto file = LOC.file_name();
  return vuk::Name(stack.format("{0}:{1}", file, LOC.line()));
}

auto to_srgb_format(vuk::Format format) -> vuk::Format {
  switch (format) {
    case vuk::Format::eR8G8B8A8Unorm    : return vuk::Format::eR8G8B8A8Srgb;
    case vuk::Format::eB8G8R8A8Unorm    : return vuk::Format::eB8G8R8A8Srgb;
    case vuk::Format::eR8G8B8Unorm      : return vuk::Format::eR8G8B8Srgb;
    case vuk::Format::eB8G8R8Unorm      : return vuk::Format::eB8G8R8Srgb;
    case vuk::Format::eBc1RgbUnormBlock : return vuk::Format::eBc1RgbSrgbBlock;
    case vuk::Format::eBc1RgbaUnormBlock: return vuk::Format::eBc1RgbaSrgbBlock;
    case vuk::Format::eBc2UnormBlock    : return vuk::Format::eBc2SrgbBlock;
    case vuk::Format::eBc3UnormBlock    : return vuk::Format::eBc3SrgbBlock;
    case vuk::Format::eBc7UnormBlock    : return vuk::Format::eBc7SrgbBlock;
    default                             : return format;
  }
}

auto to_unorm_format(vuk::Format format) -> vuk::Format {
  switch (format) {
    case vuk::Format::eR8G8B8A8Srgb    : return vuk::Format::eR8G8B8A8Unorm;
    case vuk::Format::eB8G8R8A8Srgb    : return vuk::Format::eB8G8R8A8Unorm;
    case vuk::Format::eR8G8B8Srgb      : return vuk::Format::eR8G8B8Unorm;
    case vuk::Format::eB8G8R8Srgb      : return vuk::Format::eB8G8R8Unorm;
    case vuk::Format::eBc1RgbSrgbBlock : return vuk::Format::eBc1RgbUnormBlock;
    case vuk::Format::eBc1RgbaSrgbBlock: return vuk::Format::eBc1RgbaUnormBlock;
    case vuk::Format::eBc2SrgbBlock    : return vuk::Format::eBc2UnormBlock;
    case vuk::Format::eBc3SrgbBlock    : return vuk::Format::eBc3UnormBlock;
    case vuk::Format::eBc7SrgbBlock    : return vuk::Format::eBc7UnormBlock;
    default                            : return format;
  }
}

auto apply_srgb_preference(vuk::Format format, bool is_srgb) -> vuk::Format {
  return is_srgb ? to_srgb_format(format) : to_unorm_format(format);
}

auto detect_texture_source_type(std::span<const u8> bytes) -> TextureSourceType {
  if (bytes.size() >= 4 && std::memcmp(bytes.data(), "DDS ", 4) == 0) {
    return TextureSourceType::DDS;
  }

  constexpr static auto
    KTX2_MAGIC = std::array<u8, 12>{0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
  if (bytes.size() >= KTX2_MAGIC.size() && std::memcmp(bytes.data(), KTX2_MAGIC.data(), KTX2_MAGIC.size()) == 0) {
    return TextureSourceType::KTX;
  }

  return TextureSourceType::Generic;
}

auto process_generic(std::span<const u8> bytes, bool is_srgb, vuk::Extent3D desired_extent = {~0_u32, ~0_u32, 1_u32})
  -> option<ProcessedTexture> {
  ZoneScoped;

  auto result = ProcessedTexture{};

  int width = 0, height = 0, channels = 0;
  auto* raw_data =
    stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels, STBI_rgb_alpha);
  if (!raw_data) {
    return nullopt;
  }

  OX_DEFER(&) {
    if (raw_data)
      stbi_image_free(raw_data);
  };

  auto level_w = static_cast<u32>(width);
  auto level_h = static_cast<u32>(height);
  const auto source_size = static_cast<u64>(level_w) * level_h * 4;
  auto resized_pixels = std::vector<u8>();
  auto pixels = std::span<u8>(raw_data, raw_data + source_size);

  const auto target_w = ox::min(level_w, desired_extent.width);
  const auto target_h = ox::min(level_h, desired_extent.height);
  if (target_w != level_w || target_h != level_h) {
    resized_pixels.resize(static_cast<usize>(target_w) * target_h * 4);
    stbir_resize_uint8_linear(
      pixels.data(),
      static_cast<int>(level_w),
      static_cast<int>(level_h),
      0,
      resized_pixels.data(),
      static_cast<int>(target_w),
      static_cast<int>(target_h),
      0,
      STBIR_RGBA
    );

    pixels = resized_pixels;
    level_w = target_w;
    level_h = target_h;
  }

  auto format = is_srgb ? vuk::Format::eR8G8B8A8Srgb : vuk::Format::eR8G8B8A8Unorm;
  auto extent = vuk::Extent3D{level_w, level_h, 1_u32};

  auto& render_context = App::get_rendercontext();
  auto buffer = render_context.alloc_image_buffer(format, extent);
  auto safe_size_bytes = ox::min(pixels.size_bytes(), buffer->size);
  std::memcpy(buffer->mapped_ptr, pixels.data(), safe_size_bytes);

  result.extent = extent;
  result.format = format;
  result.buffers.push_back(std::move(buffer));

  return result;
}

auto process_dds(std::span<const u8> bytes) -> option<ProcessedTexture> {
  ZoneScoped;

  auto result = ProcessedTexture{};

  auto dds_image = dds::Image{};
  if (dds::readImage(const_cast<u8*>(bytes.data()), bytes.size(), &dds_image) != dds::ReadResult::Success) {
    return nullopt;
  }

  auto& render_context = App::get_rendercontext();

  result.format = static_cast<vuk::Format>(dds::getVulkanFormat(dds_image.format, dds_image.supportsAlpha));
  result.extent = vuk::Extent3D{dds_image.width, dds_image.height, 1_u32};

  for (auto level = 0_u32; level < dds_image.numMips; level++) {
    const auto& mip = dds_image.mipmaps[level];
    auto level_extent = vuk::Extent3D{
      .width = ox::max(result.extent.width >> level, 1_u32),
      .height = ox::max(result.extent.height >> level, 1_u32),
      .depth = 1_u32,
    };

    auto buffer = render_context.alloc_image_buffer(result.format, level_extent);
    auto safe_size_bytes = ox::min(buffer->size, mip.size_bytes());
    std::memcpy(buffer->mapped_ptr, mip.data(), safe_size_bytes);

    result.buffers.push_back(std::move(buffer));
  }

  return result;
}

auto process_ktx(std::span<const u8> bytes) -> option<ProcessedTexture> {
  ZoneScoped;

  auto result = ProcessedTexture{};

  ktxTexture2* ktx = nullptr;
  if (
    ktxTexture2_CreateFromMemory(bytes.data(), bytes.size(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &ktx) !=
    KTX_SUCCESS
  ) {
    return nullopt;
  }
  std::unique_ptr<ktxTexture2, decltype([](ktxTexture2* p) { ktxTexture_Destroy(ktxTexture(p)); })> owned(ktx);

  result.format = vuk::Format::eBc7UnormBlock;
  if (
    ktxTexture2_NeedsTranscoding(ktx) &&
    ktxTexture2_TranscodeBasis(ktx, KTX_TTF_BC7_RGBA, KTX_TF_HIGH_QUALITY) != KTX_SUCCESS
  ) {
    return nullopt;
  } else {
    result.format = static_cast<vuk::Format>(static_cast<VkFormat>(ktx->vkFormat));
  }

  result.extent = {ktx->baseWidth, ktx->baseHeight, ktx->baseDepth};

  auto& render_context = App::get_rendercontext();
  for (auto level = 0_u32; level < ktx->numLevels; level++) {
    auto level_extent = vuk::Extent3D{
      .width = ox::max(result.extent.width >> level, 1_u32),
      .height = ox::max(result.extent.height >> level, 1_u32),
      .depth = 1_u32,
    };
    auto buffer = render_context.alloc_image_buffer(result.format, level_extent);

    ktx_size_t offset = 0;
    if (ktxTexture_GetImageOffset(ktxTexture(ktx), level, 0, 0, &offset) == KTX_SUCCESS) {
      auto* level_data = ktxTexture_GetData(ktxTexture(ktx)) + offset;
      auto level_size = ktxTexture_GetImageSize(ktxTexture(ktx), level);
      auto safe_size_bytes = ox::min(buffer->size, level_size);
      std::memcpy(buffer->mapped_ptr, level_data, safe_size_bytes);
    }

    result.buffers.push_back(std::move(buffer));
  }

  return result;
}

Texture::Texture(
  vuk::ImageAttachment attachment_, ImageID image_id_, ImageViewID image_view_id_, SamplerID sampler_id_
) noexcept
    : attachment(attachment_),
      image_id(image_id_),
      image_view_id(image_view_id_),
      sampler_id(sampler_id_) {}

Texture::~Texture() noexcept { destroy(); }

Texture::Texture(Texture&& other) noexcept
    : attachment(other.attachment),
      image_id(other.image_id),
      image_view_id(other.image_view_id),
      sampler_id(other.sampler_id) {
  other.attachment = {};
  other.image_id = ImageID::Invalid;
  other.image_view_id = ImageViewID::Invalid;
  other.sampler_id = SamplerID::Invalid;
}

Texture& Texture::operator=(Texture&& other) noexcept {
  if (this != &other) {
    destroy();

    attachment = other.attachment;
    image_id = other.image_id;
    image_view_id = other.image_view_id;
    sampler_id = other.sampler_id;

    other.attachment = {};
    other.image_id = ImageID::Invalid;
    other.image_view_id = ImageViewID::Invalid;
    other.sampler_id = SamplerID::Invalid;
  }

  return *this;
}

auto Texture::get_name(OX_CALLSTACK) const -> vuk::Name { return default_resource_name(LOC); }

auto Texture::create(const TextureCreateInfo& info, OX_CALLSTACK) -> Texture {
  ZoneScoped;
  memory::ScopedStack stack;

  auto& render_context = App::get_rendercontext();

  auto attachment = vuk::ImageAttachment{
    .image_flags = info.image_flags,
    .image_type = info.image_type,
    .tiling = info.tiling,
    .usage = info.usage | vuk::ImageUsageFlagBits::eTransferDst,
    .extent = info.extent,
    .format = info.format,
    .sample_count = vuk::SampleCountFlagBits::e1,
    .image_view_flags = info.image_view_flags,
    .view_type = info.view_type,
    .base_level = 0,
    .level_count = info.level_count,
    .base_layer = 0,
    .layer_count = info.layer_count,
  };

  auto image_id = render_context.allocate_image(attachment);
  if (image_id == ImageID::Invalid) {
    return {};
  }

  auto image = render_context.image(image_id);
  attachment.image = image;

  auto image_view_id = render_context.allocate_image_view(attachment);
  if (image_view_id == ImageViewID::Invalid) {
    render_context.destroy_image(image_id);
    return {};
  }

  auto image_view = render_context.image_view(image_view_id);
  attachment.image_view = image_view;

  auto sampler_id = render_context.allocate_sampler(info.sampler_info);
  if (sampler_id == SamplerID::Invalid) {
    render_context.destroy_image(image_id);
    render_context.destroy_image_view(image_view_id);
    return {};
  }

#if OX_DEBUG
  auto debug_name = stack.format("{}:{}", LOC.file_name(), LOC.line());
  render_context.runtime->set_name(render_context.image(image_id).image, debug_name);
  render_context.runtime->set_name(render_context.image_view(image_view_id).payload, debug_name);
#endif

  return Texture(attachment, image_id, image_view_id, sampler_id);
}

auto Texture::create(const TextureLoadInfo& info, OX_CALLSTACK) -> Texture {
  ZoneScoped;

  auto result = Texture{};

  auto bytes = std::span<const u8>{};
  auto file = File();
  if (auto* path = std::get_if<std::filesystem::path>(&info.source)) {
    if (!std::filesystem::exists(*path)) {
      OX_LOG_ERROR("Failed to create Texture({}). Specified path '{}' does not exist.", LOC, *path);
      return {};
    }

    file = File(*path, FileAccess::Read);
    const auto* mapped_data = file.map();
    bytes = std::span{static_cast<const u8*>(mapped_data), file.size};
  } else if (auto* span = std::get_if<std::span<const u8>>(&info.source)) {
    bytes = *span;
  }

  auto processed_texture = option<ProcessedTexture>{nullopt};
  auto source_type = detect_texture_source_type(bytes);
  switch (source_type) {
    case TextureSourceType::Generic: {
      auto desired_extent = vuk::Extent3D{
        .width = info.target_width.value_or(~0_u32),
        .height = info.target_height.value_or(~0_u32),
        .depth = 1_u32,
      };
      processed_texture = process_generic(bytes, info.is_srgb, desired_extent);
    } break;
    case TextureSourceType::DDS: {
      processed_texture = process_dds(bytes);
    } break;
    case TextureSourceType::KTX: {
      processed_texture = process_ktx(bytes);
    } break;
  }

  if (!processed_texture) {
    OX_LOG_ERROR("Failed to create Texture({}). Couldn't process the source data.", LOC);
    return {};
  }

  processed_texture->format = apply_srgb_preference(processed_texture->format, info.is_srgb);

  auto processed_level_count = static_cast<u32>(processed_texture->buffers.size());
  result = create({
    .format = processed_texture->format,
    .extent = processed_texture->extent,
    .level_count = ox::max(info.level_count, processed_level_count),
    .usage = vuk::ImageUsageFlagBits::eSampled,
  });

  auto generate_remaining_mips = info.level_count > processed_level_count;
  result.upload_mips(processed_texture->buffers, vuk::eFragmentSampled, generate_remaining_mips);

  return result;
}

auto Texture::destroy(this Texture& self) -> void {
  ZoneScoped;

  auto& render_context = App::get_rendercontext();

  if (self.image_id != ImageID::Invalid)
    render_context.destroy_image(self.image_id);
  if (self.image_view_id != ImageViewID::Invalid)
    render_context.destroy_image_view(self.image_view_id);
  if (self.sampler_id != SamplerID::Invalid)
    render_context.destroy_sampler(self.sampler_id);

  self.attachment = {};
  self.image_id = ImageID::Invalid;
  self.image_view_id = ImageViewID::Invalid;
  self.sampler_id = SamplerID::Invalid;
}

auto Texture::acquire(this const Texture& self, std::string_view name, vuk::Access last_access, OX_CALLSTACK)
  -> vuk::Value<vuk::ImageAttachment> {
  ZoneScoped;

  return self.view().acquire(name, last_access, LOC);
}

auto Texture::discard(this const Texture& self, std::string_view name, OX_CALLSTACK)
  -> vuk::Value<vuk::ImageAttachment> {
  ZoneScoped;

  return self.view().discard(name, LOC);
}

auto Texture::upload_mips(
  this Texture& self,
  std::span<const std::span<const u8>> per_mip_pixels,
  vuk::Access release_as,
  bool generate_remaining
) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  auto& render_context = App::get_rendercontext();
  auto& allocator = render_context.superframe_allocator;
  auto effective_level_count = std::min(static_cast<u32>(per_mip_pixels.size()), self.attachment.level_count);
  auto buffers = stack.alloc<vuk::Value<vuk::Buffer>>(effective_level_count);

  auto base_extent = self.attachment.extent;
  for (auto level = 0_u32; level < effective_level_count; level++) {
    auto level_extent = vuk::Extent3D{
      .width = base_extent.width >> level,
      .height = base_extent.height >> level,
      .depth = 1,
    };

    auto mip_pixels = per_mip_pixels[level];
    auto buffer = render_context.alloc_image_buffer(self.attachment.format, level_extent);
    std::memcpy(buffer->mapped_ptr, mip_pixels.data(), mip_pixels.size_bytes());

    buffers[level] = std::move(buffer);
  }

  self.upload_mips(buffers, release_as, generate_remaining);
}

auto Texture::upload_mips(
  this Texture& self,
  std::span<vuk::Value<vuk::Buffer>> per_mip_buffers,
  vuk::Access release_as,
  bool generate_remaining
) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  auto& render_context = App::get_rendercontext();
  auto effective_level_count = std::min(static_cast<u32>(per_mip_buffers.size()), self.attachment.level_count);
  auto waits = stack.alloc<vuk::UntypedValue>(effective_level_count + 1);

  auto base_extent = self.attachment.extent;
  auto attachment = vuk::discard_ia("upload mips", self.attachment);
  for (auto level = 0_u32; level < effective_level_count; level++) {
    auto level_extent = vuk::Extent3D{
      .width = base_extent.width >> level,
      .height = base_extent.height >> level,
      .depth = 1,
    };

    auto mip_buffer = std::move(per_mip_buffers[level]);
    auto mip = attachment.mip(level);
    waits[level] = std::move(vuk::copy(std::move(mip_buffer), std::move(mip)).as_released(release_as));
  }

  waits[effective_level_count] = std::move(attachment.as_released(release_as));

  render_context.wait_on_multiple(waits);
}

auto Texture::upload(this Texture& self, std::span<const u8> pixels, vuk::Access release_as, bool generate_remaining)
  -> void {
  ZoneScoped;

  const std::span<const u8> mip0_pixels[] = {pixels};
  self.upload_mips(std::span(mip0_pixels), release_as, generate_remaining);
}

auto Texture::set_name(std::string_view name, OX_CALLSTACK) -> void {
  ZoneScoped;

  auto& render_context = App::get_rendercontext();
  auto new_name = vuk::Name{name};
  if (name.empty()) {
    new_name = get_name();
  }

  render_context.runtime->set_name(render_context.image(image_id).image, new_name);
  render_context.runtime->set_name(render_context.image_view(image_view_id).payload, new_name);
}

auto Texture::view(this const Texture& self) -> TextureView { return {self.attachment, self.image_view_id}; }

auto Texture::get_image() const -> const vuk::Image {
  ZoneScoped;

  auto& render_context = App::get_rendercontext();

  return render_context.image(image_id);
}

auto Texture::get_image_view() const -> const vuk::ImageView {
  ZoneScoped;

  auto& render_context = App::get_rendercontext();

  return render_context.image_view(image_view_id);
}

auto Texture::get_extent() const -> const vuk::Extent3D& { return attachment.extent; }

auto Texture::get_format() const -> vuk::Format { return attachment.format; }

auto Texture::get_image_id() const -> ImageID { return image_id; }

auto Texture::get_view_id() const -> ImageViewID { return image_view_id; }

auto Texture::get_image_index() const -> u32 { return SlotMap_decode_id(image_id).index; }

auto Texture::get_view_index() const -> u32 { return SlotMap_decode_id(image_view_id).index; }

auto Texture::get_sampler_id() const -> SamplerID { return sampler_id; }

auto Texture::get_sampler_index() const -> u32 { return SlotMap_decode_id(sampler_id).index; }

auto TextureView::acquire(this const TextureView& self, std::string_view name, vuk::Access last_access, OX_CALLSTACK)
  -> vuk::Value<vuk::ImageAttachment> {
  ZoneScoped;

  return vuk::acquire_ia(
    name.empty() ? default_resource_name(LOC) : vuk::Name{name},
    self.attachment,
    last_access,
    LOC
  );
}

auto TextureView::discard(this const TextureView& self, std::string_view name, OX_CALLSTACK)
  -> vuk::Value<vuk::ImageAttachment> {
  ZoneScoped;

  return vuk::discard_ia(name.empty() ? default_resource_name(LOC) : vuk::Name{name}, self.attachment, LOC);
}

} // namespace ox
