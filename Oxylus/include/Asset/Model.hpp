#pragma once

#include <vuk/Buffer.hpp>

#include "Core/UUID.hpp"
#include "Scene/SceneGPU.hpp"
namespace ox {

enum class ModelID : u64 { Invalid = std::numeric_limits<u64>::max() };

struct Model {
  constexpr static auto MAX_MESHLET_INDICES = 64_sz;
  constexpr static auto MAX_MESHLET_PRIMITIVES = 64_sz;

  using Index = u32;

  struct Primitive {
    u32 material_index = 0;
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

  std::vector<GPU::Mesh> gpu_meshes = {};
  std::vector<vuk::Unique<vuk::Buffer>> gpu_mesh_buffers = {};
};

} // namespace ox
