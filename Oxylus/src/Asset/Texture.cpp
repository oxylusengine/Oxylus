#include "Asset/Texture.hpp"

#include <vuk/RenderGraph.hpp>
#include <vuk/runtime/vk/AllocatorHelpers.hpp>
#include <vuk/vsl/Core.hpp>

#include "Core/App.hpp"
#include "Memory/Stack.hpp"

namespace ox {
auto default_resource_name(OX_CALLSTACK) -> vuk::Name {
  ZoneScoped;
  memory::ScopedStack stack;

  auto file = LOC.file_name();
  return vuk::Name(stack.format("{0}:{1}", file, LOC.line()));
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

  auto& render_context = App::get_rendercontext();
  auto& allocator = render_context.superframe_allocator;
  auto effective_level_count = std::min(static_cast<u32>(per_mip_pixels.size()), self.attachment.level_count);

  auto base_extent = self.attachment.extent;
  auto attachment = vuk::discard_ia("upload mips", self.attachment);
  for (auto level = 0_u32; level < effective_level_count; level++) {
    auto level_extent = vuk::Extent3D{
      .width = base_extent.width >> level,
      .height = base_extent.height >> level,
      .depth = 1,
    };

    auto mip_pixels = per_mip_pixels[level];
    auto mip = attachment.mip(level);
    auto buffer = render_context.alloc_image_buffer(self.attachment.format, level_extent);
    std::memcpy(buffer->mapped_ptr, mip_pixels.data(), mip_pixels.size_bytes());
    vuk::copy(std::move(buffer), std::move(mip));
  }

  render_context.wait_on(std::move(attachment.as_released(release_as)));
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
