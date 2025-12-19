#pragma once

// Keep this file as clean as possible, only include this and nothing more:
#include <glm/ext/quaternion_float.hpp>
#include <span>
#include <string_view>

#include "Asset/AssetMetadata.hpp"
#include "Core/Types.hpp"

namespace ox {
template <typename T>
struct AssetDataView {
  u32 begin = 0;
  u32 end = 0;

  auto as_str(std::span<u8> asset_data) -> std::string_view {
    auto* ptr_begin = reinterpret_cast<c8*>(asset_data.data() + begin);
    auto* ptr_end = reinterpret_cast<c8*>(asset_data.data() + end);
    return std::string_view(ptr_begin, ptr_end);
  }

  auto as_span(std::span<u8> asset_data) -> std::span<T> {
    auto* ptr_begin = reinterpret_cast<T*>(asset_data.data() + begin);
    auto* ptr_end = reinterpret_cast<T*>(asset_data.data() + end);
    return std::span(ptr_begin, ptr_end);
  }

  auto at(std::span<u8> asset_data, u32 index) -> T* {
    if (index >= end * sizeof(T)) {
      return nullptr;
    }

    auto* ptr_begin = reinterpret_cast<T*>(asset_data.data() + begin);
    return ptr_begin + static_cast<usize>(index);
  }

  auto empty() const -> bool { return begin == 0 && end == 0; }
  auto size() const -> usize { return (end * sizeof(T)) - (begin * sizeof(T)); }
  auto size_bytes() const -> usize { return end - begin; }
};

struct ShaderAsset {
  enum EntryPointKind : u32 {
    Vertex = 0,
    Fragment,
    Compute,
    Count,
  };

  AssetDataView<u32> entry_points[EntryPointKind::Count] = {};
  AssetDataView<c8> entry_point_names[EntryPointKind::Count] = {};

  auto has_entry_point(EntryPointKind entry_point) -> bool { return !entry_points[entry_point].empty(); }
};

struct ModelAsset {
  struct Meshlet {
    u32 indirect_vertex_index_offset = 0;
    u32 local_triangle_index_offset = 0;
    u32 vertex_count = 0;
    u32 triangle_count = 0;
  };

  struct Bounds {
    glm::vec3 aabb_center = {};
    glm::vec3 aabb_extent = {};
    glm::vec3 sphere_center = {};
    f32 sphere_radius = 0.0f;
  };

  struct Lod {
    AssetDataView<u32> indices = {};
    AssetDataView<Meshlet> meshlets = {};
    AssetDataView<Bounds> meshlet_bounds = {};
    AssetDataView<u8> local_triangle_indices = {};
    AssetDataView<u32> indirect_vertex_indices = {};
    f32 error = 0.0f;
  };

  struct Node {
    AssetDataView<c8> name = {};
    AssetDataView<u32> child_indices = {};
    AssetDataView<u32> mesh_indices = {};
    glm::vec3 translation = {};
    glm::quat rotation = glm::quat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec3 scale = {};
  };

  struct Mesh {
    AssetDataView<glm::vec3> vertex_positions = {};
    AssetDataView<glm::vec3> vertex_normals = {};
    AssetDataView<glm::vec2> vertex_texcoords = {};
    AssetDataView<Lod> lods = {};
    Bounds bounds = {};
  };

  AssetDataView<Node> nodes = {};
  AssetDataView<Mesh> meshes = {};
};

union AssetData {
  u32 placeholder = 0;
  ShaderAsset shader;
  ModelAsset model;
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
