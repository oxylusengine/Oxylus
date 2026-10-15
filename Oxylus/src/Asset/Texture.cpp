#include "Asset/Texture.hpp"

#include <ankerl/svector.h>
#include <basisu/encoder/basisu_enc.h>
#include <cstring>

#include "Asset/AssetFile.hpp"
#include "Core/App.hpp"
#include "Memory/SlotMap.hpp"
#include "Memory/Stack.hpp"
#include "OS/File.hpp"
#include "Render/Graph/Allocator.hpp"
#include "Render/Graph/Module.hpp"
#include "Render/Graph/Types.hpp"
#include "Render/RenderContext.hpp"
#include "Render/UploadBatch.hpp"
#include "Render/Utils/TextureFormat.hpp"
#include "Utils/Log.hpp"

namespace ox {
struct ProcessedTexture {
  vir::Format format = vir::Format::Undefined;
  vir::Extent3D extent = {};
  std::vector<u8> pixels = {};
};

// the signatures basisu's in-memory loaders understand
static auto decode_image(std::span<const u8> bytes, basisu::image& image) -> bool {
  constexpr static u8 PNG[] = {0x89, 'P', 'N', 'G'};
  constexpr static u8 JPEG[] = {0xFF, 0xD8, 0xFF};
  constexpr static u8 QOI[] = {'q', 'o', 'i', 'f'};
  const auto starts_with = [bytes](std::span<const u8> signature) {
    return bytes.size() >= signature.size() && std::memcmp(bytes.data(), signature.data(), signature.size()) == 0;
  };

  if (starts_with(PNG)) {
    return basisu::load_png(bytes.data(), bytes.size(), image);
  }
  if (starts_with(JPEG)) {
    return basisu::load_jpg(bytes.data(), bytes.size(), image);
  }
  if (starts_with(QOI)) {
    return basisu::load_qoi(bytes.data(), bytes.size(), image);
  }

  return false;
}

// a PNG, JPEG or QOI handed over at runtime (UI images, sources nothing cooked), decoded by the same library the cook
// uses
static auto process_generic(std::span<const u8> bytes, bool is_srgb, vir::Extent2D desired_extent)
  -> option<ProcessedTexture> {
  ZoneScoped;

  // the resampler's sRGB tables come from here; it returns straight away once they exist
  basisu::basisu_encoder_init();

  auto image = basisu::image{};
  if (!decode_image(bytes, image)) {
    return nullopt;
  }

  const auto target_w = ox::min(image.get_width(), desired_extent.width);
  const auto target_h = ox::min(image.get_height(), desired_extent.height);
  if (target_w != image.get_width() || target_h != image.get_height()) {
    auto resized = basisu::image(target_w, target_h);
    if (!basisu::image_resample(image, resized, is_srgb)) {
      return nullopt;
    }

    image = std::move(resized);
  }

  // color_rgba is laid out r, g, b, a, which is the RGBA8 the upload expects
  const auto pixels_size = static_cast<usize>(image.get_width()) * image.get_height() * 4;
  const auto* pixels = reinterpret_cast<const u8*>(image.get_ptr());
  return ProcessedTexture{
    .format = is_srgb ? vir::Format::RGBA8Srgb : vir::Format::RGBA8Unorm,
    .extent = {image.get_width(), image.get_height(), 1_u32},
    .pixels = {pixels, pixels + pixels_size},
  };
}

Texture::Texture(
  const vir::ImageAttachment& image_, ImageID image_id_, ImageViewID image_view_id_, SamplerID sampler_id_
) noexcept
    : image(image_),
      image_id(image_id_),
      image_view_id(image_view_id_),
      sampler_id(sampler_id_) {}

Texture::~Texture() noexcept { destroy(); }

Texture::Texture(Texture&& other) noexcept
    : image(other.image),
      image_id(other.image_id),
      image_view_id(other.image_view_id),
      sampler_id(other.sampler_id) {
  other.image = {};
  other.image_id = ImageID::Invalid;
  other.image_view_id = ImageViewID::Invalid;
  other.sampler_id = SamplerID::Invalid;
}

Texture& Texture::operator=(Texture&& other) noexcept {
  if (this != &other) {
    destroy();

    image = other.image;
    image_id = other.image_id;
    image_view_id = other.image_view_id;
    sampler_id = other.sampler_id;

    other.image = {};
    other.image_id = ImageID::Invalid;
    other.image_view_id = ImageViewID::Invalid;
    other.sampler_id = SamplerID::Invalid;
  }

  return *this;
}

auto Texture::create(const TextureCreateInfo& info, OX_CALLSTACK) -> Texture {
  ZoneScoped;
  memory::ScopedStack stack;

  auto& render_context = App::get_rendercontext();

  auto usage = info.usage | vir::ImageUsageFlags::TransferDst;
  if (info.level_count > 1) {
    usage |= vir::ImageUsageFlags::TransferSrc;
  }

  const auto debug_name = stack.format("{}:{}", LOC.file_name(), LOC.line());
  const auto image_id = render_context.allocate_image({
    .extent = info.extent,
    .format = info.format,
    .usage = usage,
    .image_type = info.image_type,
    .view_type = info.view_type,
    .flags = info.image_flags,
    .mip_levels = info.level_count,
    .array_layers = info.layer_count,
    .name = std::string(debug_name),
  });
  if (!image_id) {
    OX_LOG_ERROR(
      "Failed to create Texture({}), image allocation failed ({}).",
      LOC,
      std::to_underlying(image_id.error())
    );
    return {};
  }

  const auto image = render_context.image(*image_id);
  const auto range = VkImageSubresourceRange{
    .aspectMask = vir::aspect_mask(info.format),
    .baseMipLevel = 0,
    .levelCount = info.level_count,
    .baseArrayLayer = 0,
    .layerCount = info.layer_count,
  };
  const auto image_view_id = render_context.allocate_image_view(
    {
      .image = image.handle,
      .format = info.format,
      .view_type = info.view_type,
      .range = range,
      .components = info.components,
      .usage = usage,
      .name = std::string(debug_name),
    },
    info.batch == nullptr
  );
  if (!image_view_id) {
    render_context.destroy_image(*image_id);
    return {};
  }

  const auto sampler_id = render_context.allocate_sampler(info.sampler_info, info.batch == nullptr);
  if (!sampler_id) {
    render_context.destroy_image_view(*image_view_id);
    render_context.destroy_image(*image_id);
    return {};
  }

  const auto view = render_context.image_view(*image_view_id).handle;
  if (info.batch) {
    const auto view_index = SlotMap_decode_id(*image_view_id).index;
    if (vir::intersects(usage, vir::ImageUsageFlags::Sampled)) {
      info.batch->add_descriptor_write({
        .binding = DescriptorTable_SampledImageIndex,
        .array_element = view_index,
        .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        .image_info = {VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL},
      });
    }

    if (vir::intersects(usage, vir::ImageUsageFlags::Storage)) {
      info.batch->add_descriptor_write({
        .binding = DescriptorTable_StorageImageIndex,
        .array_element = view_index,
        .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .image_info = {VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL},
      });
    }

    info.batch->add_descriptor_write({
      .binding = DescriptorTable_SamplerIndex,
      .array_element = SlotMap_decode_id(*sampler_id).index,
      .type = VK_DESCRIPTOR_TYPE_SAMPLER,
      .image_info = {render_context.sampler(*sampler_id).handle, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED},
    });
  }

  auto attachment = vir::ImageAttachment::from_image(image, VK_IMAGE_LAYOUT_UNDEFINED);
  attachment.image_view = view;
  attachment.view_type = info.view_type;
  attachment.subresource_range = range;
  auto texture = Texture(attachment, *image_id, *image_view_id, *sampler_id);
  return texture;
}

auto Texture::create(const TextureLoadInfo& info, OX_CALLSTACK) -> Texture {
  ZoneScoped;

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

  // compressed sources are cooked into a TextureData by the resource compiler, whatever reaches here
  // is a plain image
  const auto desired_extent = vir::Extent2D{info.target_width.value_or(~0_u32), info.target_height.value_or(~0_u32)};
  auto processed_texture = process_generic(bytes, info.is_srgb, desired_extent);
  if (!processed_texture) {
    OX_LOG_ERROR("Failed to create Texture({}). Couldn't process the source data.", LOC);
    return {};
  }

  const auto requested_level_count = info.level_count.value_or(calculate_mip_count(processed_texture->extent));
  auto result = create({
    .format = processed_texture->format,
    .extent = processed_texture->extent,
    .level_count = ox::max(requested_level_count, 1_u32),
    .usage = vir::ImageUsageFlags::Sampled,
    .sampler_info = info.sampler_info,
    .batch = info.batch,
  });
  if (!result) {
    return {};
  }

  const std::span<const u8> mip0[] = {processed_texture->pixels};
  result.upload_mips(mip0, vir::Access::FragmentSampled, requested_level_count > 1, info.batch);

  return result;
}

auto Texture::create(const TextureData& data, const TextureLoadInfo& info, OX_CALLSTACK) -> Texture {
  ZoneScoped;

  if (data.mips.empty()) {
    OX_LOG_ERROR("Failed to create Texture({}). Compiled texture '{}' has no mips.", LOC, data.name);
    return {};
  }

  auto result = create({
    .format = static_cast<vir::Format>(data.vk_format),
    .extent = {data.width, data.height, 1_u32},
    .layer_count = data.layer_count,
    .level_count = static_cast<u32>(data.mips.size()),
    .usage = vir::ImageUsageFlags::Sampled,
    .components =
      {
        .r = static_cast<vir::ComponentSwizzle>(data.components[0]),
        .g = static_cast<vir::ComponentSwizzle>(data.components[1]),
        .b = static_cast<vir::ComponentSwizzle>(data.components[2]),
        .a = static_cast<vir::ComponentSwizzle>(data.components[3]),
      },
    .sampler_info = info.sampler_info,
    .batch = info.batch,
  });
  if (!result) {
    return {};
  }

  auto per_mip_pixels = ankerl::svector<std::span<const u8>, 16>();
  per_mip_pixels.reserve(data.mips.size());
  for (const auto& mip : data.mips) {
    per_mip_pixels.emplace_back(mip.pixels);
  }

  result.upload_mips(per_mip_pixels, vir::Access::FragmentSampled, false, info.batch);

  return result;
}

auto Texture::destroy(this Texture& self) -> void {
  ZoneScoped;

  // never created, so there is nothing to free and possibly no renderer to ask
  if (
    self.image_id == ImageID::Invalid && self.image_view_id == ImageViewID::Invalid &&
    self.sampler_id == SamplerID::Invalid
  ) {
    return;
  }

  // retired, so frames still in flight keep sampling it
  auto& render_context = App::get_rendercontext();
  if (self.image_view_id != ImageViewID::Invalid)
    render_context.destroy_image_view(self.image_view_id);
  if (self.image_id != ImageID::Invalid)
    render_context.destroy_image(self.image_id);
  if (self.sampler_id != SamplerID::Invalid)
    render_context.destroy_sampler(self.sampler_id);

  self.image = {};
  self.image_id = ImageID::Invalid;
  self.image_view_id = ImageViewID::Invalid;
  self.sampler_id = SamplerID::Invalid;
}

auto Texture::import(this const Texture& self, vir::Module& module, vir::Access rests_at) -> vir::ValueID {
  auto resting = self.image;
  resting.layout = vir::to_image_layout(rests_at);
  return module.import_attachment(resting, rests_at);
}

auto Texture::import_discarded(this const Texture& self, vir::Module& module, vir::Access rests_at) -> vir::ValueID {
  auto discarded = self.image;
  discarded.layout = VK_IMAGE_LAYOUT_UNDEFINED;
  return module.import_attachment(discarded, rests_at);
}

auto Texture::clear(this const Texture& self, vir::ClearValue value, vir::Access rests_at)
  -> std::expected<void, VkResult> {
  ZoneScoped;
  if (!self) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }

