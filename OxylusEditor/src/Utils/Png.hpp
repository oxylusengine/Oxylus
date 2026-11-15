#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "Core/Types.hpp"

namespace ox {
struct RgbaImage {
  std::vector<u8> pixels = {};
  u32 width = 0;
  u32 height = 0;
};

auto downscale_rgba(RgbaImage image, u32 max_size) -> RgbaImage;
// empty on failure
auto encode_png(std::span<const u8> rgba, u32 width, u32 height) -> std::string;
auto write_png(const std::filesystem::path& path, std::span<const u8> rgba, u32 width, u32 height) -> bool;
} // namespace ox
