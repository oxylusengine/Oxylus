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
    std::vector<usize> light_indices = {};
    glm::vec3 translation = {};
    glm::quat rotation = {};
    glm::vec3 scale = {};
  };

  enum class LightType { Directional, Spot, Point };

  struct Light {
    std::string name;
    LightType type;
    glm::vec3 color = {1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    ox::option<f32> range = ox::nullopt;
    ox::option<f32> inner_cone_angle = ox::nullopt;
    ox::option<f32> outer_cone_angle = ox::nullopt;
  };

  std::vector<UUID> textures = {};
  std::vector<UUID> materials = {};
  std::vector<MeshGroup> mesh_groups = {};
  std::vector<Light> lights = {};
  std::vector<GPU::Mesh> gpu_meshes = {};
  std::vector<option<u32>> material_indices = {}; // these are per mesh, not per MeshGroup
  std::vector<vuk::Unique<vuk::Buffer>> gpu_mesh_buffers = {};

  usize default_scene_index = 0;
};

enum struct MeshInstanceID : u64 { Invalid = ~0_u64 };
struct MeshInstance {
  UUID model_uuid = UUID(nullptr);
  usize mesh_node_index = 0;
  UUID material_uuid = UUID(nullptr);
  GPU::TransformID transform_id = GPU::TransformID::Invalid;
};

} // namespace ox