  auto module = vir::Module{};
  const auto cleared = module.clear(self.import_discarded(module, vir::Access::None), value);
  module.export_to(cleared, rests_at);
  const auto ticket = App::get_rendercontext().execute_blocking(std::move(module));
  if (!ticket) {
    return std::unexpected(ticket.error());
  }
  return {};
}

auto Texture::upload_mips(
  this Texture& self,
  std::span<const std::span<const u8>> per_mip_pixels,
  vir::Access release_as,
  bool generate_remaining,
  UploadBatch* batch
) -> void {
  ZoneScoped;

  auto& render_context = App::get_rendercontext();
  const auto level_count = self.image.subresource_range.levelCount;
  const auto uploaded_count = std::min(static_cast<u32>(per_mip_pixels.size()), level_count);
  if (uploaded_count == 0) {
    return;
  }

  const auto should_generate = generate_remaining && level_count > uploaded_count;

  // every mip below is rewritten, so the old contents go, but whatever last read them is waited on
  auto module = vir::Module{};
  auto current = self.import_discarded(module, release_as);
  auto staging = std::vector<vir::Buffer>{};
  staging.reserve(uploaded_count);
  for (auto level = 0_u32; level < uploaded_count; level++) {
    const auto pixels = per_mip_pixels[level];
    if (pixels.empty()) {
      continue;
    }

    auto buffer = render_context.allocate_staging(pixels);
    if (!buffer) {
      OX_LOG_ERROR("Failed to upload texture mip {}, staging allocation failed.", level);
      for (const auto& allocated : staging) {
        render_context.persistent->deallocate_buffer(allocated);
      }

      return;
    }

    staging.push_back(*buffer);
    const auto source = module.import_buffer(*buffer, vir::Access::HostWrite);
    current = module.copy_buffer_to_image(source, module.mip(current, level));
  }

  if (should_generate) {
    current = module.generate_mips(current, uploaded_count - 1, level_count - uploaded_count + 1);
  } else {
    current = module.whole(current);
  }

  module.export_to(current, release_as);
  if (auto submitted = render_context.submit_upload(std::move(module), std::move(staging), batch); !submitted) {
    OX_LOG_ERROR("Failed to upload texture ({}).", std::to_underlying(submitted.error()));
  }
}

