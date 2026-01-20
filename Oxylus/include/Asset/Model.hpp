#pragma once

#include <glm/gtx/quaternion.hpp>
#include <vuk/Buffer.hpp>

#include "Core/UUID.hpp"
#include "Scene/SceneGPU.hpp"

namespace ox {

enum class ModelID : u64 { Invalid = std::numeric_limits<u64>::max() };

struct Model {
  constexpr static auto MAX_MESHLET_INDICES = 64_sz;
  constexpr static auto MAX_MESHLET_PRIMITIVES = 64_sz;

  using Index = u32;

  struct MeshGroup {
    std::string name = {};
    std::vector<usize> child_indices = {};
    std::vector<usize> mesh_indices = {};
    std::vector<usize> light_indicies = {};
    glm::vec3 translation = {};
    glm::quat rotation = glm::quat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec3 scale = {};
  };

  struct Scene {
    std::string name = {};
    std::vector<usize> node_indices = {};
  };

  enum class LightType { Directional, Point, Spot };

  struct Light {
    std::string name;
    LightType type;
    glm::vec3 color = {1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    ox::option<f32> range = ox::nullopt;
    ox::option<f32> inner_cone_angle = ox::nullopt;
    ox::option<f32> outer_cone_angle = ox::nullopt;
  };

  std::vector<UUID> embedded_textures = {};
  std::vector<UUID> initial_materials = {};
  std::vector<MeshGroup> mesh_groups = {};
  std::vector<Scene> scenes = {};
  std::vector<Light> lights = {};
  std::vector<GPU::Mesh> gpu_meshes = {};
  std::vector<vuk::Unique<vuk::Buffer>> gpu_mesh_buffers = {};

  usize default_scene_index = 0;
};

} // namespace ox
