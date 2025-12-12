#pragma once

// Keep this file as clean as possible, only include this and nothing more:
#include <span>
#include <string_view>

#include "Asset/AssetMetadata.hpp"
#include "Core/Types.hpp"

namespace ox {
struct AssetDataView {
  u32 begin = 0;
  u32 end = 0;

  auto as_str(std::span<u8> asset_data) -> std::string_view {
    auto* ptr_begin = reinterpret_cast<c8*>(asset_data.data() + begin);
    auto* ptr_end = reinterpret_cast<c8*>(asset_data.data() + end);
    return std::string_view(ptr_begin, ptr_end);
  }

  template <typename T>
  auto as_span(std::span<u8> asset_data) -> std::span<T> {
    auto* ptr_begin = reinterpret_cast<T*>(asset_data.data() + begin);
    auto* ptr_end = reinterpret_cast<T*>(asset_data.data() + end);
    return std::span(ptr_begin, ptr_end);
  }

  auto empty() const -> bool { return begin == 0 && end == 0; }
};

struct ShaderAsset {
  enum EntryPointKind : u32 {
    Vertex = 0,
    Fragment,
    Compute,
    Count,
  };

  AssetDataView entry_points[EntryPointKind::Count] = {};
  AssetDataView entry_point_names[EntryPointKind::Count] = {};

  auto has_entry_point(EntryPointKind entry_point) -> bool { return !entry_points[entry_point].empty(); }
};

union AssetData {
  u32 placeholder = 0;
  ShaderAsset shader;
};

struct AssetFileEntry {
  UUID uuid = {};
  u32 data_size = 0;
  u32 data_offset = 0;
  AssetType type = AssetType::None;
  AssetData asset = {};
};

enum class AssetFileFlags : u32 {
  None = 0,
  Packed = 1 << 0,
};
consteval void enable_bitmask(AssetFileFlags);

struct AssetFileHeader {
  u32 magic = 1129470031; // OXRC
  u16 version = 10;
  AssetFileFlags flags = AssetFileFlags::None;
  u32 asset_count = 0;
};

} // namespace ox
