#include "Asset/Texture.hpp"

#include <ankerl/svector.h>
#include <basisu/encoder/basisu_enc.h>
#include <cstring>
#include <vuk/RenderGraph.hpp>
#include <vuk/runtime/vk/AllocatorHelpers.hpp>
#include <vuk/vsl/Core.hpp>

#include "Core/App.hpp"
#include "Memory/Stack.hpp"
#include "OS/File.hpp"
#include "Render/UploadBatch.hpp"
#include "Render/Utils/TextureFormat.hpp"

namespace ox {
struct ProcessedTexture {
  vuk::Format format = {};
  vuk::Extent3D extent = {};
  ankerl::svector<vuk::Unique<vuk::Buffer>, 12> buffers = {};
};

auto default_resource_name(OX_CALLSTACK) -> vuk::Name {
  ZoneScoped;
  memory::ScopedStack stack;

  auto file = LOC.file_name();
  return vuk::Name(stack.format("{0}:{1}", file, LOC.line()));
}

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
auto process_generic(std::span<const u8> bytes, bool is_srgb, vuk::Extent3D desired_extent = {~0_u32, ~0_u32, 1_u32})
  -> option<ProcessedTexture> {
  ZoneScoped;

  // the resampler's sRGB tables come from here; it returns straight away once they exist
  basisu::basisu_encoder_init();

  auto result = ProcessedTexture{};

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

  auto format = is_srgb ? vuk::Format::eR8G8B8A8Srgb : vuk::Format::eR8G8B8A8Unorm;
  auto extent = vuk::Extent3D{image.get_width(), image.get_height(), 1_u32};

  // color_rgba is laid out r, g, b, a, which is the RGBA8 the buffer holds
  const auto pixels_size = static_cast<usize>(image.get_width()) * image.get_height() * 4;
  auto& render_context = App::get_rendercontext();
  auto buffer = render_context.alloc_image_buffer(format, extent);
  std::memcpy(buffer->mapped_ptr, image.get_ptr(), ox::min(pixels_size, buffer->size));

  result.extent = extent;
  result.format = format;
  result.buffers.push_back(std::move(buffer));

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

  auto usage = info.usage | vuk::ImageUsageFlagBits::eTransferDst;
  if (info.level_count > 1) {
    usage |= vuk::ImageUsageFlagBits::eTransferSrc;
  }

  auto attachment = vuk::ImageAttachment{
    .image_flags = info.image_flags,
    .image_type = info.image_type,
    .tiling = info.tiling,
    .usage = usage,
    .extent = info.extent,
    .format = info.format,
    .sample_count = vuk::SampleCountFlagBits::e1,
    .image_view_flags = info.image_view_flags,
    .view_type = info.view_type,
    .components = info.components,
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

  auto image_view_id = render_context.allocate_image_view(attachment, info.batch);
  if (image_view_id == ImageViewID::Invalid) {
    render_context.destroy_image(image_id);
    return {};
  }

  auto image_view = render_context.image_view(image_view_id);
  attachment.image_view = image_view;

  auto sampler_id = render_context.allocate_sampler(info.sampler_info, info.batch);
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

  // Compressed sources are cooked into a TextureData by the resource compiler; whatever reaches here
  // is a plain image.
  auto desired_extent = vuk::Extent3D{
    .width = info.target_width.value_or(~0_u32),
    .height = info.target_height.value_or(~0_u32),
    .depth = 1_u32,
  };
  auto processed_texture = process_generic(bytes, info.is_srgb, desired_extent);
  if (!processed_texture) {
    OX_LOG_ERROR("Failed to create Texture({}). Couldn't process the source data.", LOC);
    return {};
  }

  const auto processed_level_count = static_cast<u32>(processed_texture->buffers.size());
  const auto requested_level_count = info.level_count.value_or(calculate_mip_count(processed_texture->extent));

  auto result = create({
    .format = processed_texture->format,
    .extent = processed_texture->extent,
    .level_count = ox::max(requested_level_count, processed_level_count),
    .usage = vuk::ImageUsageFlagBits::eSampled,
    .sampler_info = info.sampler_info,
    .batch = info.batch,
  });

  const auto generate_remaining_mips = requested_level_count > processed_level_count;
  result.upload_mips(processed_texture->buffers, vuk::eFragmentSampled, generate_remaining_mips, info.batch);

  if (info.batch) {
    info.batch->take_staging(processed_texture->buffers);
  }

  return result;
}

auto Texture::create(const TextureData& data, const TextureLoadInfo& info, OX_CALLSTACK) -> Texture {
  ZoneScoped;

  if (data.mips.empty()) {
    OX_LOG_ERROR("Failed to create Texture({}). Compiled texture '{}' has no mips.", LOC, data.name);
    return {};
  }

  auto result = create({
    .format = static_cast<vuk::Format>(data.vk_format),
    .extent = vuk::Extent3D{data.width, data.height, 1_u32},
    .layer_count = data.layer_count,
    .level_count = static_cast<u32>(data.mips.size()),
    .usage = vuk::ImageUsageFlagBits::eSampled,
    .components =
      {
        .r = static_cast<vuk::ComponentSwizzle>(data.components[0]),
        .g = static_cast<vuk::ComponentSwizzle>(data.components[1]),
        .b = static_cast<vuk::ComponentSwizzle>(data.components[2]),
        .a = static_cast<vuk::ComponentSwizzle>(data.components[3]),
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

  result.upload_mips(per_mip_pixels, vuk::eFragmentSampled, false, info.batch);

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
  bool generate_remaining,
  UploadBatch* batch
) -> void {
  ZoneScoped;

  auto& render_context = App::get_rendercontext();
  auto effective_level_count = std::min(static_cast<u32>(per_mip_pixels.size()), self.attachment.level_count);
  auto buffers = ankerl::svector<vuk::Unique<vuk::Buffer>, 12>();
  buffers.reserve(effective_level_count);

  auto base_extent = self.attachment.extent;
  for (auto level = 0_u32; level < effective_level_count; level++) {
    auto level_extent = vuk::Extent3D{
      .width = ox::max(base_extent.width >> level, 1_u32),
      .height = ox::max(base_extent.height >> level, 1_u32),
      .depth = 1,
    };

    auto mip_pixels = per_mip_pixels[level];
    auto buffer = render_context.alloc_image_buffer(self.attachment.format, level_extent);
    const auto safe_size_bytes = ox::min(buffer->size, mip_pixels.size_bytes());
    std::memcpy(buffer->mapped_ptr, mip_pixels.data(), safe_size_bytes);

    buffers.push_back(std::move(buffer));
  }

  self.upload_mips(buffers, release_as, generate_remaining, batch);

  if (batch) {
    batch->take_staging(buffers);
  }
}

auto Texture::upload_mips(
  this Texture& self,
  std::span<const vuk::Unique<vuk::Buffer>> per_mip_buffers,
  vuk::Access release_as,
  bool generate_remaining,
  UploadBatch* batch
) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  auto& render_context = App::get_rendercontext();
  auto effective_level_count = std::min(static_cast<u32>(per_mip_buffers.size()), self.attachment.level_count);
  auto should_generate = generate_remaining && self.attachment.level_count > effective_level_count;
  auto waits = stack.alloc<vuk::UntypedValue>(should_generate ? 1_u32 : effective_level_count + 1);

  auto attachment = vuk::discard_ia("upload mips", self.attachment);
  for (auto level = 0_u32; level < effective_level_count; level++) {
    auto mip_buffer = vuk::acquire_buf("mip staging", *per_mip_buffers[level], vuk::Access::eNone);
    auto uploaded = vuk::copy(std::move(mip_buffer), attachment.mip(level));
    if (!should_generate) {
      waits[level] = std::move(uploaded.as_released(release_as));
    }
  }

  if (should_generate) {
    auto base_mip = effective_level_count - 1;
    auto num_mips = self.attachment.level_count - effective_level_count;
    attachment = vuk::generate_mips(attachment, base_mip, num_mips);
  }

  waits[waits.size() - 1] = std::move(attachment.as_released(release_as));

  if (batch) {
    render_context.submit_multiple(waits);
    batch->add_upload(waits);
  } else {
    render_context.wait_on_multiple(waits);
  }
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

auto Texture::is_srgb() const -> bool { return to_unorm_format(attachment.format) != attachment.format; }

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
