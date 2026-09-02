#include "TextureCompiler.hpp"

#include <atomic>
#include <cstring>
#include <fmt/format.h>
#include <fmt/std.h>
#include <ktx.h>
#include <mutex>
#include <stb_image.h>
#include <stb_image_resize2.h>
#include <vuk/Types.hpp>

#include "Core/Base.hpp"
#include "DDS/DDS.hpp"
#include "OS/File.hpp"
#include "Render/Utils/TextureFormat.hpp"

namespace ox::rc {
// libktx builds the basisu transcoder tables lazily on the first `ktxTexture2_TranscodeBasis`, from
// a plain non-atomic `static bool`, so the first transcode has to run alone
static std::mutex ktx_transcoder_init_mutex = {};
static std::atomic<bool> ktx_transcoder_ready = false;

auto transcode_ktx2(ktxTexture2* ktx) -> KTX_error_code {
  if (ktx_transcoder_ready.load(std::memory_order_acquire)) {
    return ktxTexture2_TranscodeBasis(ktx, KTX_TTF_BC7_RGBA, KTX_TF_HIGH_QUALITY);
  }

  auto lock = std::unique_lock(ktx_transcoder_init_mutex);
  const auto result = ktxTexture2_TranscodeBasis(ktx, KTX_TTF_BC7_RGBA, KTX_TF_HIGH_QUALITY);
  ktx_transcoder_ready.store(true, std::memory_order_release);

  return result;
}

auto mip_extent(u32 base, u32 level) -> u32 { return ox::max(base >> level, 1_u32); }

auto detect_texture_source_kind(std::span<const u8> bytes) -> TextureSourceKind {
  if (bytes.size() >= 4 && std::memcmp(bytes.data(), "DDS ", 4) == 0) {
    return TextureSourceKind::DDS;
  }

  constexpr static auto
    KTX2_MAGIC = std::array<u8, 12>{0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
  if (bytes.size() >= KTX2_MAGIC.size() && std::memcmp(bytes.data(), KTX2_MAGIC.data(), KTX2_MAGIC.size()) == 0) {
    return TextureSourceKind::KTX2;
  }

  return TextureSourceKind::Generic;
}

auto compile_dds(Session& session, std::span<const u8> bytes, std::string_view name, option<bool> srgb)
  -> option<TextureData> {
  ZoneScoped;

  auto dds_image = dds::Image{};
  if (dds::readImage(const_cast<u8*>(bytes.data()), bytes.size(), &dds_image) != dds::ReadResult::Success) {
    session.push_error(fmt::format("'{}' is not a readable DDS file.", name));
    return nullopt;
  }

  // DXGI spells sRGB into the format itself, so the file already answers this unless asked otherwise.
  auto format = static_cast<vuk::Format>(dds::getVulkanFormat(dds_image.format, dds_image.supportsAlpha));
  if (srgb.has_value()) {
    format = apply_srgb_preference(format, *srgb);
  }

  auto result = TextureData{
    .name = std::string(name),
    .vk_format = static_cast<u32>(format),
    .width = dds_image.width,
    .height = dds_image.height,
  };

  result.mips.reserve(dds_image.numMips);
  for (auto level = 0_u32; level < dds_image.numMips; level++) {
    const auto& mip = dds_image.mipmaps[level];
    auto& out = result.mips.emplace_back();
    out.width = mip_extent(result.width, level);
    out.height = mip_extent(result.height, level);
    out.pixels.assign(mip.data(), mip.data() + mip.size_bytes());
  }

  return result;
}

auto compile_ktx2(Session& session, std::span<const u8> bytes, std::string_view name, option<bool> srgb)
  -> option<TextureData> {
  ZoneScoped;

  ktxTexture2* ktx = nullptr;
  if (
    ktxTexture2_CreateFromMemory(bytes.data(), bytes.size(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &ktx) !=
    KTX_SUCCESS
  ) {
    session.push_error(fmt::format("'{}' is not a readable KTX2 file.", name));
    return nullopt;
  }
  std::unique_ptr<ktxTexture2, decltype([](ktxTexture2* p) { ktxTexture_Destroy(ktxTexture(p)); })> owned(ktx);

  // A supercompressed KTX2 has no vkFormat to read until it is transcoded, but its data format
  // descriptor carries the transfer function either way, so that is the file's own answer.
  const auto declares_srgb = ktxTexture2_GetTransferFunction_e(ktx) == KHR_DF_TRANSFER_SRGB;

  auto format = vuk::Format::eBc7UnormBlock;
  if (ktxTexture2_NeedsTranscoding(ktx)) {
    ZoneNamedN(z, "Transcode KTX2", true);
    if (transcode_ktx2(ktx) != KTX_SUCCESS) {
      session.push_error(fmt::format("Couldn't transcode KTX2 texture '{}'.", name));
      return nullopt;
    }
  } else {
    format = static_cast<vuk::Format>(static_cast<u32>(ktx->vkFormat));
  }

  auto result = TextureData{
    .name = std::string(name),
    .vk_format = static_cast<u32>(apply_srgb_preference(format, srgb.value_or(declares_srgb))),
    .width = ktx->baseWidth,
    .height = ktx->baseHeight,
  };

  result.mips.reserve(ktx->numLevels);
  for (auto level = 0_u32; level < ktx->numLevels; level++) {
    ktx_size_t offset = 0;
    if (ktxTexture_GetImageOffset(ktxTexture(ktx), level, 0, 0, &offset) != KTX_SUCCESS) {
      session.push_error(fmt::format("Failed to get KTX2 image offset for level {} of '{}'.", level, name));
      return nullopt;
    }

    const auto* level_data = ktxTexture_GetData(ktxTexture(ktx)) + offset;
    const auto level_size = ktxTexture_GetImageSize(ktxTexture(ktx), level);

    auto& out = result.mips.emplace_back();
    out.width = mip_extent(result.width, level);
    out.height = mip_extent(result.height, level);
    out.pixels.assign(level_data, level_data + level_size);
  }

  return result;
}

auto mip_count(u32 width, u32 height) -> u32 {
  auto count = 1_u32;
  auto extent = ox::max(width, height);
  while (extent > 1) {
    extent >>= 1;
    count += 1;
  }

  return count;
}

// PNG/JPEG and friends: stb decodes them to RGBA8 and the mip chain is built here rather than on the
// GPU at load, so a compiled texture is the same shape whatever it was authored as.
auto compile_generic(Session& session, std::span<const u8> bytes, std::string_view name, option<bool> srgb)
  -> option<TextureData> {
  ZoneScoped;

  auto width = 0;
  auto height = 0;
  auto channels = 0;
  auto* raw =
    stbi_load_from_memory(bytes.data(), static_cast<i32>(bytes.size()), &width, &height, &channels, STBI_rgb_alpha);
  if (!raw) {
    session.push_error(fmt::format("Couldn't decode image '{}': {}", name, stbi_failure_reason()));
    return nullopt;
  }

  OX_DEFER(&) { stbi_image_free(raw); };

  // a plain image carries no colour space of its own, and all but a handful are authored as sRGB. a
  // model naming one as a normal or metallic-roughness map overrides this.
  const auto is_srgb = srgb.value_or(true);
  auto result = TextureData{
    .name = std::string(name),
    .vk_format = static_cast<u32>(is_srgb ? vuk::Format::eR8G8B8A8Srgb : vuk::Format::eR8G8B8A8Unorm),
    .width = static_cast<u32>(width),
    .height = static_cast<u32>(height),
  };

  const auto level_count = mip_count(result.width, result.height);
  result.mips.reserve(level_count);

  auto& base = result.mips.emplace_back();
  base.width = result.width;
  base.height = result.height;
  base.pixels.assign(raw, raw + static_cast<usize>(width) * static_cast<usize>(height) * 4);

  for (auto level = 1_u32; level < level_count; level++) {
    const auto level_width = mip_extent(result.width, level);
    const auto level_height = mip_extent(result.height, level);
    const auto& previous = result.mips[level - 1];

    auto pixels = std::vector<u8>(static_cast<usize>(level_width) * level_height * 4);
    // downsampling sRGB values with a linear filter darkens the result, so the sRGB variant decodes
    // to linear, filters, and re-encodes
    const auto* resized = is_srgb ? stbir_resize_uint8_srgb(
                                      previous.pixels.data(),
                                      static_cast<i32>(previous.width),
                                      static_cast<i32>(previous.height),
                                      0,
                                      pixels.data(),
                                      static_cast<i32>(level_width),
                                      static_cast<i32>(level_height),
                                      0,
                                      STBIR_RGBA
                                    )
                                  : stbir_resize_uint8_linear(
                                      previous.pixels.data(),
                                      static_cast<i32>(previous.width),
                                      static_cast<i32>(previous.height),
                                      0,
                                      pixels.data(),
                                      static_cast<i32>(level_width),
                                      static_cast<i32>(level_height),
                                      0,
                                      STBIR_RGBA
                                    );
    if (!resized) {
      session.push_error(fmt::format("Couldn't build mip {} of '{}'.", level, name));
      return nullopt;
    }

    auto& mip = result.mips.emplace_back();
    mip.width = level_width;
    mip.height = level_height;
    mip.pixels = std::move(pixels);
  }

  return result;
}

auto compile_texture(Session& session, std::span<const u8> bytes, std::string_view name, option<bool> srgb)
  -> option<TextureData> {
  ZoneScoped;

  switch (detect_texture_source_kind(bytes)) {
    case TextureSourceKind::DDS    : return compile_dds(session, bytes, name, srgb);
    case TextureSourceKind::KTX2   : return compile_ktx2(session, bytes, name, srgb);
    case TextureSourceKind::Generic: return compile_generic(session, bytes, name, srgb);
  }

  return nullopt;
}

auto compile_texture(Session& session, const TextureCompileRequest& request) -> option<TextureData> {
  ZoneScoped;

  auto name = request.name.empty() ? request.path.filename().string() : request.name;
  if (!request.source_bytes.empty()) {
    return compile_texture(session, request.source_bytes, name, request.srgb);
  }

  if (!std::filesystem::exists(request.path)) {
    session.push_error(fmt::format("Texture '{}' does not exist.", request.path));
    return nullopt;
  }

  auto file = File(request.path, FileAccess::Read);
  const auto* mapped = file.map();
  if (!mapped) {
    session.push_error(fmt::format("Could not map texture '{}'.", request.path));
    return nullopt;
  }

  return compile_texture(session, std::span(static_cast<const u8*>(mapped), file.size), name, request.srgb);
}
} // namespace ox::rc