auto Texture::upload(this Texture& self, std::span<const u8> pixels, vir::Access release_as, bool generate_remaining)
  -> void {
  ZoneScoped;

  const std::span<const u8> mip0_pixels[] = {pixels};
  self.upload_mips(std::span(mip0_pixels), release_as, generate_remaining);
}

auto Texture::set_name(std::string_view name, OX_CALLSTACK) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  auto& render_context = App::get_rendercontext();
  const auto new_name = name.empty() ? stack.format("{}:{}", LOC.file_name(), LOC.line()) : name;
  render_context.context->set_name(VK_OBJECT_TYPE_IMAGE, image.image.handle, new_name);
  render_context.context->set_name(VK_OBJECT_TYPE_IMAGE_VIEW, image.image_view, new_name);
}

auto Texture::view(this const Texture& self) -> TextureView {
  return {.extent = self.image.extent, .format = self.image.format, .image_view_id = self.image_view_id};
}

auto Texture::is_srgb() const -> bool { return to_unorm_format(image.format) != image.format; }

auto Texture::get_image_id() const -> ImageID { return image_id; }

auto Texture::get_view_id() const -> ImageViewID { return image_view_id; }

auto Texture::get_image_index() const -> u32 { return SlotMap_decode_id(image_id).index; }

auto Texture::get_view_index() const -> u32 { return SlotMap_decode_id(image_view_id).index; }

auto Texture::get_sampler_id() const -> SamplerID { return sampler_id; }

auto Texture::get_sampler_index() const -> u32 { return SlotMap_decode_id(sampler_id).index; }

} // namespace ox
