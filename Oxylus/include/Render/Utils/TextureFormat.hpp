#pragma once

#include <string_view>

#include "Render/Graph/Types.hpp"

namespace ox {
inline auto to_srgb_format(vir::Format format) -> vir::Format {
  switch (format) {
    case vir::Format::RGBA8Unorm       : return vir::Format::RGBA8Srgb;
    case vir::Format::BGRA8Unorm       : return vir::Format::BGRA8Srgb;
    case vir::Format::RGB8Unorm        : return vir::Format::RGB8Srgb;
    case vir::Format::BGR8Unorm        : return vir::Format::BGR8Srgb;
    case vir::Format::BC1RGBUnormBlock : return vir::Format::BC1RGBSrgbBlock;
    case vir::Format::BC1RGBAUnormBlock: return vir::Format::BC1RGBASrgbBlock;
    case vir::Format::BC2UnormBlock    : return vir::Format::BC2SrgbBlock;
    case vir::Format::BC3UnormBlock    : return vir::Format::BC3SrgbBlock;
    case vir::Format::BC7UnormBlock    : return vir::Format::BC7SrgbBlock;
    default                            : return format;
  }
}

inline auto to_unorm_format(vir::Format format) -> vir::Format {
  switch (format) {
    case vir::Format::RGBA8Srgb       : return vir::Format::RGBA8Unorm;
    case vir::Format::BGRA8Srgb       : return vir::Format::BGRA8Unorm;
    case vir::Format::RGB8Srgb        : return vir::Format::RGB8Unorm;
    case vir::Format::BGR8Srgb        : return vir::Format::BGR8Unorm;
    case vir::Format::BC1RGBSrgbBlock : return vir::Format::BC1RGBUnormBlock;
    case vir::Format::BC1RGBASrgbBlock: return vir::Format::BC1RGBAUnormBlock;
    case vir::Format::BC2SrgbBlock    : return vir::Format::BC2UnormBlock;
    case vir::Format::BC3SrgbBlock    : return vir::Format::BC3UnormBlock;
    case vir::Format::BC7SrgbBlock    : return vir::Format::BC7UnormBlock;
    default                           : return format;
  }
}

inline auto apply_srgb_preference(vir::Format format, bool is_srgb) -> vir::Format {
  return is_srgb ? to_srgb_format(format) : to_unorm_format(format);
}
inline auto format_name(vir::Format format) -> std::string_view {
  switch (format) {
    case vir::Format::R8Unorm               : return "R8_UNORM";
    case vir::Format::RG8Unorm              : return "R8G8_UNORM";
    case vir::Format::RGBA8Unorm            : return "R8G8B8A8_UNORM";
    case vir::Format::RGBA8Srgb             : return "R8G8B8A8_SRGB";
    case vir::Format::BGRA8Unorm            : return "B8G8R8A8_UNORM";
    case vir::Format::BGRA8Srgb             : return "B8G8R8A8_SRGB";
    case vir::Format::R16Unorm              : return "R16_UNORM";
    case vir::Format::R16Sfloat             : return "R16_SFLOAT";
    case vir::Format::RG16Unorm             : return "R16G16_UNORM";
    case vir::Format::RG16Sfloat            : return "R16G16_SFLOAT";
    case vir::Format::RGBA16Unorm           : return "R16G16B16A16_UNORM";
    case vir::Format::RGBA16Sfloat          : return "R16G16B16A16_SFLOAT";
    case vir::Format::R32Sfloat             : return "R32_SFLOAT";
    case vir::Format::RG32Sfloat            : return "R32G32_SFLOAT";
    case vir::Format::RGBA32Sfloat          : return "R32G32B32A32_SFLOAT";
    case vir::Format::A2B10G10R10UnormPack32: return "A2B10G10R10_UNORM_PACK32";
    case vir::Format::B10G11R11UfloatPack32 : return "B10G11R11_UFLOAT_PACK32";
    case vir::Format::E5B9G9R9UfloatPack32  : return "E5B9G9R9_UFLOAT_PACK32";
    case vir::Format::BC1RGBUnormBlock      : return "BC1_RGB_UNORM_BLOCK";
    case vir::Format::BC1RGBSrgbBlock       : return "BC1_RGB_SRGB_BLOCK";
    case vir::Format::BC1RGBAUnormBlock     : return "BC1_RGBA_UNORM_BLOCK";
    case vir::Format::BC1RGBASrgbBlock      : return "BC1_RGBA_SRGB_BLOCK";
    case vir::Format::BC2UnormBlock         : return "BC2_UNORM_BLOCK";
    case vir::Format::BC2SrgbBlock          : return "BC2_SRGB_BLOCK";
    case vir::Format::BC3UnormBlock         : return "BC3_UNORM_BLOCK";
    case vir::Format::BC3SrgbBlock          : return "BC3_SRGB_BLOCK";
    case vir::Format::BC4UnormBlock         : return "BC4_UNORM_BLOCK";
    case vir::Format::BC4SnormBlock         : return "BC4_SNORM_BLOCK";
    case vir::Format::BC5UnormBlock         : return "BC5_UNORM_BLOCK";
    case vir::Format::BC5SnormBlock         : return "BC5_SNORM_BLOCK";
    case vir::Format::BC6HUfloatBlock       : return "BC6H_UFLOAT_BLOCK";
    case vir::Format::BC6HSfloatBlock       : return "BC6H_SFLOAT_BLOCK";
    case vir::Format::BC7UnormBlock         : return "BC7_UNORM_BLOCK";
    case vir::Format::BC7SrgbBlock          : return "BC7_SRGB_BLOCK";
    default                                 : return "other";
  }
}
} // namespace ox
