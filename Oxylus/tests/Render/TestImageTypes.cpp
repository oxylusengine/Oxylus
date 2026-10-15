#include <bit>
#include <gtest/gtest.h>
#include <type_traits>
#include <vulkan/vulkan_core.h>

#include "Asset/AssetFile.hpp"
#include "Render/Graph/Resource.hpp"
#include "Render/Graph/Types.hpp"

namespace ox {
// include promoted and legacy aliases so SDK changes cannot silently alter asset values
static_assert(static_cast<u32>(vir::Format::Undefined) == VK_FORMAT_UNDEFINED);
static_assert(static_cast<u32>(vir::Format::RG4UnormPack8) == VK_FORMAT_R4G4_UNORM_PACK8);
static_assert(static_cast<u32>(vir::Format::RGBA4UnormPack16) == VK_FORMAT_R4G4B4A4_UNORM_PACK16);
static_assert(static_cast<u32>(vir::Format::BGRA4UnormPack16) == VK_FORMAT_B4G4R4A4_UNORM_PACK16);
static_assert(static_cast<u32>(vir::Format::R5G6B5UnormPack16) == VK_FORMAT_R5G6B5_UNORM_PACK16);
static_assert(static_cast<u32>(vir::Format::B5G6R5UnormPack16) == VK_FORMAT_B5G6R5_UNORM_PACK16);
static_assert(static_cast<u32>(vir::Format::R5G5B5A1UnormPack16) == VK_FORMAT_R5G5B5A1_UNORM_PACK16);
static_assert(static_cast<u32>(vir::Format::B5G5R5A1UnormPack16) == VK_FORMAT_B5G5R5A1_UNORM_PACK16);
static_assert(static_cast<u32>(vir::Format::A1R5G5B5UnormPack16) == VK_FORMAT_A1R5G5B5_UNORM_PACK16);
static_assert(static_cast<u32>(vir::Format::R8Unorm) == VK_FORMAT_R8_UNORM);
static_assert(static_cast<u32>(vir::Format::R8Snorm) == VK_FORMAT_R8_SNORM);
static_assert(static_cast<u32>(vir::Format::R8Uscaled) == VK_FORMAT_R8_USCALED);
static_assert(static_cast<u32>(vir::Format::R8Sscaled) == VK_FORMAT_R8_SSCALED);
static_assert(static_cast<u32>(vir::Format::R8Uint) == VK_FORMAT_R8_UINT);
static_assert(static_cast<u32>(vir::Format::R8Sint) == VK_FORMAT_R8_SINT);
static_assert(static_cast<u32>(vir::Format::R8Srgb) == VK_FORMAT_R8_SRGB);
static_assert(static_cast<u32>(vir::Format::RG8Unorm) == VK_FORMAT_R8G8_UNORM);
static_assert(static_cast<u32>(vir::Format::RG8Snorm) == VK_FORMAT_R8G8_SNORM);
static_assert(static_cast<u32>(vir::Format::RG8Uscaled) == VK_FORMAT_R8G8_USCALED);
static_assert(static_cast<u32>(vir::Format::RG8Sscaled) == VK_FORMAT_R8G8_SSCALED);
static_assert(static_cast<u32>(vir::Format::RG8Uint) == VK_FORMAT_R8G8_UINT);
static_assert(static_cast<u32>(vir::Format::RG8Sint) == VK_FORMAT_R8G8_SINT);
static_assert(static_cast<u32>(vir::Format::RG8Srgb) == VK_FORMAT_R8G8_SRGB);
static_assert(static_cast<u32>(vir::Format::RGB8Unorm) == VK_FORMAT_R8G8B8_UNORM);
static_assert(static_cast<u32>(vir::Format::RGB8Snorm) == VK_FORMAT_R8G8B8_SNORM);
static_assert(static_cast<u32>(vir::Format::RGB8Uscaled) == VK_FORMAT_R8G8B8_USCALED);
static_assert(static_cast<u32>(vir::Format::RGB8Sscaled) == VK_FORMAT_R8G8B8_SSCALED);
static_assert(static_cast<u32>(vir::Format::RGB8Uint) == VK_FORMAT_R8G8B8_UINT);
static_assert(static_cast<u32>(vir::Format::RGB8Sint) == VK_FORMAT_R8G8B8_SINT);
static_assert(static_cast<u32>(vir::Format::RGB8Srgb) == VK_FORMAT_R8G8B8_SRGB);
static_assert(static_cast<u32>(vir::Format::BGR8Unorm) == VK_FORMAT_B8G8R8_UNORM);
static_assert(static_cast<u32>(vir::Format::BGR8Snorm) == VK_FORMAT_B8G8R8_SNORM);
static_assert(static_cast<u32>(vir::Format::BGR8Uscaled) == VK_FORMAT_B8G8R8_USCALED);
static_assert(static_cast<u32>(vir::Format::BGR8Sscaled) == VK_FORMAT_B8G8R8_SSCALED);
static_assert(static_cast<u32>(vir::Format::BGR8Uint) == VK_FORMAT_B8G8R8_UINT);
static_assert(static_cast<u32>(vir::Format::BGR8Sint) == VK_FORMAT_B8G8R8_SINT);
static_assert(static_cast<u32>(vir::Format::BGR8Srgb) == VK_FORMAT_B8G8R8_SRGB);
static_assert(static_cast<u32>(vir::Format::RGBA8Unorm) == VK_FORMAT_R8G8B8A8_UNORM);
static_assert(static_cast<u32>(vir::Format::RGBA8Snorm) == VK_FORMAT_R8G8B8A8_SNORM);
static_assert(static_cast<u32>(vir::Format::RGBA8Uscaled) == VK_FORMAT_R8G8B8A8_USCALED);
static_assert(static_cast<u32>(vir::Format::RGBA8Sscaled) == VK_FORMAT_R8G8B8A8_SSCALED);
static_assert(static_cast<u32>(vir::Format::RGBA8Uint) == VK_FORMAT_R8G8B8A8_UINT);
static_assert(static_cast<u32>(vir::Format::RGBA8Sint) == VK_FORMAT_R8G8B8A8_SINT);
static_assert(static_cast<u32>(vir::Format::RGBA8Srgb) == VK_FORMAT_R8G8B8A8_SRGB);
static_assert(static_cast<u32>(vir::Format::BGRA8Unorm) == VK_FORMAT_B8G8R8A8_UNORM);
static_assert(static_cast<u32>(vir::Format::BGRA8Snorm) == VK_FORMAT_B8G8R8A8_SNORM);
static_assert(static_cast<u32>(vir::Format::BGRA8Uscaled) == VK_FORMAT_B8G8R8A8_USCALED);
static_assert(static_cast<u32>(vir::Format::BGRA8Sscaled) == VK_FORMAT_B8G8R8A8_SSCALED);
static_assert(static_cast<u32>(vir::Format::BGRA8Uint) == VK_FORMAT_B8G8R8A8_UINT);
static_assert(static_cast<u32>(vir::Format::BGRA8Sint) == VK_FORMAT_B8G8R8A8_SINT);
static_assert(static_cast<u32>(vir::Format::BGRA8Srgb) == VK_FORMAT_B8G8R8A8_SRGB);
static_assert(static_cast<u32>(vir::Format::ABGR8UnormPack32) == VK_FORMAT_A8B8G8R8_UNORM_PACK32);
static_assert(static_cast<u32>(vir::Format::ABGR8SnormPack32) == VK_FORMAT_A8B8G8R8_SNORM_PACK32);
static_assert(static_cast<u32>(vir::Format::ABGR8UscaledPack32) == VK_FORMAT_A8B8G8R8_USCALED_PACK32);
static_assert(static_cast<u32>(vir::Format::ABGR8SscaledPack32) == VK_FORMAT_A8B8G8R8_SSCALED_PACK32);
static_assert(static_cast<u32>(vir::Format::ABGR8UintPack32) == VK_FORMAT_A8B8G8R8_UINT_PACK32);
static_assert(static_cast<u32>(vir::Format::ABGR8SintPack32) == VK_FORMAT_A8B8G8R8_SINT_PACK32);
static_assert(static_cast<u32>(vir::Format::ABGR8SrgbPack32) == VK_FORMAT_A8B8G8R8_SRGB_PACK32);
static_assert(static_cast<u32>(vir::Format::A2R10G10B10UnormPack32) == VK_FORMAT_A2R10G10B10_UNORM_PACK32);
static_assert(static_cast<u32>(vir::Format::A2R10G10B10SnormPack32) == VK_FORMAT_A2R10G10B10_SNORM_PACK32);
static_assert(static_cast<u32>(vir::Format::A2R10G10B10UscaledPack32) == VK_FORMAT_A2R10G10B10_USCALED_PACK32);
static_assert(static_cast<u32>(vir::Format::A2R10G10B10SscaledPack32) == VK_FORMAT_A2R10G10B10_SSCALED_PACK32);
static_assert(static_cast<u32>(vir::Format::A2R10G10B10UintPack32) == VK_FORMAT_A2R10G10B10_UINT_PACK32);
static_assert(static_cast<u32>(vir::Format::A2R10G10B10SintPack32) == VK_FORMAT_A2R10G10B10_SINT_PACK32);
static_assert(static_cast<u32>(vir::Format::A2B10G10R10UnormPack32) == VK_FORMAT_A2B10G10R10_UNORM_PACK32);
static_assert(static_cast<u32>(vir::Format::A2B10G10R10SnormPack32) == VK_FORMAT_A2B10G10R10_SNORM_PACK32);
static_assert(static_cast<u32>(vir::Format::A2B10G10R10UscaledPack32) == VK_FORMAT_A2B10G10R10_USCALED_PACK32);
static_assert(static_cast<u32>(vir::Format::A2B10G10R10SscaledPack32) == VK_FORMAT_A2B10G10R10_SSCALED_PACK32);
static_assert(static_cast<u32>(vir::Format::A2B10G10R10UintPack32) == VK_FORMAT_A2B10G10R10_UINT_PACK32);
static_assert(static_cast<u32>(vir::Format::A2B10G10R10SintPack32) == VK_FORMAT_A2B10G10R10_SINT_PACK32);
static_assert(static_cast<u32>(vir::Format::R16Unorm) == VK_FORMAT_R16_UNORM);
static_assert(static_cast<u32>(vir::Format::R16Snorm) == VK_FORMAT_R16_SNORM);
static_assert(static_cast<u32>(vir::Format::R16Uscaled) == VK_FORMAT_R16_USCALED);
static_assert(static_cast<u32>(vir::Format::R16Sscaled) == VK_FORMAT_R16_SSCALED);
static_assert(static_cast<u32>(vir::Format::R16Uint) == VK_FORMAT_R16_UINT);
static_assert(static_cast<u32>(vir::Format::R16Sint) == VK_FORMAT_R16_SINT);
static_assert(static_cast<u32>(vir::Format::R16Sfloat) == VK_FORMAT_R16_SFLOAT);
static_assert(static_cast<u32>(vir::Format::RG16Unorm) == VK_FORMAT_R16G16_UNORM);
static_assert(static_cast<u32>(vir::Format::RG16Snorm) == VK_FORMAT_R16G16_SNORM);
static_assert(static_cast<u32>(vir::Format::RG16Uscaled) == VK_FORMAT_R16G16_USCALED);
static_assert(static_cast<u32>(vir::Format::RG16Sscaled) == VK_FORMAT_R16G16_SSCALED);
static_assert(static_cast<u32>(vir::Format::RG16Uint) == VK_FORMAT_R16G16_UINT);
static_assert(static_cast<u32>(vir::Format::RG16Sint) == VK_FORMAT_R16G16_SINT);
static_assert(static_cast<u32>(vir::Format::RG16Sfloat) == VK_FORMAT_R16G16_SFLOAT);
static_assert(static_cast<u32>(vir::Format::RGB16Unorm) == VK_FORMAT_R16G16B16_UNORM);
static_assert(static_cast<u32>(vir::Format::RGB16Snorm) == VK_FORMAT_R16G16B16_SNORM);
static_assert(static_cast<u32>(vir::Format::RGB16Uscaled) == VK_FORMAT_R16G16B16_USCALED);
static_assert(static_cast<u32>(vir::Format::RGB16Sscaled) == VK_FORMAT_R16G16B16_SSCALED);
static_assert(static_cast<u32>(vir::Format::RGB16Uint) == VK_FORMAT_R16G16B16_UINT);
static_assert(static_cast<u32>(vir::Format::RGB16Sint) == VK_FORMAT_R16G16B16_SINT);
static_assert(static_cast<u32>(vir::Format::RGB16Sfloat) == VK_FORMAT_R16G16B16_SFLOAT);
static_assert(static_cast<u32>(vir::Format::RGBA16Unorm) == VK_FORMAT_R16G16B16A16_UNORM);
static_assert(static_cast<u32>(vir::Format::RGBA16Snorm) == VK_FORMAT_R16G16B16A16_SNORM);
static_assert(static_cast<u32>(vir::Format::RGBA16Uscaled) == VK_FORMAT_R16G16B16A16_USCALED);
static_assert(static_cast<u32>(vir::Format::RGBA16Sscaled) == VK_FORMAT_R16G16B16A16_SSCALED);
static_assert(static_cast<u32>(vir::Format::RGBA16Uint) == VK_FORMAT_R16G16B16A16_UINT);
static_assert(static_cast<u32>(vir::Format::RGBA16Sint) == VK_FORMAT_R16G16B16A16_SINT);
static_assert(static_cast<u32>(vir::Format::RGBA16Sfloat) == VK_FORMAT_R16G16B16A16_SFLOAT);
static_assert(static_cast<u32>(vir::Format::R32Uint) == VK_FORMAT_R32_UINT);
static_assert(static_cast<u32>(vir::Format::R32Sint) == VK_FORMAT_R32_SINT);
static_assert(static_cast<u32>(vir::Format::R32Sfloat) == VK_FORMAT_R32_SFLOAT);
static_assert(static_cast<u32>(vir::Format::RG32Uint) == VK_FORMAT_R32G32_UINT);
static_assert(static_cast<u32>(vir::Format::RG32Sint) == VK_FORMAT_R32G32_SINT);
static_assert(static_cast<u32>(vir::Format::RG32Sfloat) == VK_FORMAT_R32G32_SFLOAT);
static_assert(static_cast<u32>(vir::Format::RGB32Uint) == VK_FORMAT_R32G32B32_UINT);
static_assert(static_cast<u32>(vir::Format::RGB32Sint) == VK_FORMAT_R32G32B32_SINT);
static_assert(static_cast<u32>(vir::Format::RGB32Sfloat) == VK_FORMAT_R32G32B32_SFLOAT);
static_assert(static_cast<u32>(vir::Format::RGBA32Uint) == VK_FORMAT_R32G32B32A32_UINT);
static_assert(static_cast<u32>(vir::Format::RGBA32Sint) == VK_FORMAT_R32G32B32A32_SINT);
static_assert(static_cast<u32>(vir::Format::RGBA32Sfloat) == VK_FORMAT_R32G32B32A32_SFLOAT);
static_assert(static_cast<u32>(vir::Format::R64Uint) == VK_FORMAT_R64_UINT);
static_assert(static_cast<u32>(vir::Format::R64Sint) == VK_FORMAT_R64_SINT);
static_assert(static_cast<u32>(vir::Format::R64Sfloat) == VK_FORMAT_R64_SFLOAT);
static_assert(static_cast<u32>(vir::Format::RG64Uint) == VK_FORMAT_R64G64_UINT);
static_assert(static_cast<u32>(vir::Format::RG64Sint) == VK_FORMAT_R64G64_SINT);
static_assert(static_cast<u32>(vir::Format::RG64Sfloat) == VK_FORMAT_R64G64_SFLOAT);
static_assert(static_cast<u32>(vir::Format::RGB64Uint) == VK_FORMAT_R64G64B64_UINT);
static_assert(static_cast<u32>(vir::Format::RGB64Sint) == VK_FORMAT_R64G64B64_SINT);
static_assert(static_cast<u32>(vir::Format::RGB64Sfloat) == VK_FORMAT_R64G64B64_SFLOAT);
static_assert(static_cast<u32>(vir::Format::RGBA64Uint) == VK_FORMAT_R64G64B64A64_UINT);
static_assert(static_cast<u32>(vir::Format::RGBA64Sint) == VK_FORMAT_R64G64B64A64_SINT);
static_assert(static_cast<u32>(vir::Format::RGBA64Sfloat) == VK_FORMAT_R64G64B64A64_SFLOAT);
static_assert(static_cast<u32>(vir::Format::B10G11R11UfloatPack32) == VK_FORMAT_B10G11R11_UFLOAT_PACK32);
static_assert(static_cast<u32>(vir::Format::E5B9G9R9UfloatPack32) == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32);
static_assert(static_cast<u32>(vir::Format::D16Unorm) == VK_FORMAT_D16_UNORM);
static_assert(static_cast<u32>(vir::Format::X8D24UnormPack32) == VK_FORMAT_X8_D24_UNORM_PACK32);
static_assert(static_cast<u32>(vir::Format::D32Sfloat) == VK_FORMAT_D32_SFLOAT);
static_assert(static_cast<u32>(vir::Format::S8Uint) == VK_FORMAT_S8_UINT);
static_assert(static_cast<u32>(vir::Format::D16UnormS8Uint) == VK_FORMAT_D16_UNORM_S8_UINT);
static_assert(static_cast<u32>(vir::Format::D24UnormS8Uint) == VK_FORMAT_D24_UNORM_S8_UINT);
static_assert(static_cast<u32>(vir::Format::D32SfloatS8Uint) == VK_FORMAT_D32_SFLOAT_S8_UINT);
static_assert(static_cast<u32>(vir::Format::BC1RGBUnormBlock) == VK_FORMAT_BC1_RGB_UNORM_BLOCK);
static_assert(static_cast<u32>(vir::Format::BC1RGBSrgbBlock) == VK_FORMAT_BC1_RGB_SRGB_BLOCK);
static_assert(static_cast<u32>(vir::Format::BC1RGBAUnormBlock) == VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
static_assert(static_cast<u32>(vir::Format::BC1RGBASrgbBlock) == VK_FORMAT_BC1_RGBA_SRGB_BLOCK);
static_assert(static_cast<u32>(vir::Format::BC2UnormBlock) == VK_FORMAT_BC2_UNORM_BLOCK);
static_assert(static_cast<u32>(vir::Format::BC2SrgbBlock) == VK_FORMAT_BC2_SRGB_BLOCK);
static_assert(static_cast<u32>(vir::Format::BC3UnormBlock) == VK_FORMAT_BC3_UNORM_BLOCK);
static_assert(static_cast<u32>(vir::Format::BC3SrgbBlock) == VK_FORMAT_BC3_SRGB_BLOCK);
static_assert(static_cast<u32>(vir::Format::BC4UnormBlock) == VK_FORMAT_BC4_UNORM_BLOCK);
static_assert(static_cast<u32>(vir::Format::BC4SnormBlock) == VK_FORMAT_BC4_SNORM_BLOCK);
static_assert(static_cast<u32>(vir::Format::BC5UnormBlock) == VK_FORMAT_BC5_UNORM_BLOCK);
static_assert(static_cast<u32>(vir::Format::BC5SnormBlock) == VK_FORMAT_BC5_SNORM_BLOCK);
static_assert(static_cast<u32>(vir::Format::BC6HUfloatBlock) == VK_FORMAT_BC6H_UFLOAT_BLOCK);
static_assert(static_cast<u32>(vir::Format::BC6HSfloatBlock) == VK_FORMAT_BC6H_SFLOAT_BLOCK);
static_assert(static_cast<u32>(vir::Format::BC7UnormBlock) == VK_FORMAT_BC7_UNORM_BLOCK);
static_assert(static_cast<u32>(vir::Format::BC7SrgbBlock) == VK_FORMAT_BC7_SRGB_BLOCK);
static_assert(static_cast<u32>(vir::Format::ETC2RGB8UnormBlock) == VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK);
static_assert(static_cast<u32>(vir::Format::ETC2RGB8SrgbBlock) == VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK);
static_assert(static_cast<u32>(vir::Format::ETC2R8G8B8A1UnormBlock) == VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK);
static_assert(static_cast<u32>(vir::Format::ETC2R8G8B8A1SrgbBlock) == VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK);
static_assert(static_cast<u32>(vir::Format::ETC2RGBA8UnormBlock) == VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK);
static_assert(static_cast<u32>(vir::Format::ETC2RGBA8SrgbBlock) == VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK);
static_assert(static_cast<u32>(vir::Format::EACR11UnormBlock) == VK_FORMAT_EAC_R11_UNORM_BLOCK);
static_assert(static_cast<u32>(vir::Format::EACR11SnormBlock) == VK_FORMAT_EAC_R11_SNORM_BLOCK);
static_assert(static_cast<u32>(vir::Format::EACRG11UnormBlock) == VK_FORMAT_EAC_R11G11_UNORM_BLOCK);
static_assert(static_cast<u32>(vir::Format::EACRG11SnormBlock) == VK_FORMAT_EAC_R11G11_SNORM_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC4x4UnormBlock) == VK_FORMAT_ASTC_4x4_UNORM_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC4x4SrgbBlock) == VK_FORMAT_ASTC_4x4_SRGB_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC5x4UnormBlock) == VK_FORMAT_ASTC_5x4_UNORM_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC5x4SrgbBlock) == VK_FORMAT_ASTC_5x4_SRGB_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC5x5UnormBlock) == VK_FORMAT_ASTC_5x5_UNORM_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC5x5SrgbBlock) == VK_FORMAT_ASTC_5x5_SRGB_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC6x5UnormBlock) == VK_FORMAT_ASTC_6x5_UNORM_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC6x5SrgbBlock) == VK_FORMAT_ASTC_6x5_SRGB_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC6x6UnormBlock) == VK_FORMAT_ASTC_6x6_UNORM_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC6x6SrgbBlock) == VK_FORMAT_ASTC_6x6_SRGB_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC8x5UnormBlock) == VK_FORMAT_ASTC_8x5_UNORM_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC8x5SrgbBlock) == VK_FORMAT_ASTC_8x5_SRGB_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC8x6UnormBlock) == VK_FORMAT_ASTC_8x6_UNORM_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC8x6SrgbBlock) == VK_FORMAT_ASTC_8x6_SRGB_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC8x8UnormBlock) == VK_FORMAT_ASTC_8x8_UNORM_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC8x8SrgbBlock) == VK_FORMAT_ASTC_8x8_SRGB_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC10x5UnormBlock) == VK_FORMAT_ASTC_10x5_UNORM_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC10x5SrgbBlock) == VK_FORMAT_ASTC_10x5_SRGB_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC10x6UnormBlock) == VK_FORMAT_ASTC_10x6_UNORM_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC10x6SrgbBlock) == VK_FORMAT_ASTC_10x6_SRGB_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC10x8UnormBlock) == VK_FORMAT_ASTC_10x8_UNORM_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC10x8SrgbBlock) == VK_FORMAT_ASTC_10x8_SRGB_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC10x10UnormBlock) == VK_FORMAT_ASTC_10x10_UNORM_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC10x10SrgbBlock) == VK_FORMAT_ASTC_10x10_SRGB_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC12x10UnormBlock) == VK_FORMAT_ASTC_12x10_UNORM_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC12x10SrgbBlock) == VK_FORMAT_ASTC_12x10_SRGB_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC12x12UnormBlock) == VK_FORMAT_ASTC_12x12_UNORM_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC12x12SrgbBlock) == VK_FORMAT_ASTC_12x12_SRGB_BLOCK);
static_assert(static_cast<u32>(vir::Format::GBGR8422Unorm) == VK_FORMAT_G8B8G8R8_422_UNORM);
static_assert(static_cast<u32>(vir::Format::BGRG8422Unorm) == VK_FORMAT_B8G8R8G8_422_UNORM);
static_assert(static_cast<u32>(vir::Format::G8B8R83Plane420Unorm) == VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM);
static_assert(static_cast<u32>(vir::Format::G8BR82Plane420Unorm) == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM);
static_assert(static_cast<u32>(vir::Format::G8B8R83Plane422Unorm) == VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM);
static_assert(static_cast<u32>(vir::Format::G8BR82Plane422Unorm) == VK_FORMAT_G8_B8R8_2PLANE_422_UNORM);
static_assert(static_cast<u32>(vir::Format::G8B8R83Plane444Unorm) == VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM);
static_assert(static_cast<u32>(vir::Format::R10X6UnormPack16) == VK_FORMAT_R10X6_UNORM_PACK16);
static_assert(static_cast<u32>(vir::Format::R10X6G10X6Unorm2Pack16) == VK_FORMAT_R10X6G10X6_UNORM_2PACK16);
static_assert(
  static_cast<u32>(vir::Format::R10X6G10X6B10X6A10X6Unorm4Pack16) == VK_FORMAT_R10X6G10X6B10X6A10X6_UNORM_4PACK16
);
static_assert(
  static_cast<u32>(vir::Format::G10X6B10X6G10X6R10X6422Unorm4Pack16) == VK_FORMAT_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16
);
static_assert(
  static_cast<u32>(vir::Format::B10X6G10X6R10X6G10X6422Unorm4Pack16) == VK_FORMAT_B10X6G10X6R10X6G10X6_422_UNORM_4PACK16
);
static_assert(
  static_cast<u32>(vir::Format::G10X6B10X6R10X63Plane420Unorm3Pack16) ==
  VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16
);
static_assert(
  static_cast<u32>(vir::Format::G10X6B10X6R10X62Plane420Unorm3Pack16) ==
  VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16
);
static_assert(
  static_cast<u32>(vir::Format::G10X6B10X6R10X63Plane422Unorm3Pack16) ==
  VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16
);
static_assert(
  static_cast<u32>(vir::Format::G10X6B10X6R10X62Plane422Unorm3Pack16) ==
  VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16
);
static_assert(
  static_cast<u32>(vir::Format::G10X6B10X6R10X63Plane444Unorm3Pack16) ==
  VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16
);
static_assert(static_cast<u32>(vir::Format::R12X4UnormPack16) == VK_FORMAT_R12X4_UNORM_PACK16);
static_assert(static_cast<u32>(vir::Format::R12X4G12X4Unorm2Pack16) == VK_FORMAT_R12X4G12X4_UNORM_2PACK16);
static_assert(
  static_cast<u32>(vir::Format::R12X4G12X4B12X4A12X4Unorm4Pack16) == VK_FORMAT_R12X4G12X4B12X4A12X4_UNORM_4PACK16
);
static_assert(
  static_cast<u32>(vir::Format::G12X4B12X4G12X4R12X4422Unorm4Pack16) == VK_FORMAT_G12X4B12X4G12X4R12X4_422_UNORM_4PACK16
);
static_assert(
  static_cast<u32>(vir::Format::B12X4G12X4R12X4G12X4422Unorm4Pack16) == VK_FORMAT_B12X4G12X4R12X4G12X4_422_UNORM_4PACK16
);
static_assert(
  static_cast<u32>(vir::Format::G12X4B12X4R12X43Plane420Unorm3Pack16) ==
  VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16
);
static_assert(
  static_cast<u32>(vir::Format::G12X4B12X4R12X42Plane420Unorm3Pack16) ==
  VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16
);
static_assert(
  static_cast<u32>(vir::Format::G12X4B12X4R12X43Plane422Unorm3Pack16) ==
  VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16
);
static_assert(
  static_cast<u32>(vir::Format::G12X4B12X4R12X42Plane422Unorm3Pack16) ==
  VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16
);
static_assert(
  static_cast<u32>(vir::Format::G12X4B12X4R12X43Plane444Unorm3Pack16) ==
  VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16
);
static_assert(static_cast<u32>(vir::Format::GBGR16422Unorm) == VK_FORMAT_G16B16G16R16_422_UNORM);
static_assert(static_cast<u32>(vir::Format::BGRG16422Unorm) == VK_FORMAT_B16G16R16G16_422_UNORM);
static_assert(static_cast<u32>(vir::Format::G16B16R163Plane420Unorm) == VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM);
static_assert(static_cast<u32>(vir::Format::G16BR162Plane420Unorm) == VK_FORMAT_G16_B16R16_2PLANE_420_UNORM);
static_assert(static_cast<u32>(vir::Format::G16B16R163Plane422Unorm) == VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM);
static_assert(static_cast<u32>(vir::Format::G16BR162Plane422Unorm) == VK_FORMAT_G16_B16R16_2PLANE_422_UNORM);
static_assert(static_cast<u32>(vir::Format::G16B16R163Plane444Unorm) == VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM);
static_assert(static_cast<u32>(vir::Format::G8BR82Plane444Unorm) == VK_FORMAT_G8_B8R8_2PLANE_444_UNORM);
static_assert(
  static_cast<u32>(vir::Format::G10X6B10X6R10X62Plane444Unorm3Pack16) ==
  VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16
);
static_assert(
  static_cast<u32>(vir::Format::G12X4B12X4R12X42Plane444Unorm3Pack16) ==
  VK_FORMAT_G12X4_B12X4R12X4_2PLANE_444_UNORM_3PACK16
);
static_assert(static_cast<u32>(vir::Format::G16BR162Plane444Unorm) == VK_FORMAT_G16_B16R16_2PLANE_444_UNORM);
static_assert(static_cast<u32>(vir::Format::ARGB4UnormPack16) == VK_FORMAT_A4R4G4B4_UNORM_PACK16);
static_assert(static_cast<u32>(vir::Format::ABGR4UnormPack16) == VK_FORMAT_A4B4G4R4_UNORM_PACK16);
static_assert(static_cast<u32>(vir::Format::ASTC4x4SfloatBlock) == VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC5x4SfloatBlock) == VK_FORMAT_ASTC_5x4_SFLOAT_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC5x5SfloatBlock) == VK_FORMAT_ASTC_5x5_SFLOAT_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC6x5SfloatBlock) == VK_FORMAT_ASTC_6x5_SFLOAT_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC6x6SfloatBlock) == VK_FORMAT_ASTC_6x6_SFLOAT_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC8x5SfloatBlock) == VK_FORMAT_ASTC_8x5_SFLOAT_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC8x6SfloatBlock) == VK_FORMAT_ASTC_8x6_SFLOAT_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC8x8SfloatBlock) == VK_FORMAT_ASTC_8x8_SFLOAT_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC10x5SfloatBlock) == VK_FORMAT_ASTC_10x5_SFLOAT_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC10x6SfloatBlock) == VK_FORMAT_ASTC_10x6_SFLOAT_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC10x8SfloatBlock) == VK_FORMAT_ASTC_10x8_SFLOAT_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC10x10SfloatBlock) == VK_FORMAT_ASTC_10x10_SFLOAT_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC12x10SfloatBlock) == VK_FORMAT_ASTC_12x10_SFLOAT_BLOCK);
static_assert(static_cast<u32>(vir::Format::ASTC12x12SfloatBlock) == VK_FORMAT_ASTC_12x12_SFLOAT_BLOCK);
static_assert(static_cast<u32>(vir::Format::A1B5G5R5UnormPack16) == VK_FORMAT_A1B5G5R5_UNORM_PACK16);
static_assert(static_cast<u32>(vir::Format::A8Unorm) == VK_FORMAT_A8_UNORM);
static_assert(static_cast<u32>(vir::Format::PVRTC12BppUnormBlockIMG) == VK_FORMAT_PVRTC1_2BPP_UNORM_BLOCK_IMG);
static_assert(static_cast<u32>(vir::Format::PVRTC14BppUnormBlockIMG) == VK_FORMAT_PVRTC1_4BPP_UNORM_BLOCK_IMG);
static_assert(static_cast<u32>(vir::Format::PVRTC22BppUnormBlockIMG) == VK_FORMAT_PVRTC2_2BPP_UNORM_BLOCK_IMG);
static_assert(static_cast<u32>(vir::Format::PVRTC24BppUnormBlockIMG) == VK_FORMAT_PVRTC2_4BPP_UNORM_BLOCK_IMG);
static_assert(static_cast<u32>(vir::Format::PVRTC12BppSrgbBlockIMG) == VK_FORMAT_PVRTC1_2BPP_SRGB_BLOCK_IMG);
static_assert(static_cast<u32>(vir::Format::PVRTC14BppSrgbBlockIMG) == VK_FORMAT_PVRTC1_4BPP_SRGB_BLOCK_IMG);
static_assert(static_cast<u32>(vir::Format::PVRTC22BppSrgbBlockIMG) == VK_FORMAT_PVRTC2_2BPP_SRGB_BLOCK_IMG);
static_assert(static_cast<u32>(vir::Format::PVRTC24BppSrgbBlockIMG) == VK_FORMAT_PVRTC2_4BPP_SRGB_BLOCK_IMG);
static_assert(static_cast<u32>(vir::Format::R8BoolARM) == VK_FORMAT_R8_BOOL_ARM);
static_assert(static_cast<u32>(vir::Format::RG16Sfixed5NV) == VK_FORMAT_R16G16_SFIXED5_NV);
static_assert(static_cast<u32>(vir::Format::R10X6UintPack16ARM) == VK_FORMAT_R10X6_UINT_PACK16_ARM);
static_assert(static_cast<u32>(vir::Format::R10X6G10X6Uint2Pack16ARM) == VK_FORMAT_R10X6G10X6_UINT_2PACK16_ARM);
static_assert(
  static_cast<u32>(vir::Format::R10X6G10X6B10X6A10X6Uint4Pack16ARM) == VK_FORMAT_R10X6G10X6B10X6A10X6_UINT_4PACK16_ARM
);
static_assert(static_cast<u32>(vir::Format::R12X4UintPack16ARM) == VK_FORMAT_R12X4_UINT_PACK16_ARM);
static_assert(static_cast<u32>(vir::Format::R12X4G12X4Uint2Pack16ARM) == VK_FORMAT_R12X4G12X4_UINT_2PACK16_ARM);
static_assert(
  static_cast<u32>(vir::Format::R12X4G12X4B12X4A12X4Uint4Pack16ARM) == VK_FORMAT_R12X4G12X4B12X4A12X4_UINT_4PACK16_ARM
);
static_assert(static_cast<u32>(vir::Format::R14X2UintPack16ARM) == VK_FORMAT_R14X2_UINT_PACK16_ARM);
static_assert(static_cast<u32>(vir::Format::R14X2G14X2Uint2Pack16ARM) == VK_FORMAT_R14X2G14X2_UINT_2PACK16_ARM);
static_assert(
  static_cast<u32>(vir::Format::R14X2G14X2B14X2A14X2Uint4Pack16ARM) == VK_FORMAT_R14X2G14X2B14X2A14X2_UINT_4PACK16_ARM
);
static_assert(static_cast<u32>(vir::Format::R14X2UnormPack16ARM) == VK_FORMAT_R14X2_UNORM_PACK16_ARM);
static_assert(static_cast<u32>(vir::Format::R14X2G14X2Unorm2Pack16ARM) == VK_FORMAT_R14X2G14X2_UNORM_2PACK16_ARM);
static_assert(
  static_cast<u32>(vir::Format::R14X2G14X2B14X2A14X2Unorm4Pack16ARM) == VK_FORMAT_R14X2G14X2B14X2A14X2_UNORM_4PACK16_ARM
);
static_assert(
  static_cast<u32>(vir::Format::G14X2B14X2R14X22Plane420Unorm3Pack16ARM) ==
  VK_FORMAT_G14X2_B14X2R14X2_2PLANE_420_UNORM_3PACK16_ARM
);
static_assert(
  static_cast<u32>(vir::Format::G14X2B14X2R14X22Plane422Unorm3Pack16ARM) ==
  VK_FORMAT_G14X2_B14X2R14X2_2PLANE_422_UNORM_3PACK16_ARM
);
static_assert(static_cast<u32>(vir::Format::ASTC4x4SfloatBlockEXT) == VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK_EXT);
static_assert(static_cast<u32>(vir::Format::ASTC5x4SfloatBlockEXT) == VK_FORMAT_ASTC_5x4_SFLOAT_BLOCK_EXT);
static_assert(static_cast<u32>(vir::Format::ASTC5x5SfloatBlockEXT) == VK_FORMAT_ASTC_5x5_SFLOAT_BLOCK_EXT);
static_assert(static_cast<u32>(vir::Format::ASTC6x5SfloatBlockEXT) == VK_FORMAT_ASTC_6x5_SFLOAT_BLOCK_EXT);
static_assert(static_cast<u32>(vir::Format::ASTC6x6SfloatBlockEXT) == VK_FORMAT_ASTC_6x6_SFLOAT_BLOCK_EXT);
static_assert(static_cast<u32>(vir::Format::ASTC8x5SfloatBlockEXT) == VK_FORMAT_ASTC_8x5_SFLOAT_BLOCK_EXT);
static_assert(static_cast<u32>(vir::Format::ASTC8x6SfloatBlockEXT) == VK_FORMAT_ASTC_8x6_SFLOAT_BLOCK_EXT);
static_assert(static_cast<u32>(vir::Format::ASTC8x8SfloatBlockEXT) == VK_FORMAT_ASTC_8x8_SFLOAT_BLOCK_EXT);
static_assert(static_cast<u32>(vir::Format::ASTC10x5SfloatBlockEXT) == VK_FORMAT_ASTC_10x5_SFLOAT_BLOCK_EXT);
static_assert(static_cast<u32>(vir::Format::ASTC10x6SfloatBlockEXT) == VK_FORMAT_ASTC_10x6_SFLOAT_BLOCK_EXT);
static_assert(static_cast<u32>(vir::Format::ASTC10x8SfloatBlockEXT) == VK_FORMAT_ASTC_10x8_SFLOAT_BLOCK_EXT);
static_assert(static_cast<u32>(vir::Format::ASTC10x10SfloatBlockEXT) == VK_FORMAT_ASTC_10x10_SFLOAT_BLOCK_EXT);
static_assert(static_cast<u32>(vir::Format::ASTC12x10SfloatBlockEXT) == VK_FORMAT_ASTC_12x10_SFLOAT_BLOCK_EXT);
static_assert(static_cast<u32>(vir::Format::ASTC12x12SfloatBlockEXT) == VK_FORMAT_ASTC_12x12_SFLOAT_BLOCK_EXT);
static_assert(static_cast<u32>(vir::Format::GBGR8422UnormKHR) == VK_FORMAT_G8B8G8R8_422_UNORM_KHR);
static_assert(static_cast<u32>(vir::Format::BGRG8422UnormKHR) == VK_FORMAT_B8G8R8G8_422_UNORM_KHR);
static_assert(static_cast<u32>(vir::Format::G8B8R83Plane420UnormKHR) == VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM_KHR);
static_assert(static_cast<u32>(vir::Format::G8BR82Plane420UnormKHR) == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM_KHR);
static_assert(static_cast<u32>(vir::Format::G8B8R83Plane422UnormKHR) == VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM_KHR);
static_assert(static_cast<u32>(vir::Format::G8BR82Plane422UnormKHR) == VK_FORMAT_G8_B8R8_2PLANE_422_UNORM_KHR);
static_assert(static_cast<u32>(vir::Format::G8B8R83Plane444UnormKHR) == VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM_KHR);
static_assert(static_cast<u32>(vir::Format::R10X6UnormPack16KHR) == VK_FORMAT_R10X6_UNORM_PACK16_KHR);
static_assert(static_cast<u32>(vir::Format::R10X6G10X6Unorm2Pack16KHR) == VK_FORMAT_R10X6G10X6_UNORM_2PACK16_KHR);
static_assert(
  static_cast<u32>(vir::Format::R10X6G10X6B10X6A10X6Unorm4Pack16KHR) == VK_FORMAT_R10X6G10X6B10X6A10X6_UNORM_4PACK16_KHR
);
static_assert(
  static_cast<u32>(vir::Format::G10X6B10X6G10X6R10X6422Unorm4Pack16KHR) ==
  VK_FORMAT_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16_KHR
);
static_assert(
  static_cast<u32>(vir::Format::B10X6G10X6R10X6G10X6422Unorm4Pack16KHR) ==
  VK_FORMAT_B10X6G10X6R10X6G10X6_422_UNORM_4PACK16_KHR
);
static_assert(
  static_cast<u32>(vir::Format::G10X6B10X6R10X63Plane420Unorm3Pack16KHR) ==
  VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16_KHR
);
static_assert(
  static_cast<u32>(vir::Format::G10X6B10X6R10X62Plane420Unorm3Pack16KHR) ==
  VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16_KHR
);
static_assert(
  static_cast<u32>(vir::Format::G10X6B10X6R10X63Plane422Unorm3Pack16KHR) ==
  VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16_KHR
);
static_assert(
  static_cast<u32>(vir::Format::G10X6B10X6R10X62Plane422Unorm3Pack16KHR) ==
  VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16_KHR
);
static_assert(
  static_cast<u32>(vir::Format::G10X6B10X6R10X63Plane444Unorm3Pack16KHR) ==
  VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16_KHR
);
static_assert(static_cast<u32>(vir::Format::R12X4UnormPack16KHR) == VK_FORMAT_R12X4_UNORM_PACK16_KHR);
static_assert(static_cast<u32>(vir::Format::R12X4G12X4Unorm2Pack16KHR) == VK_FORMAT_R12X4G12X4_UNORM_2PACK16_KHR);
static_assert(
  static_cast<u32>(vir::Format::R12X4G12X4B12X4A12X4Unorm4Pack16KHR) == VK_FORMAT_R12X4G12X4B12X4A12X4_UNORM_4PACK16_KHR
);
static_assert(
  static_cast<u32>(vir::Format::G12X4B12X4G12X4R12X4422Unorm4Pack16KHR) ==
  VK_FORMAT_G12X4B12X4G12X4R12X4_422_UNORM_4PACK16_KHR
);
static_assert(
  static_cast<u32>(vir::Format::B12X4G12X4R12X4G12X4422Unorm4Pack16KHR) ==
  VK_FORMAT_B12X4G12X4R12X4G12X4_422_UNORM_4PACK16_KHR
);
static_assert(
  static_cast<u32>(vir::Format::G12X4B12X4R12X43Plane420Unorm3Pack16KHR) ==
  VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16_KHR
);
static_assert(
  static_cast<u32>(vir::Format::G12X4B12X4R12X42Plane420Unorm3Pack16KHR) ==
  VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16_KHR
);
static_assert(
  static_cast<u32>(vir::Format::G12X4B12X4R12X43Plane422Unorm3Pack16KHR) ==
  VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16_KHR
);
static_assert(
  static_cast<u32>(vir::Format::G12X4B12X4R12X42Plane422Unorm3Pack16KHR) ==
  VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16_KHR
);
static_assert(
  static_cast<u32>(vir::Format::G12X4B12X4R12X43Plane444Unorm3Pack16KHR) ==
  VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16_KHR
);
static_assert(static_cast<u32>(vir::Format::GBGR16422UnormKHR) == VK_FORMAT_G16B16G16R16_422_UNORM_KHR);
static_assert(static_cast<u32>(vir::Format::BGRG16422UnormKHR) == VK_FORMAT_B16G16R16G16_422_UNORM_KHR);
static_assert(static_cast<u32>(vir::Format::G16B16R163Plane420UnormKHR) == VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM_KHR);
static_assert(static_cast<u32>(vir::Format::G16BR162Plane420UnormKHR) == VK_FORMAT_G16_B16R16_2PLANE_420_UNORM_KHR);
static_assert(static_cast<u32>(vir::Format::G16B16R163Plane422UnormKHR) == VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM_KHR);
static_assert(static_cast<u32>(vir::Format::G16BR162Plane422UnormKHR) == VK_FORMAT_G16_B16R16_2PLANE_422_UNORM_KHR);
static_assert(static_cast<u32>(vir::Format::G16B16R163Plane444UnormKHR) == VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM_KHR);
static_assert(static_cast<u32>(vir::Format::G8BR82Plane444UnormEXT) == VK_FORMAT_G8_B8R8_2PLANE_444_UNORM_EXT);
static_assert(
  static_cast<u32>(vir::Format::G10X6B10X6R10X62Plane444Unorm3Pack16EXT) ==
  VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16_EXT
);
static_assert(
  static_cast<u32>(vir::Format::G12X4B12X4R12X42Plane444Unorm3Pack16EXT) ==
  VK_FORMAT_G12X4_B12X4R12X4_2PLANE_444_UNORM_3PACK16_EXT
);
static_assert(static_cast<u32>(vir::Format::G16BR162Plane444UnormEXT) == VK_FORMAT_G16_B16R16_2PLANE_444_UNORM_EXT);
static_assert(static_cast<u32>(vir::Format::ARGB4UnormPack16EXT) == VK_FORMAT_A4R4G4B4_UNORM_PACK16_EXT);
static_assert(static_cast<u32>(vir::Format::ABGR4UnormPack16EXT) == VK_FORMAT_A4B4G4R4_UNORM_PACK16_EXT);
static_assert(static_cast<u32>(vir::Format::RG16S105NV) == VK_FORMAT_R16G16_S10_5_NV);
static_assert(static_cast<u32>(vir::Format::A1B5G5R5UnormPack16KHR) == VK_FORMAT_A1B5G5R5_UNORM_PACK16_KHR);
static_assert(static_cast<u32>(vir::Format::A8UnormKHR) == VK_FORMAT_A8_UNORM_KHR);
static_assert(static_cast<u32>(vir::Format::MaxEnum) == VK_FORMAT_MAX_ENUM);
static_assert(static_cast<u32>(vir::ImageCreateFlags::SparseBinding) == VK_IMAGE_CREATE_SPARSE_BINDING_BIT);
static_assert(static_cast<u32>(vir::ImageCreateFlags::SparseResidency) == VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT);
static_assert(static_cast<u32>(vir::ImageCreateFlags::SparseAliased) == VK_IMAGE_CREATE_SPARSE_ALIASED_BIT);
static_assert(static_cast<u32>(vir::ImageCreateFlags::MutableFormat) == VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT);
static_assert(static_cast<u32>(vir::ImageCreateFlags::CubeCompatible) == VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT);
static_assert(static_cast<u32>(vir::ImageCreateFlags::Alias) == VK_IMAGE_CREATE_ALIAS_BIT);
static_assert(
  static_cast<u32>(vir::ImageCreateFlags::SplitInstanceBindRegions) == VK_IMAGE_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT
);
static_assert(static_cast<u32>(vir::ImageCreateFlags::e2DArrayCompatible) == VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT);
static_assert(
  static_cast<u32>(vir::ImageCreateFlags::BlockTexelViewCompatible) == VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT
);
static_assert(static_cast<u32>(vir::ImageCreateFlags::ExtendedUsage) == VK_IMAGE_CREATE_EXTENDED_USAGE_BIT);
static_assert(static_cast<u32>(vir::ImageCreateFlags::Protected) == VK_IMAGE_CREATE_PROTECTED_BIT);
static_assert(static_cast<u32>(vir::ImageCreateFlags::Disjoint) == VK_IMAGE_CREATE_DISJOINT_BIT);
static_assert(static_cast<u32>(vir::ImageCreateFlags::CornerSampledNV) == VK_IMAGE_CREATE_CORNER_SAMPLED_BIT_NV);
static_assert(
  static_cast<u32>(vir::ImageCreateFlags::SampleLocationsCompatibleDepthEXT) ==
  VK_IMAGE_CREATE_SAMPLE_LOCATIONS_COMPATIBLE_DEPTH_BIT_EXT
);
static_assert(static_cast<u32>(vir::ImageCreateFlags::SubsampledEXT) == VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT);
static_assert(
  static_cast<u32>(vir::ImageCreateFlags::DescriptorBufferCaptureReplayEXT) ==
  VK_IMAGE_CREATE_DESCRIPTOR_BUFFER_CAPTURE_REPLAY_BIT_EXT
);
static_assert(
  static_cast<u32>(vir::ImageCreateFlags::MultisampledRenderToSingleSampledEXT) ==
  VK_IMAGE_CREATE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_BIT_EXT
);
static_assert(
  static_cast<u32>(vir::ImageCreateFlags::e2DViewCompatibleEXT) == VK_IMAGE_CREATE_2D_VIEW_COMPATIBLE_BIT_EXT
);
static_assert(
  static_cast<u32>(vir::ImageCreateFlags::VideoProfileIndependentKHR) ==
  VK_IMAGE_CREATE_VIDEO_PROFILE_INDEPENDENT_BIT_KHR
);
static_assert(
  static_cast<u32>(vir::ImageCreateFlags::FragmentDensityMapOffsetEXT) ==
  VK_IMAGE_CREATE_FRAGMENT_DENSITY_MAP_OFFSET_BIT_EXT
);
static_assert(
  static_cast<u32>(vir::ImageCreateFlags::SplitInstanceBindRegionsKHR) ==
  VK_IMAGE_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT_KHR
);
static_assert(
  static_cast<u32>(vir::ImageCreateFlags::e2DArrayCompatibleKHR) == VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT_KHR
);
static_assert(
  static_cast<u32>(vir::ImageCreateFlags::BlockTexelViewCompatibleKHR) ==
  VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT_KHR
);
static_assert(static_cast<u32>(vir::ImageCreateFlags::ExtendedUsageKHR) == VK_IMAGE_CREATE_EXTENDED_USAGE_BIT_KHR);
static_assert(static_cast<u32>(vir::ImageCreateFlags::DisjointKHR) == VK_IMAGE_CREATE_DISJOINT_BIT_KHR);
static_assert(static_cast<u32>(vir::ImageCreateFlags::AliasKHR) == VK_IMAGE_CREATE_ALIAS_BIT_KHR);
static_assert(
  static_cast<u32>(vir::ImageCreateFlags::FragmentDensityMapOffsetQCOM) ==
  VK_IMAGE_CREATE_FRAGMENT_DENSITY_MAP_OFFSET_BIT_QCOM
);
static_assert(static_cast<u32>(vir::ImageUsageFlags::TransferSrc) == VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
static_assert(static_cast<u32>(vir::ImageUsageFlags::TransferDst) == VK_IMAGE_USAGE_TRANSFER_DST_BIT);
static_assert(static_cast<u32>(vir::ImageUsageFlags::Sampled) == VK_IMAGE_USAGE_SAMPLED_BIT);
static_assert(static_cast<u32>(vir::ImageUsageFlags::Storage) == VK_IMAGE_USAGE_STORAGE_BIT);
static_assert(static_cast<u32>(vir::ImageUsageFlags::ColorAttachment) == VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
static_assert(
  static_cast<u32>(vir::ImageUsageFlags::DepthStencilAttachment) == VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
);
static_assert(static_cast<u32>(vir::ImageUsageFlags::TransientAttachment) == VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT);
static_assert(static_cast<u32>(vir::ImageUsageFlags::InputAttachment) == VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT);
static_assert(static_cast<u32>(vir::ImageUsageFlags::HostTransfer) == VK_IMAGE_USAGE_HOST_TRANSFER_BIT);
static_assert(static_cast<u32>(vir::ImageUsageFlags::VideoDecodeDstKHR) == VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR);
static_assert(static_cast<u32>(vir::ImageUsageFlags::VideoDecodeSrcKHR) == VK_IMAGE_USAGE_VIDEO_DECODE_SRC_BIT_KHR);
static_assert(static_cast<u32>(vir::ImageUsageFlags::VideoDecodeDpbKHR) == VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR);
static_assert(
  static_cast<u32>(vir::ImageUsageFlags::FragmentDensityMapEXT) == VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT
);
static_assert(
  static_cast<u32>(vir::ImageUsageFlags::FragmentShadingRateAttachmentKHR) ==
  VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR
);
static_assert(static_cast<u32>(vir::ImageUsageFlags::VideoEncodeDstKHR) == VK_IMAGE_USAGE_VIDEO_ENCODE_DST_BIT_KHR);
static_assert(static_cast<u32>(vir::ImageUsageFlags::VideoEncodeSrcKHR) == VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR);
static_assert(static_cast<u32>(vir::ImageUsageFlags::VideoEncodeDpbKHR) == VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR);
static_assert(
  static_cast<u32>(vir::ImageUsageFlags::AttachmentFeedbackLoopEXT) == VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT
);
static_assert(
  static_cast<u32>(vir::ImageUsageFlags::InvocationMaskHUAWEI) == VK_IMAGE_USAGE_INVOCATION_MASK_BIT_HUAWEI
);
static_assert(static_cast<u32>(vir::ImageUsageFlags::SampleWeightQCOM) == VK_IMAGE_USAGE_SAMPLE_WEIGHT_BIT_QCOM);
static_assert(
  static_cast<u32>(vir::ImageUsageFlags::SampleBlockMatchQCOM) == VK_IMAGE_USAGE_SAMPLE_BLOCK_MATCH_BIT_QCOM
);
static_assert(static_cast<u32>(vir::ImageUsageFlags::TensorAliasingARM) == VK_IMAGE_USAGE_TENSOR_ALIASING_BIT_ARM);
static_assert(static_cast<u32>(vir::ImageUsageFlags::TileMemoryQCOM) == VK_IMAGE_USAGE_TILE_MEMORY_BIT_QCOM);
static_assert(
  static_cast<u32>(vir::ImageUsageFlags::VideoEncodeQuantizationDeltaMapKHR) ==
  VK_IMAGE_USAGE_VIDEO_ENCODE_QUANTIZATION_DELTA_MAP_BIT_KHR
);
static_assert(
  static_cast<u32>(vir::ImageUsageFlags::VideoEncodeEmphasisMapKHR) == VK_IMAGE_USAGE_VIDEO_ENCODE_EMPHASIS_MAP_BIT_KHR
);
static_assert(static_cast<u32>(vir::ImageUsageFlags::ShadingRateImageNV) == VK_IMAGE_USAGE_SHADING_RATE_IMAGE_BIT_NV);
static_assert(static_cast<u32>(vir::ImageUsageFlags::HostTransferEXT) == VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT);
static_assert(
  static_cast<u32>(vir::ImageViewCreateFlags::FragmentDensityMapDynamicEXT) ==
  VK_IMAGE_VIEW_CREATE_FRAGMENT_DENSITY_MAP_DYNAMIC_BIT_EXT
);
static_assert(
  static_cast<u32>(vir::ImageViewCreateFlags::DescriptorBufferCaptureReplayEXT) ==
  VK_IMAGE_VIEW_CREATE_DESCRIPTOR_BUFFER_CAPTURE_REPLAY_BIT_EXT
);
static_assert(
  static_cast<u32>(vir::ImageViewCreateFlags::FragmentDensityMapDeferredEXT) ==
  VK_IMAGE_VIEW_CREATE_FRAGMENT_DENSITY_MAP_DEFERRED_BIT_EXT
);
static_assert(static_cast<u32>(vir::ImageType::e1D) == VK_IMAGE_TYPE_1D);
static_assert(static_cast<u32>(vir::ImageType::e2D) == VK_IMAGE_TYPE_2D);
static_assert(static_cast<u32>(vir::ImageType::e3D) == VK_IMAGE_TYPE_3D);
static_assert(static_cast<u32>(vir::ImageViewType::e1D) == VK_IMAGE_VIEW_TYPE_1D);
static_assert(static_cast<u32>(vir::ImageViewType::e2D) == VK_IMAGE_VIEW_TYPE_2D);
static_assert(static_cast<u32>(vir::ImageViewType::e3D) == VK_IMAGE_VIEW_TYPE_3D);
static_assert(static_cast<u32>(vir::ImageViewType::Cube) == VK_IMAGE_VIEW_TYPE_CUBE);
static_assert(static_cast<u32>(vir::ImageViewType::e1DArray) == VK_IMAGE_VIEW_TYPE_1D_ARRAY);
static_assert(static_cast<u32>(vir::ImageViewType::e2DArray) == VK_IMAGE_VIEW_TYPE_2D_ARRAY);
static_assert(static_cast<u32>(vir::ImageViewType::CubeArray) == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY);
static_assert(static_cast<u32>(vir::ImageViewType::Auto) == VK_IMAGE_VIEW_TYPE_MAX_ENUM);
static_assert(static_cast<u32>(vir::ComponentSwizzle::Identity) == VK_COMPONENT_SWIZZLE_IDENTITY);
static_assert(static_cast<u32>(vir::ComponentSwizzle::Zero) == VK_COMPONENT_SWIZZLE_ZERO);
static_assert(static_cast<u32>(vir::ComponentSwizzle::One) == VK_COMPONENT_SWIZZLE_ONE);
static_assert(static_cast<u32>(vir::ComponentSwizzle::R) == VK_COMPONENT_SWIZZLE_R);
static_assert(static_cast<u32>(vir::ComponentSwizzle::G) == VK_COMPONENT_SWIZZLE_G);
static_assert(static_cast<u32>(vir::ComponentSwizzle::B) == VK_COMPONENT_SWIZZLE_B);
static_assert(static_cast<u32>(vir::ComponentSwizzle::A) == VK_COMPONENT_SWIZZLE_A);
static_assert(static_cast<u32>(vir::Samples::e1) == VK_SAMPLE_COUNT_1_BIT);
static_assert(static_cast<u32>(vir::Samples::e2) == VK_SAMPLE_COUNT_2_BIT);
static_assert(static_cast<u32>(vir::Samples::e4) == VK_SAMPLE_COUNT_4_BIT);
static_assert(static_cast<u32>(vir::Samples::e8) == VK_SAMPLE_COUNT_8_BIT);
static_assert(static_cast<u32>(vir::Samples::e16) == VK_SAMPLE_COUNT_16_BIT);
static_assert(static_cast<u32>(vir::Samples::e32) == VK_SAMPLE_COUNT_32_BIT);
static_assert(static_cast<u32>(vir::Samples::e64) == VK_SAMPLE_COUNT_64_BIT);

static_assert(std::is_same_v<std::underlying_type_t<vir::Format>, u32>);
static_assert(std::is_same_v<std::underlying_type_t<vir::ImageCreateFlags>, u32>);
static_assert(std::is_same_v<std::underlying_type_t<vir::ImageUsageFlags>, u32>);
static_assert(std::is_same_v<std::underlying_type_t<vir::ImageViewCreateFlags>, u32>);
static_assert(std::is_same_v<std::underlying_type_t<vir::ImageType>, u32>);
static_assert(std::is_same_v<std::underlying_type_t<vir::ImageViewType>, u32>);
static_assert(std::is_same_v<std::underlying_type_t<vir::ComponentSwizzle>, u32>);
static_assert(std::is_same_v<std::underlying_type_t<vir::Samples>, u32>);

static_assert(vir::Extent2D{} == vir::Extent2D{0, 0});
static_assert(vir::Extent3D{} == vir::Extent3D{0, 0, 0});
static_assert(
  vir::ComponentMapping{} == vir::ComponentMapping{
                               vir::ComponentSwizzle::Identity,
                               vir::ComponentSwizzle::Identity,
                               vir::ComponentSwizzle::Identity,
                               vir::ComponentSwizzle::Identity
                             }
);

TEST(ImageTypes, AggregateConversionsRoundTrip) {
  for (const auto extent : {vir::Extent2D{}, vir::Extent2D{1, 17}, vir::Extent2D{~0_u32, 4096}}) {
    const auto native = std::bit_cast<VkExtent2D>(extent);
    EXPECT_EQ(native.width, extent.width);
    EXPECT_EQ(native.height, extent.height);
    EXPECT_EQ(std::bit_cast<vir::Extent2D>(native), extent);
  }
  for (const auto extent : {vir::Extent3D{}, vir::Extent3D{13, 29, 5}, vir::Extent3D{~0_u32, 8192, 64}}) {
    const auto native = std::bit_cast<VkExtent3D>(extent);
    EXPECT_EQ(native.width, extent.width);
    EXPECT_EQ(native.height, extent.height);
    EXPECT_EQ(native.depth, extent.depth);
    EXPECT_EQ(std::bit_cast<vir::Extent3D>(native), extent);
  }
  for (const auto components :
       {vir::ComponentMapping{},
        vir::ComponentMapping{
          vir::ComponentSwizzle::R,
          vir::ComponentSwizzle::G,
          vir::ComponentSwizzle::B,
          vir::ComponentSwizzle::A
        },
        vir::ComponentMapping{
          vir::ComponentSwizzle::Zero,
          vir::ComponentSwizzle::One,
          vir::ComponentSwizzle::Identity,
          vir::ComponentSwizzle::R
        }}) {
    const auto native = std::bit_cast<VkComponentMapping>(components);
    EXPECT_EQ(static_cast<vir::ComponentSwizzle>(native.r), components.r);
    EXPECT_EQ(static_cast<vir::ComponentSwizzle>(native.g), components.g);
    EXPECT_EQ(static_cast<vir::ComponentSwizzle>(native.b), components.b);
    EXPECT_EQ(static_cast<vir::ComponentSwizzle>(native.a), components.a);
    EXPECT_EQ(std::bit_cast<vir::ComponentMapping>(native), components);
  }
}

TEST(ImageTypes, StrongFlagOperationsPreserveVulkanMasks) {
  constexpr auto usage = vir::ImageUsageFlags::Sampled | vir::ImageUsageFlags::Storage;
  static_assert(static_cast<VkImageUsageFlags>(usage) == (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT));
  static_assert(vir::contains(usage, vir::ImageUsageFlags::Sampled));
  static_assert(!vir::intersects(usage, vir::ImageUsageFlags::ColorAttachment));
  static_assert(vir::is_empty(usage & vir::ImageUsageFlags::ColorAttachment));
  static_assert(vir::difference(usage, vir::ImageUsageFlags::Sampled) == vir::ImageUsageFlags::Storage);
  auto flags = vir::ImageCreateFlags::MutableFormat;
  flags |= vir::ImageCreateFlags::CubeCompatible;
  EXPECT_EQ(
    static_cast<VkImageCreateFlags>(flags),
    VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT
  );
  flags &= ~vir::ImageCreateFlags::MutableFormat;
  flags ^= vir::ImageCreateFlags::CubeCompatible;
  EXPECT_TRUE(vir::is_empty(flags));
  constexpr auto view = vir::ImageViewCreateFlags::FragmentDensityMapDynamicEXT |
                        vir::ImageViewCreateFlags::FragmentDensityMapDeferredEXT;
  static_assert(
    static_cast<VkImageViewCreateFlags>(view) == (VK_IMAGE_VIEW_CREATE_FRAGMENT_DENSITY_MAP_DYNAMIC_BIT_EXT |
                                                  VK_IMAGE_VIEW_CREATE_FRAGMENT_DENSITY_MAP_DEFERRED_BIT_EXT)
  );
}

TEST(ImageTypes, FormatsPreserveCompressedBlockSizing) {
  EXPECT_EQ(vir::image_byte_size(vir::Format::BC7SrgbBlock, {5, 7, 2}), 128_u64);
  EXPECT_EQ(vir::image_byte_size(vir::Format::BC4UnormBlock, {5, 7, 1}), 32_u64);
  EXPECT_EQ(vir::format_texel_block_extent(vir::Format::BC5SnormBlock), (vir::Extent2D{4, 4}));
  EXPECT_EQ(vir::image_byte_size(vir::Format::RGB8Srgb, {5, 7, 1}), 105_u64);
}

TEST(ImageTypes, VertexFormatsKeepTheLegacySerializedBytes) {
  struct LegacyVertexInput {
    u32 location = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    u32 size = 0;
  };
  const auto legacy = LegacyVertexInput{2, VK_FORMAT_R32G32B32_SFLOAT, 12};
  const auto current = vir::VertexInput{2, vir::Format::RGB32Sfloat, 12};
  auto legacy_bytes = std::vector<std::byte>{};
  auto current_bytes = std::vector<std::byte>{};
  auto legacy_out = zpp::bits::out(legacy_bytes);
  auto current_out = zpp::bits::out(current_bytes);
  ASSERT_FALSE(zpp::bits::failure(legacy_out(legacy)));
  ASSERT_FALSE(zpp::bits::failure(current_out(current)));
  EXPECT_EQ(legacy_bytes, current_bytes);
  auto decoded = vir::VertexInput{};
  auto input = zpp::bits::in(legacy_bytes);
  ASSERT_FALSE(zpp::bits::failure(input(decoded)));
  EXPECT_EQ(decoded, current);
}
} // namespace ox
