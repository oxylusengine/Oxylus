#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION

#include "TextureCompiler.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fmt/std.h>
#include <ktx.h>
#include <stb_image.h>
#include <stb_image_resize2.h>
#include <vuk/Types.hpp>

#include "Core/Option.hpp"
#include "DDS/DDS.hpp"
#include "OS/File.hpp"
#include "OS/OS.hpp"

namespace ox::rc {
auto append_level(TextureData& out, u32 width, u32 height, const void* pixels, u64 size) -> void {
  auto mip = TextureMipData{.width = width, .height = height};
  mip.pixels.resize(size);
  std::memcpy(mip.pixels.data(), pixels, size);
  out.mips.push_back(std::move(mip));
}

auto compile_generic(const TextureCompileRequest& info) -> option<TextureData> {
  int width = 0, height = 0, channels = 0;
  const auto path_str = info.path.string();
  auto* pixels = stbi_load(path_str.c_str(), &width, &height, &channels, STBI_rgb_alpha);
  if (!pixels) {
    return nullopt;
  }

  auto result = TextureData{
    .name = info.name.empty() ? info.path.stem().string() : info.name,
    .format = info.srgb ? vuk::Format::eR8G8B8A8Srgb : vuk::Format::eR8G8B8A8Unorm,
    .width = static_cast<u32>(width),
    .height = static_cast<u32>(height),
    .layer_count = 1,
  };

  auto level_w = static_cast<u32>(width);
  auto level_h = static_cast<u32>(height);
  const auto level0_size = static_cast<u64>(level_w) * level_h * 4;
  append_level(result, level_w, level_h, pixels, level0_size);

  auto src = std::vector<u8>(pixels, pixels + level0_size);
  stbi_image_free(pixels);

  while (level_w > 1 || level_h > 1) {
    const auto next_w = std::max(level_w / 2, 1_u32);
    const auto next_h = std::max(level_h / 2, 1_u32);

    auto next = std::vector<u8>(static_cast<usize>(next_w) * next_h * 4);
    stbir_resize_uint8_linear(
      src.data(),
      static_cast<int>(level_w),
      static_cast<int>(level_h),
      0,
      next.data(),
      static_cast<int>(next_w),
      static_cast<int>(next_h),
      0,
      STBIR_RGBA
    );

    append_level(result, next_w, next_h, next.data(), next.size());

    src = std::move(next);
    level_w = next_w;
    level_h = next_h;
  }

  return result;
}

auto compile_dds(const TextureCompileRequest& info) -> option<TextureData> {
  auto dds_image = dds::Image{};
  const auto path_str = info.path.string();
  if (dds::readFile(path_str, &dds_image) != dds::ReadResult::Success) {
    return nullopt;
  }

  auto result = TextureData{
    .name = info.name.empty() ? info.path.stem().string() : info.name,
    .format = static_cast<vuk::Format>(dds::getVulkanFormat(dds_image.format, dds_image.supportsAlpha)),
    .width = dds_image.width,
    .height = dds_image.height,
    .layer_count = std::max(dds_image.arraySize, 1_u32),
  };

  for (auto level = 0_u32; level < dds_image.numMips; level++) {
    const auto& mip = dds_image.mipmaps[level];
    append_level(
      result,
      std::max(dds_image.width >> level, 1_u32),
      std::max(dds_image.height >> level, 1_u32),
      mip.data(),
      mip.size()
    );
  }

  return result;
}

auto compile_ktx(const TextureCompileRequest& info) -> option<TextureData> {
  ktxTexture2* ktx = nullptr;
  const auto path_str = info.path.string();
  if (ktxTexture2_CreateFromNamedFile(path_str.c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &ktx) != KTX_SUCCESS) {
    return nullopt;
  }
  std::unique_ptr<ktxTexture2, decltype([](ktxTexture2* p) { ktxTexture_Destroy(ktxTexture(p)); })> owned(ktx);

  auto format = vuk::Format::eBc7UnormBlock;
  if (
    ktxTexture2_NeedsTranscoding(ktx) &&
    ktxTexture2_TranscodeBasis(ktx, KTX_TTF_BC7_RGBA, KTX_TF_HIGH_QUALITY) != KTX_SUCCESS
  ) {
    return nullopt;
  } else {
    format = static_cast<vuk::Format>(static_cast<VkFormat>(ktx->vkFormat));
  }

  auto result = TextureData{
    .name = info.name.empty() ? info.path.stem().string() : info.name,
    .format = static_cast<vuk::Format>(format),
    .width = ktx->baseWidth,
    .height = ktx->baseHeight,
    .layer_count = std::max(ktx->numLayers, 1_u32),
  };

  for (auto level = 0_u32; level < ktx->numLevels; level++) {
    ktx_size_t offset = 0;
    if (ktxTexture_GetImageOffset(ktxTexture(ktx), level, 0, 0, &offset) != KTX_SUCCESS) {
      return nullopt;
    }
    auto* level_data = ktxTexture_GetData(ktxTexture(ktx)) + offset;
    auto level_size = ktxTexture_GetImageSize(ktxTexture(ktx), level);
    append_level(
      result,
      std::max(ktx->baseWidth >> level, 1_u32),
      std::max(ktx->baseHeight >> level, 1_u32),
      level_data,
      level_size
    );
  }

  return result;
}

auto detect_texture_source_type(const std::filesystem::path& path) -> TextureSourceType {
  auto file = File(path, FileAccess::Read);
  if (!file) {
    return TextureSourceType::Generic;
  }

  constexpr static auto SAFE_HEADER_SIZE = 12_sz;
  auto header = std::array<u8, SAFE_HEADER_SIZE>{};
  auto read_bytes = file.read(header.data(), header.size());

  if (read_bytes >= 4 && std::memcmp(header.data(), "DDS ", 4) == 0) {
    return TextureSourceType::DDS;
  }

  constexpr static auto
    KTX2_MAGIC = std::array<u8, 12>{0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
  if (read_bytes >= KTX2_MAGIC.size() && std::memcmp(header.data(), KTX2_MAGIC.data(), KTX2_MAGIC.size()) == 0) {
    return TextureSourceType::KTX;
  }

  return TextureSourceType::Generic;
}

auto TextureCompiler::compile(const TextureCompileRequest& info) -> option<TextureData> {
  switch (detect_texture_source_type(info.path)) {
    case TextureSourceType::DDS    : return compile_dds(info);
    case TextureSourceType::KTX    : return compile_ktx(info);
    case TextureSourceType::Generic:
    default                        : return compile_generic(info);
  }
}

} // namespace ox::rc
