#pragma once

#include <vuk/Types.hpp>

namespace ox {
inline auto to_srgb_format(vuk::Format format) -> vuk::Format {
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

inline auto to_unorm_format(vuk::Format format) -> vuk::Format {
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

inline auto apply_srgb_preference(vuk::Format format, bool is_srgb) -> vuk::Format {
  return is_srgb ? to_srgb_format(format) : to_unorm_format(format);
}
} // namespace ox
