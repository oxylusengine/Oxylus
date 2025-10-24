#pragma once

// Keep this file as clean as possible, only include this and nothing more:
#include <span>
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
  enum EntryPoint : u32 {
    Vertex = 0,
    Fragment,
    Compute,
    Count,
  };

  // Offsets in `spirv_blob`, each element is u32
  u32 entry_point_offsets[EntryPoint::Count] = {};
  u32 entry_point_lengths[EntryPoint::Count] = {};
  AssetString entry_point_names[EntryPoint::Count] = {};
  u32 spirv_blob = 0;

  auto has_entry_point(EntryPoint entry_point) -> bool { return entry_point_lengths[entry_point] != 0; }
  auto get_entry_point_code(EntryPoint entry_point) -> std::span<u32> {
    return std::span(&spirv_blob + entry_point_offsets[entry_point], entry_point_lengths[entry_point]);
  }
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
};
} // namespace ox
