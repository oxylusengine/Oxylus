#pragma once

#include <vuk/Buffer.hpp>

#include "Core/UUID.hpp"
namespace ox {

enum class MeshID : u64 { Invalid = std::numeric_limits<u64>::max() };

struct Mesh {
  constexpr static auto MAX_MESHLET_INDICES = 64_sz;
  constexpr static auto MAX_MESHLET_PRIMITIVES = 64_sz;

  using Index = u32;

  struct Primitive {
    u32 material_index = 0;
    u32 meshlet_count = 0;
    u32 meshlet_offset = 0;
    u32 local_triangle_indices_offset = 0;
    u32 vertex_count = 0;
    u32 vertex_offset = 0;
    u32 index_count = 0;
    u32 index_offset = 0;
  };

  struct GLTFMesh {
    std::string name = {};
    std::vector<u32> primitive_indices = {};
  };

  struct Node {
    std::string name = {};
    std::vector<usize> child_indices = {};
    ox::option<usize> mesh_index = ox::nullopt;
    glm::vec3 translation = {};
    glm::quat rotation = {};
    glm::vec3 scale = {};
  };

  struct Scene {
    std::string name = {};
    std::vector<usize> node_indices = {};
  };

  std::vector<UUID> embedded_textures = {};
  std::vector<UUID> materials = {};
  std::vector<Primitive> primitives = {};
  std::vector<GLTFMesh> meshes = {};
  std::vector<Node> nodes = {};
  std::vector<Scene> scenes = {};

  usize default_scene_index = 0;

  usize indices_count = 0;

  vuk::Unique<vuk::Buffer> indices = vuk::Unique<vuk::Buffer>();
  vuk::Unique<vuk::Buffer> vertex_positions = vuk::Unique<vuk::Buffer>();
  vuk::Unique<vuk::Buffer> vertex_normals = vuk::Unique<vuk::Buffer>();
  vuk::Unique<vuk::Buffer> texture_coords = vuk::Unique<vuk::Buffer>();
  vuk::Unique<vuk::Buffer> meshlets = vuk::Unique<vuk::Buffer>();
  vuk::Unique<vuk::Buffer> meshlet_bounds = vuk::Unique<vuk::Buffer>();
  vuk::Unique<vuk::Buffer> local_triangle_indices = vuk::Unique<vuk::Buffer>();
};

} // namespace ox
