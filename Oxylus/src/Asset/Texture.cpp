#define STB_IMAGE_IMPLEMENTATION

#include "Asset/Texture.hpp"

#include <ktx.h>
#include <stb_image.h>
#include <vuk/RenderGraph.hpp>
#include <vuk/runtime/vk/AllocatorHelpers.hpp>
#include <vuk/vsl/Core.hpp>

#include "Core/App.hpp"
#include "Core/FileSystem.hpp"
#include "Render/RendererCommon.hpp"
#include "Render/Utils/VukCommon.hpp"

namespace ox {
void Texture::create(const std::string& path, const TextureLoadInfo& load_info, const std::source_location& loc) {
  ZoneScoped;

  auto& vk_context = App::get_vkcontext();
  auto& allocator = vk_context.superframe_allocator;

  const auto is_generic = load_info.mime == TextureLoadInfo::MimeType::Generic;

  std::unique_ptr<u8[]> stb_data = nullptr;
  std::unique_ptr<ktxTexture2, decltype([](ktxTexture2* p) { ktxTexture_Destroy(ktxTexture(p)); })> ktx_texture = {};

  auto extent = load_info.extent.value_or(vuk::Extent3D{0, 0, 1});
  u32 chans = {};
  vuk::Format format = load_info.format;

  if (is_generic) {
    if (!path.empty()) {
      stb_data = load_stb_image(path, &extent.width, &extent.height, &chans);
    } else if (load_info.bytes.has_value()) {
      stb_data = load_stb_image_from_memory((void*)load_info.bytes->data(), //
                                            load_info.bytes->size(),
                                            &extent.width,
                                            &extent.height,
                                            &chans);
    }
  } else if (!is_generic) {
    ktxTexture2* ktx{};
    if (path.empty()) {
      OX_CHECK_EQ(load_info.bytes.has_value(), true);
      if (const auto result = ktxTexture2_CreateFromMemory(load_info.bytes->data(), //
                                                           load_info.bytes->size(),
                                                           KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
                                                           &ktx);
          result != KTX_SUCCESS) {
        OX_LOG_ERROR("Couldn't load KTX2 file {}", ktxErrorString(result));
      }
    } else {
      if (const auto result = ktxTexture2_CreateFromNamedFile(
              path.c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &ktx);
          result != KTX_SUCCESS) {
        OX_LOG_ERROR("Couldn't load KTX2 file {}", ktxErrorString(result));
      }
    }

    OX_CHECK_NULL(ktx);

    auto format_ktx = vuk::Format::eBc7UnormBlock;
    constexpr ktx_transcode_fmt_e ktx_transcode_format = KTX_TTF_BC7_RGBA;

    // If the image needs is in a supercompressed encoding, transcode it to a desired format
    if (ktxTexture2_NeedsTranscoding(ktx)) {
      ZoneNamedN(z, "Transcode KTX 2 Texture", true);
      if (const auto result = ktxTexture2_TranscodeBasis(ktx, ktx_transcode_format, KTX_TF_HIGH_QUALITY);
          result != KTX_SUCCESS) {
        OX_LOG_ERROR("Couldn't transcode KTX2 file {}", ktxErrorString(result));
      }
    } else {
      // Use the format that the image is already in
      format_ktx = static_cast<vuk::Format>(static_cast<VkFormat>(ktx->vkFormat));
    }

    extent.width = ktx->baseWidth;
    extent.height = ktx->baseHeight;
    format = format_ktx;

    ktx_texture.reset(ktx);
  }

  OX_CHECK_NE(extent.height, 0u, "Height can't be 0!");
  OX_CHECK_NE(extent.width, 0u, "Width can't be 0!");
  OX_CHECK_NE(extent.depth, 0u, "Depth can't be 0!");

  auto ia = vuk::ImageAttachment::from_preset(
      load_info.preset, format, {extent.width, extent.height, extent.depth}, vuk::Samples::e1);
  ia.usage |= vuk::ImageUsageFlagBits::eTransferDst | vuk::ImageUsageFlagBits::eTransferSrc;

  image_id = vk_context.allocate_image(ia);
  ia.image = vk_context.image(image_id);
  image_view_id = vk_context.allocate_image_view(ia);
  ia.image_view = vk_context.image_view(image_view_id);

  std::vector<u8> ktx_data = {};
  std::vector<usize> ktx_per_level_offsets = {};
  const void* final_data = load_info.loaded_data
                               .or_else([&ia, is_generic, &stb_data, &ktx_data, &ktx_per_level_offsets, &ktx_texture] {
                                 if (!is_generic) {
                                   ia.level_count = ktx_texture->numLevels;

                                   ktx_per_level_offsets.resize(ia.level_count);

                                   for (u32 level = 0; level < ia.level_count; level++) {
                                     u64 offset = 0;
                                     auto offset_result = ktxTexture_GetImageOffset(
                                         ktxTexture(ktx_texture.get()), level, 0, 0, &offset);
                                     if (offset_result != KTX_SUCCESS) {
                                       OX_LOG_ERROR("Failed to get KTX2 offset.");
                                       return option<void*>(nullptr);
                                     }

                                     auto* image_data = ktxTexture_GetData(ktxTexture(ktx_texture.get())) + offset;
                                     auto image_size = ktxTexture_GetImageSize(ktxTexture(ktx_texture.get()), level);

                                     auto output_offset = static_cast<usize>(ktx_data.size());
                                     ktx_per_level_offsets[level] = output_offset;
                                     ktx_data.resize(ktx_data.size() + image_size);
                                     std::memcpy(ktx_data.data() + output_offset, image_data, image_size);
                                   }

                                   return option<void*>{ktx_data.data()};
                                 }

                                 return option<void*>{stb_data.get()};
                               })
                               .value();

  if (final_data != nullptr) {
    vuk::Value<vuk::ImageAttachment> fut = {};

    if (ktx_data.empty()) {
      fut = vuk::host_data_to_image(*allocator, vuk::DomainFlagBits::eTransferOnTransfer, ia, final_data);
      if (ia.level_count > 1) {
        fut = vuk::generate_mips(fut, ia.level_count);
      }
    } else {
      if (ia.level_count > 1) {
        fut = vuk::discard_ia("iv", ia);

        for (u32 level = 0; level < ia.level_count; level++) {
          auto mip_data_offset = ktx_per_level_offsets[level];
          auto level_extent = vuk::Extent3D{
              .width = extent.width >> level,
              .height = extent.height >> level,
              .depth = 1,
          };
          auto size = vuk::compute_image_size(format, level_extent);
          auto buffer = vk_context.allocate_buffer_super(vuk::MemoryUsage::eCPUonly, size);

          std::memcpy(buffer->mapped_ptr, ktx_data.data() + mip_data_offset, size);
          auto dst_mip = fut.mip(level);
          auto acquired_buf = vuk::acquire_buf("transient buffer", *buffer, vuk::Access::eNone);
          vuk::copy(std::move(acquired_buf), std::move(dst_mip));
        }
      }
    }

    vuk::Compiler compiler{};

    if (ia.usage & vuk::ImageUsageFlagBits::eStorage && ia.usage & vuk::ImageUsageFlagBits::eSampled) {
    } else {
      fut = fut.as_released(vuk::eFragmentSampled, vuk::DomainFlagBits::eGraphicsQueue);
    }

    fut.wait(*allocator, compiler);
  }

  // TODO: Actually fill the correct parameters
  sampler_id = vk_context.allocate_sampler({
      .magFilter = vuk::Filter::eLinear,
      .minFilter = vuk::Filter::eLinear,
      .maxLod = f32(ia.level_count),
  });
  attachment_ = ia;

  set_name(name_.c_str(), loc);
}

auto Texture::destroy() -> void {
  ZoneScoped;
  attachment_ = {};
  name_ = {};

  auto& vk_context = App::get_vkcontext();

  vk_context.destroy_image(image_id);
  image_id = {};

  vk_context.destroy_image_view(image_view_id);
  image_view_id = {};
}

vuk::Value<vuk::ImageAttachment> Texture::acquire(const vuk::Name name, const vuk::Access last_access) const {
  ZoneScoped;
  return vuk::acquire_ia(name.is_invalid() ? name_ : name, attachment(), last_access);
}

vuk::Value<vuk::ImageAttachment> Texture::discard(vuk::Name name) const {
  ZoneScoped;
  return vuk::discard_ia(name.is_invalid() ? name_ : name, attachment());
}

auto Texture::get_image() const -> const vuk::Image {
  ZoneScoped;

  auto& vk_context = App::get_vkcontext();

  return vk_context.image(image_id);
}

auto Texture::get_view() const -> const vuk::ImageView {
  ZoneScoped;

  auto& vk_context = App::get_vkcontext();

  return vk_context.image_view(image_view_id);
}

void Texture::set_name(std::string_view name, const std::source_location& loc) {
  ZoneScoped;
  auto& vk_context = App::get_vkcontext();
  vuk::Name new_name = vuk::Name(name);
  if (name.empty()) {
    auto file = fs::get_file_name(loc.file_name());
    const auto n = fmt::format("{0}:{1}", file, loc.line());
    new_name = vuk::Name(n);
  }

  vk_context.runtime->set_name(vk_context.image(image_id).image, vuk::Name(name));
  vk_context.runtime->set_name(vk_context.image_view(image_view_id).payload, vuk::Name(name));

  name_ = new_name;
}

std::unique_ptr<u8[]>
Texture::load_stb_image(const std::string& filename, uint32_t* width, uint32_t* height, uint32_t* bits, bool srgb) {
  ZoneScoped;

  const auto filePath = std::filesystem::path(filename);

  if (!exists(filePath))
    OX_LOG_ERROR("Couldn't load image, file doesn't exists. {}", filename);

  int tex_width = 0, tex_height = 0, tex_channels = 0;
  constexpr int size_of_channel = 8;

  const auto pixels = stbi_load(filename.c_str(), &tex_width, &tex_height, &tex_channels, STBI_rgb_alpha);

  if (tex_channels != 4)
    tex_channels = 4;

  if (width)
    *width = tex_width;
  if (height)
    *height = tex_height;
  if (bits)
    *bits = tex_channels * size_of_channel;

  const int32_t size = tex_width * tex_height * tex_channels * size_of_channel / 8;
  auto result = std::make_unique<u8[]>(size);
  memcpy(result.get(), pixels, size);
  stbi_image_free(pixels);

  return result;
}

std::unique_ptr<u8[]> Texture::load_stb_image_from_memory(
    void* buffer, size_t len, uint32_t* width, uint32_t* height, uint32_t* bits, bool flipY, bool srgb) {
  ZoneScoped;

  int tex_width = 0, tex_height = 0, tex_channels = 0;
  int size_of_channel = 8;
  const auto pixels = stbi_load_from_memory(
      static_cast<stbi_uc*>(buffer), static_cast<int>(len), &tex_width, &tex_height, &tex_channels, STBI_rgb_alpha);

  if (stbi_is_16_bit_from_memory(static_cast<stbi_uc*>(buffer), static_cast<int>(len))) {
    size_of_channel = 16;
  }

  if (tex_channels != 4)
    tex_channels = 4;

  if (width)
    *width = tex_width;
  if (height)
    *height = tex_height;
  if (bits)
    *bits = tex_channels * size_of_channel;

  const int32_t size = tex_width * tex_height * tex_channels * size_of_channel / 8;
  auto result = std::make_unique<u8[]>(size);
  memcpy(result.get(), pixels, size);

  stbi_image_free(pixels);
  return result;
}

u8* Texture::get_magenta_texture(uint32_t width, uint32_t height, uint32_t channels) {
  ZoneScoped;

  const uint32_t size = width * height * channels;
  const auto data = new u8[size];

  const u8 magenta[16] = {255, 0, 255, 255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 0, 255, 255};

  memcpy(data, magenta, size);

  return data;
}

u8* Texture::convert_to_four_channels(uint32_t width, uint32_t height, const u8* three_channel_data) {
  ZoneScoped;

  const auto bufferSize = width * height * 4;
  const auto buffer = new u8[bufferSize];
  auto* rgba = buffer;
  const auto* rgb = three_channel_data;
  for (uint32_t i = 0; i < width * height; ++i) {
    for (uint32_t j = 0; j < 3; ++j) {
      rgba[j] = rgb[j];
    }
    rgba += 4;
    rgb += 3;
  }
  return buffer;
}
} // namespace ox
