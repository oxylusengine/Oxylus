#pragma once

// Keep this file as clean as possible, only include this and nothing more:
#include <string_view>

#include "Core/Types.hpp"

namespace ox {
// Specialized string only for Asset Files
struct AssetString {
  u32 length = 0;

  // This should always be null terminated
  union {
    u32 _padding = 0;
    c8 data;
  };

  auto as_sv() -> std::string_view { return std::string_view(&data, length); }
};

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

struct ShaderAsset {
  enum EntryPointKind : u32 {
    Vertex = 0,
    Fragment,
    Compute,
    Count,
  };

  // <Offset, Length> in `spirv_blob`, each element is u32
  std::pair<u32, u32> entry_point_ranges[EntryPointKind::Count] = {};
  AssetString entry_point_names[EntryPointKind::Count] = {};

  auto has_entry_point(EntryPointKind entry_point) -> bool { return entry_point_ranges[entry_point].second != 0; }
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

enum class AssetFileFlags : u32 {
  None = 0,
};
consteval void enable_bitmask(AssetFileFlags);

struct AssetFileHeader {
  c8 magic[2] = {'O', 'X'};
  u16 version = 1;
  AssetFileFlags flags = AssetFileFlags::None;
  AssetType type = AssetType::None;
  union {
    u32 placeholder = 0;
    ShaderAsset shader;
  };
  u8 data = 0;
};
} // namespace ox
