#pragma once

#include <vuk/Types.hpp>
#include <vuk/runtime/vk/VkTypes.hpp>

#include "Core/Types.hpp"

namespace ox {
enum class AssetType : u32 {
  None = 0,
  Shader,
  Model,
  Texture,
  Material,
  Font,
  Scene,
  Audio,
  Script,
};

// List of file extensions supported by Engine.
enum class AssetFileType : u32 {
  None = 0,
  Binary,
  Meta,
  GLB,
  GLTF,
  PNG,
  JPEG,
  DDS,
  JSON,
  KTX2,
  LUA,
};

struct ShaderAssetEntry {
  vuk::ShaderStageFlags shader_stage = {};
  std::vector<u32> code = {};
};

struct ShaderEntryPointData {
  std::string name = {};
  u32 shader_stage = {};
  std::vector<u32> spirv = {};
};

struct ShaderPipelineData {
  std::string module_name = {};
  std::vector<ShaderEntryPointData> entry_points = {};
};

enum class AssetFileFlags : u32 {
  None = 0,
};
consteval void enable_bitmask(AssetFileFlags);

struct AssetFileHeader {
  static constexpr auto SIGNATURE = 0x584F_u16;
  u16 magic = SIGNATURE; // "OX"
  u16 version = 1;
  AssetType type = AssetType::None;
  AssetFileFlags flags = AssetFileFlags::None;
  u32 entry_count = 0;
};
} // namespace ox
