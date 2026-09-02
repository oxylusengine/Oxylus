#pragma once

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "Core/Types.hpp"

namespace ox::GPU {
struct MeshletBounds {
  alignas(2) glm::u16vec3 aabb_center = {};
  alignas(1) glm::i8vec2 cone_axis_xy = {};
  alignas(2) glm::u16vec3 aabb_extent = {};
  alignas(1) i8 cone_axis_z = {};
  alignas(1) i8 cone_cutoff = {};
};

struct MeshBounds {
  alignas(4) glm::vec3 aabb_center = {};
  alignas(4) glm::vec3 aabb_extent = {};
};

struct Meshlet {
  alignas(4) u32 indirect_vertex_index_offset = 0;
  alignas(4) u32 local_triangle_index_offset = 0;
  alignas(4) u32 vertex_count = 0;
  alignas(4) u32 triangle_count = 0;
};

// Every u64 here is a device address at runtime, but a blob-relative offset on disk. Anything that
// changes the layout of this struct, MeshLOD, Meshlet or MeshletBounds invalidates every compiled
// model, so bump AssetFileHeader::VERSION with it.
struct MeshLOD {
  alignas(8) u64 indices = 0;
  alignas(8) u64 meshlets = 0;
  alignas(8) u64 meshlet_bounds = 0;
  alignas(8) u64 local_triangle_indices = 0;
  alignas(8) u64 indirect_vertex_indices = 0;

  alignas(4) u32 indices_count = 0;
  alignas(4) u32 meshlet_count = 0;
  alignas(4) u32 meshlet_bounds_count = 0;
  alignas(4) u32 local_triangle_indices_count = 0;
  alignas(4) u32 indirect_vertex_indices_count = 0;

  alignas(4) f32 error = 0.0f;
};

struct Mesh {
  constexpr static auto MAX_LODS = 8_sz;
  constexpr static auto MAX_MESHLET_INDICES = 64_sz;
  constexpr static auto MAX_MESHLET_PRIMITIVES = 64_sz;

  alignas(8) u64 vertex_positions = 0;
  alignas(8) u64 vertex_normals = 0;
  alignas(8) u64 texture_coords = 0;
  alignas(4) u32 vertex_count = 0;
  alignas(4) u32 lod_count = 0;
  alignas(8) u64 lods = 0;
  alignas(4) MeshBounds bounds = {};
};
} // namespace ox::GPU
