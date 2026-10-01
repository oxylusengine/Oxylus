#include "TextureCompiler.hpp"

#include <algorithm>
#include <array>
#include <basisu/encoder/basisu_bc15_spmd.h>
#include <basisu/encoder/basisu_bc7e_scalar.h>
#include <basisu/encoder/basisu_enc.h>
#include <basisu/encoder/pvpngreader.h>
#include <basisu/transcoder/basisu_transcoder.h>
#include <cmath>
#include <cstring>
#include <fmt/format.h>
#include <fmt/std.h>
#include <mutex>
#include <vuk/Types.hpp>

#include "Memory/Stack.hpp"
#include "OS/File.hpp"
#include "Parallel.hpp"
#include "Render/Utils/TextureFormat.hpp"
#include "Session.hpp"

namespace ox::rc {
// VkComponentSwizzle per channel. a one-channel pack reads back the way the grayscale image it came from would, and a
// BC5 normal map gets a blue of one: the shader rebuilds z anyway, but previews look like a normal map again and
// anything sampling it without the rebuild still gets a normal facing outward
constexpr static auto GRAYSCALE_COMPONENTS = std::array<u8, 4>{3, 3, 3, 2};
constexpr static auto NORMAL_COMPONENTS = std::array<u8, 4>{3, 4, 2, 2};

static auto usage_components(TextureUsage usage) -> std::array<u8, 4> {
  switch (usage) {
    case TextureUsage::Mask  : return GRAYSCALE_COMPONENTS;
    case TextureUsage::Normal: return NORMAL_COMPONENTS;
    default                  : return {};
  }
}

// enough blocks per job that a small mip isn't split into jobs smaller than their dispatch
constexpr static auto BLOCKS_PER_JOB = 2048_u32;

constexpr static auto PNG_SIGNATURE = std::array<u8, 8>{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
constexpr static auto JPEG_SIGNATURE = std::array<u8, 3>{0xFF, 0xD8, 0xFF};
constexpr static auto DDS_SIGNATURE = std::array<u8, 4>{'D', 'D', 'S', ' '};
constexpr static auto KTX2_SIGNATURE = std::array<u8, 12>{
  0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, 0x0D, 0x0A, 0x1A, 0x0A
};

// bc7e builds its tables behind a plain static flag, so the first initialisation has to run alone
static std::once_flag codec_init_flag = {};

static auto init_codecs() -> void {
  std::call_once(codec_init_flag, [] {
    basisu::basisu_encoder_init();
    bc7e_scalar::bc7e_compress_block_init();
  });
}

template <usize N>
static auto starts_with(std::span<const u8> bytes, const std::array<u8, N>& signature) -> bool {
  return bytes.size() >= N && std::memcmp(bytes.data(), signature.data(), N) == 0;
}

static auto mip_extent(u32 base, u32 level) -> u32 { return ox::max(base >> level, 1_u32); }

static auto mip_count(u32 width, u32 height) -> u32 {
  auto count = 1_u32;
  for (auto extent = ox::max(width, height); extent > 1; extent >>= 1) {
    count += 1;
  }

  return count;
}

static auto block_bytes(TextureUsage usage) -> u32 { return usage == TextureUsage::Mask ? 8 : 16; }

static auto block_format(TextureUsage usage) -> vuk::Format {
  switch (usage) {
    case TextureUsage::Color : return vuk::Format::eBc7SrgbBlock;
    case TextureUsage::Linear: return vuk::Format::eBc7UnormBlock;
    case TextureUsage::Normal: return vuk::Format::eBc5UnormBlock;
    case TextureUsage::Mask  : return vuk::Format::eBc4UnormBlock;
  }

  return vuk::Format::eBc7SrgbBlock;
}

// the usual names for a normal map, since nothing in a plain image says it is one
static auto looks_like_normal_map(std::string_view name) -> bool {
  memory::ScopedStack stack;

  auto stem = std::string_view(stack.to_lower(name));
  if (const auto dot = stem.find_last_of('.'); dot != std::string_view::npos) {
    stem = stem.substr(0, dot);
  }

  return stem.contains("normal") || stem.ends_with("_n") || stem.ends_with("_nrm") || stem.ends_with("_nor");
}

// a grayscale PNG says so in its header; anything else counts when every texel is gray and opaque
static auto is_grayscale(std::span<const u8> bytes, const basisu::image& image) -> bool {
  if (auto info = pv_png::png_info{}; pv_png::get_png_info(bytes.data(), bytes.size(), info)) {
    return info.m_num_chans == 1;
  }

  return std::ranges::all_of(image.get_pixels(), [](const basisu::color_rgba& texel) {
    return texel.r == texel.g && texel.g == texel.b && texel.a == 255;
  });
}

static auto infer_usage(std::span<const u8> bytes, const basisu::image& image, std::string_view name) -> TextureUsage {
  if (looks_like_normal_map(name)) {
    return TextureUsage::Normal;
  }

  if (is_grayscale(bytes, image)) {
    return TextureUsage::Mask;
  }

  return TextureUsage::Color;
}

static auto decode_image(Session& session, std::span<const u8> bytes, std::string_view name) -> option<basisu::image> {
  ZoneScoped;

  auto image = basisu::image{};
  auto decoded = false;
  if (starts_with(bytes, PNG_SIGNATURE)) {
    decoded = basisu::load_png(bytes.data(), bytes.size(), image);
  } else if (starts_with(bytes, JPEG_SIGNATURE)) {
    decoded = basisu::load_jpg(bytes.data(), bytes.size(), image);
  } else {
    session.push_error(fmt::format("'{}' is not a PNG, JPEG, DDS or KTX2 image.", name));
    return nullopt;
  }

  if (!decoded) {
    session.push_error(fmt::format("Couldn't decode image '{}'.", name));
    return nullopt;
  }

  return image;
}

// resampling drifts normals off unit length, which the shader's z rebuild assumes
static auto renormalize(basisu::image& image) -> void {
  const auto to_unit = [](u8 value) {
    return static_cast<f32>(value) / 127.5f - 1.0f;
  };
  const auto to_byte = [](f32 value) {
    return static_cast<u8>(std::clamp((value + 1.0f) * 127.5f + 0.5f, 0.0f, 255.0f));
  };

  for (auto& texel : image.get_pixels()) {
    const auto x = to_unit(texel.r);
    const auto y = to_unit(texel.g);
    const auto z = to_unit(texel.b);
    const auto length = std::sqrt(x * x + y * y + z * z);
    if (length > 0.0f) {
      texel.r = to_byte(x / length);
      texel.g = to_byte(y / length);
      texel.b = to_byte(z / length);
    }
  }
}

static auto build_mips(Session& session, basisu::image base, TextureUsage usage, std::string_view name)
  -> option<std::vector<basisu::image>> {
  ZoneScoped;

  const auto level_count = mip_count(base.get_width(), base.get_height());
  auto mips = std::vector<basisu::image>();
  mips.reserve(level_count);
  mips.push_back(std::move(base));

  for (auto level = 1_u32; level < level_count; level++) {
    const auto& previous = mips.back();
    auto next = basisu::image(mip_extent(previous.get_width(), 1), mip_extent(previous.get_height(), 1));
    // filtering sRGB values directly darkens the result, so colour goes through linear and back
    if (!basisu::image_resample(previous, next, usage == TextureUsage::Color, "kaiser")) {
      session.push_error(fmt::format("Couldn't build mip {} of '{}'.", level, name));
      return nullopt;
    }

    if (usage == TextureUsage::Normal) {
      renormalize(next);
    }

    mips.push_back(std::move(next));
  }

  return mips;
}

// the 16 texels of one block, repeating the last row and column where the image doesn't fill it
static auto gather_block(const basisu::image& image, u32 block_x, u32 block_y, std::span<basisu::color_rgba> out)
  -> void {
  const auto max_x = image.get_width() - 1;
  const auto max_y = image.get_height() - 1;
  for (auto y = 0_u32; y < 4; y++) {
    for (auto x = 0_u32; x < 4; x++) {
      out[y * 4 + x] = image(ox::min(block_x * 4 + x, max_x), ox::min(block_y * 4 + y, max_y));
    }
  }
}

static auto encode_level(Session& session, const basisu::image& image, TextureUsage usage) -> std::vector<u8> {
  ZoneScoped;

  const auto blocks_x = (image.get_width() + 3) / 4;
  const auto blocks_y = (image.get_height() + 3) / 4;
  const auto row_bytes = static_cast<usize>(blocks_x) * block_bytes(usage);
  auto blocks = std::vector<u8>(row_bytes * blocks_y);

  auto bc7_params = bc7e_scalar::bc7e_compress_block_params{};
  bc7e_scalar::bc7e_compress_block_params_init_slow(&bc7_params, usage == TextureUsage::Color);

  const auto rows_per_job = ox::max(BLOCKS_PER_JOB / blocks_x, 1_u32);
  auto scope = ParallelScope(session->job_manager);
  for (auto first_row = 0_u32; first_row < blocks_y; first_row += rows_per_job) {
    scope.dispatch([&, first_row] {
      ZoneScopedN("Encode block rows");
      memory::ScopedStack stack;

      auto texels = stack.alloc<basisu::color_rgba>(static_cast<usize>(blocks_x) * 16);
      const auto last_row = ox::min(first_row + rows_per_job, blocks_y);
      for (auto row = first_row; row < last_row; row++) {
        for (auto block = 0_u32; block < blocks_x; block++) {
          gather_block(image, block, row, texels.subspan(static_cast<usize>(block) * 16, 16));
        }

        auto* out = blocks.data() + row * row_bytes;
        switch (usage) {
          case TextureUsage::Color :
          case TextureUsage::Linear: {
            // color_rgba is laid out r, g, b, a, which is the packed RGBA bc7e reads
            bc7e_scalar::bc7e_compress_blocks(
              blocks_x,
              reinterpret_cast<u64*>(out),
              reinterpret_cast<const u32*>(texels.data()),
              &bc7_params
            );
          } break;
          case TextureUsage::Normal: basisu::bc_spmd::encode_bc5(out, texels.data(), blocks_x, true, true); break;
          case TextureUsage::Mask  : basisu::bc_spmd::encode_bc4(out, texels.data(), blocks_x, true, true); break;
        }
      }
    });
  }

  return blocks;
}

static auto encode_image(Session& session, basisu::image image, TextureUsage usage, std::string_view name)
  -> option<TextureData> {
  ZoneScoped;

  auto result = TextureData{
    .name = std::string(name),
    .vk_format = static_cast<u32>(block_format(usage)),
    .width = image.get_width(),
    .height = image.get_height(),
    .components = usage_components(usage),
  };

  auto mips = build_mips(session, std::move(image), usage, name);
  if (!mips.has_value()) {
    return nullopt;
  }

  result.mips.reserve(mips->size());
  for (const auto& mip : *mips) {
    result.mips.push_back(
      TextureMipData{.width = mip.get_width(), .height = mip.get_height(), .pixels = encode_level(session, mip, usage)}
    );
  }

  return result;
}

static auto compile_image(
  Session& session, std::span<const u8> bytes, std::string_view name, option<TextureUsage> usage
) -> option<TextureData> {
  ZoneScoped;

  auto image = decode_image(session, bytes, name);
  if (!image.has_value()) {
    return nullopt;
  }

  const auto resolved = usage.value_or(infer_usage(bytes, *image, name));
  return encode_image(session, std::move(*image), resolved, name);
}

// a DDS already in a block format ships as is, mips included; an uncompressed one is encoded like any image
static auto compile_dds(Session& session, std::span<const u8> bytes, std::string_view name, option<TextureUsage> usage)
  -> option<TextureData> {
  ZoneScoped;

  auto dds = basist::dds_transcoder{};
  if (!dds.init(bytes.data(), static_cast<u32>(bytes.size())) || !dds.start_transcoding()) {
    session.push_error(fmt::format("'{}' is not a DDS file basisu can read (BC1-5, BC7 or uncompressed).", name));
    return nullopt;
  }

  if (dds.get_layers() > 1 || dds.get_faces() > 1) {
    session.push_message(fmt::format("'{}' is an array or cubemap, only its first image is used.", name));
  }

  using Kind = basist::dds_transcoder::source_kind;
  const auto kind = dds.get_source_kind();
  if (kind == Kind::cUncompressed) {
    auto image = basisu::image(dds.get_width(), dds.get_height());
    const auto texel_count = dds.get_width() * dds.get_height();
    if (!dds.transcode_image_level(
          0,
          0,
          0,
          image.get_ptr(),
          texel_count,
          basist::transcoder_texture_format::cTFRGBA32
        )) {
      session.push_error(fmt::format("Couldn't decode DDS '{}'.", name));
      return nullopt;
    }

    const auto resolved = usage.value_or(infer_usage(bytes, image, name));
    return encode_image(session, std::move(image), resolved, name);
  }

  auto format = vuk::Format::eUndefined;
  auto components = std::array<u8, 4>{};
  switch (kind) {
    case Kind::cBC1: format = vuk::Format::eBc1RgbaUnormBlock; break;
    case Kind::cBC2: format = vuk::Format::eBc2UnormBlock; break;
    case Kind::cBC3: format = vuk::Format::eBc3UnormBlock; break;
    case Kind::cBC4: {
      format = vuk::Format::eBc4UnormBlock;
      components = GRAYSCALE_COMPONENTS;
    } break;
    case Kind::cBC5: {
      format = vuk::Format::eBc5UnormBlock;
      components = NORMAL_COMPONENTS;
    } break;
    case Kind::cBC7: format = vuk::Format::eBc7UnormBlock; break;
    default        : {
      session.push_error(fmt::format("'{}' holds a DDS format basisu can't read.", name));
      return nullopt;
    }
  }

  // the file's own colour space unless a material slot says otherwise; formats without an sRGB variant ignore it
  const auto is_srgb = usage.has_value() ? *usage == TextureUsage::Color : dds.is_srgb();
  auto result = TextureData{
    .name = std::string(name),
    .vk_format = static_cast<u32>(apply_srgb_preference(format, is_srgb)),
    .width = dds.get_width(),
    .height = dds.get_height(),
    .components = components,
  };

  result.mips.reserve(dds.get_levels());
  for (auto level = 0_u32; level < dds.get_levels(); level++) {
    auto slice = basist::dds_transcoder::slice_desc{};
    if (!dds.get_slice_desc(slice, level, 0, 0)) {
      session.push_error(fmt::format("DDS '{}' is missing mip {}.", name, level));
      return nullopt;
    }

    const auto* data = dds.get_data() + slice.m_ofs;
    result.mips.push_back(
      TextureMipData{
        .width = slice.m_width,
        .height = slice.m_height,
        .pixels = std::vector<u8>(data, data + slice.m_size),
      }
    );
  }

  return result;
}

// Basis Universal payloads only: one with its own mips transcodes each level straight to the target block format,
// one without is decoded and encoded like any image so it gets a mip chain
static auto compile_ktx2(Session& session, std::span<const u8> bytes, std::string_view name, option<TextureUsage> usage)
  -> option<TextureData> {
  ZoneScoped;

  auto ktx2 = basist::ktx2_transcoder{};
  if (!ktx2.init(bytes.data(), static_cast<u32>(bytes.size())) || !ktx2.start_transcoding()) {
    session.push_error(
      fmt::format(
        "'{}' isn't a Basis Universal KTX2. KTX2 files holding a plain Vulkan format aren't supported, "
        "convert it to DDS or PNG.",
        name
      )
    );
    return nullopt;
  }

  if (ktx2.is_hdr()) {
    session.push_error(fmt::format("'{}' is an HDR KTX2, which isn't supported yet.", name));
    return nullopt;
  }

  if (ktx2.get_layers() > 1 || ktx2.get_faces() > 1) {
    session.push_message(fmt::format("'{}' is an array or cubemap, only its first image is used.", name));
  }

  const auto inferred = looks_like_normal_map(name) ? TextureUsage::Normal
                        : ktx2.is_srgb()            ? TextureUsage::Color
                                                    : TextureUsage::Linear;
  const auto resolved = usage.value_or(inferred);

  if (ktx2.get_levels() == 1) {
    auto image = basisu::image(ktx2.get_width(), ktx2.get_height());
    const auto texel_count = ktx2.get_width() * ktx2.get_height();
    if (!ktx2.transcode_image_level(
          0,
          0,
          0,
          image.get_ptr(),
          texel_count,
          basist::transcoder_texture_format::cTFRGBA32
        )) {
      session.push_error(fmt::format("Couldn't decode KTX2 '{}'.", name));
      return nullopt;
    }

    return encode_image(session, std::move(image), resolved, name);
  }

  auto result = TextureData{
    .name = std::string(name),
    .vk_format = static_cast<u32>(block_format(resolved)),
    .width = ktx2.get_width(),
    .height = ktx2.get_height(),
    .components = usage_components(resolved),
  };

  result.mips.reserve(ktx2.get_levels());
  for (auto level = 0_u32; level < ktx2.get_levels(); level++) {
    auto info = basist::ktx2_image_level_info{};
    if (!ktx2.get_image_level_info(info, level, 0, 0)) {
      session.push_error(fmt::format("KTX2 '{}' is missing mip {}.", name, level));
      return nullopt;
    }

    auto mip = TextureMipData{.width = info.m_orig_width, .height = info.m_orig_height};
    if (resolved == TextureUsage::Color || resolved == TextureUsage::Linear) {
      const auto block_count = info.m_num_blocks_x * info.m_num_blocks_y;
      mip.pixels.resize(static_cast<usize>(block_count) * block_bytes(resolved));
      if (!ktx2.transcode_image_level(
            level,
            0,
            0,
            mip.pixels.data(),
            block_count,
            basist::transcoder_texture_format::cTFBC7_RGBA
          )) {
        session.push_error(fmt::format("Couldn't transcode mip {} of KTX2 '{}'.", level, name));
        return nullopt;
      }
    } else {
      // basisu's BC4/BC5 targets read alpha for the second channel whatever they are asked, so a normal map (x and y
      // in red and green) goes through RGBA and our own encoder
      auto image = basisu::image(info.m_orig_width, info.m_orig_height);
      const auto texel_count = info.m_orig_width * info.m_orig_height;
      if (!ktx2.transcode_image_level(
            level,
            0,
            0,
            image.get_ptr(),
            texel_count,
            basist::transcoder_texture_format::cTFRGBA32
          )) {
        session.push_error(fmt::format("Couldn't decode mip {} of KTX2 '{}'.", level, name));
        return nullopt;
      }

      mip.pixels = encode_level(session, image, resolved);
    }

    result.mips.push_back(std::move(mip));
  }

  return result;
}

auto compile_texture(Session& session, std::span<const u8> bytes, std::string_view name, option<TextureUsage> usage)
  -> option<TextureData> {
  ZoneScoped;

  init_codecs();

  if (starts_with(bytes, DDS_SIGNATURE)) {
    return compile_dds(session, bytes, name, usage);
  }

  if (starts_with(bytes, KTX2_SIGNATURE)) {
    return compile_ktx2(session, bytes, name, usage);
  }

  return compile_image(session, bytes, name, usage);
}

auto compile_texture(Session& session, const TextureCompileRequest& request) -> option<TextureData> {
  ZoneScoped;

  auto name = request.name.empty() ? request.path.filename().string() : request.name;
  if (!request.source_bytes.empty()) {
    return compile_texture(session, request.source_bytes, name, request.usage);
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

  return compile_texture(session, std::span(static_cast<const u8*>(mapped), file.size), name, request.usage);
}
} // namespace ox::rc
