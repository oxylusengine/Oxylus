#pragma once

namespace ox {
enum class AssetType : u32 {
  None = 0,
  Shader,
  Mesh,
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
  JSON,
  KTX2,
  LUA,
};

enum class AssetFileFlags : u64 {
  None = 0,
};
consteval void enable_bitmask(AssetFileFlags);

struct TextureAssetFileHeader {
  vuk::Extent3D extent = {};
  vuk::Format format = vuk::Format::eUndefined;
};

struct AssetFileHeader {
  c8 magic[2] = {'O', 'X'};
  u16 version = 1;
  AssetFileFlags flags = AssetFileFlags::None;
  AssetType type = AssetType::None;
  union {
    TextureAssetFileHeader texture_header = {};
  };
};
} // namespace ox
