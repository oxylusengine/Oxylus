#include "Utils/Png.hpp"

#include <basisu/encoder/basisu_enc.h>
#include <cmath>
#include <cstring>
#include <tracy/Tracy.hpp>

#define MINIZ_HEADER_FILE_ONLY
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include <basisu/encoder/basisu_miniz.h>

#include "OS/File.hpp"

namespace ox {
auto downscale_rgba(RgbaImage image, const u32 max_size) -> RgbaImage {
  ZoneScoped;

  const auto longest = ox::max(image.width, image.height);
  if (longest <= max_size || longest == 0) {
    return image;
  }

  const auto scale = static_cast<f64>(max_size) / static_cast<f64>(longest);
  const auto width = ox::max(1_u32, static_cast<u32>(std::lround(image.width * scale)));
  const auto height = ox::max(1_u32, static_cast<u32>(std::lround(image.height * scale)));

  // color_rgba is laid out r, g, b, a, the same as the pixels
  auto source = basisu::image(image.width, image.height);
  std::memcpy(
    source.get_ptr(),
    image.pixels.data(),
    ox::min(image.pixels.size(), static_cast<usize>(source.get_total_pixels()) * 4)
  );
  auto resized = basisu::image(width, height);
  if (!basisu::image_resample(source, resized, true)) {
    return image;
  }

  auto out = RgbaImage{
    .pixels = std::vector<u8>(static_cast<usize>(width) * height * 4),
    .width = width,
    .height = height
  };
  std::memcpy(out.pixels.data(), resized.get_ptr(), out.pixels.size());
  return out;
}

auto encode_png(const std::span<const u8> rgba, const u32 width, const u32 height) -> std::string {
  ZoneScoped;

  if (rgba.size() < static_cast<usize>(width) * height * 4) {
    return {};
  }

  auto size = usize{0};
  auto* data = buminiz::tdefl_write_image_to_png_file_in_memory(
    rgba.data(),
    static_cast<i32>(width),
    static_cast<i32>(height),
    4,
    &size
  );
  if (data == nullptr) {
    return {};
  }

  auto png = std::string(static_cast<const c8*>(data), size);
  buminiz::mz_free(data);
  return png;
}

auto write_png(const std::filesystem::path& path, const std::span<const u8> rgba, const u32 width, const u32 height)
  -> bool {
  ZoneScoped;

  const auto png = encode_png(rgba, width, height);
  if (png.empty()) {
    return false;
  }

  auto file = File(path, FileAccess::Write);
  if (!file) {
    return false;
  }

  file.write(png);
  file.close();
  return true;
}
} // namespace ox
