#pragma once

#include <atomic>
#include <glm/gtx/quaternion.hpp>
#include <vuk/Buffer.hpp>

#include "Animation/Fwd.hpp"
#include "Core/UUID.hpp"
#include "Render/AccelerationStructure.hpp"
#include "Scene/SceneGPU.hpp"

// repeating the alias avoids dragging flecs.h into every consumer of this header
namespace flecs {
using entity_t = std::uint64_t;
}

namespace ox {
enum class ModelID : u64 { Invalid = std::numeric_limits<u64>::max() };

struct ModelLoadInfo {
  struct Vertex {
    glm::vec3 position = {};
    glm::vec3 normal = {};
    glm::vec2 uv = {};
  };

  std::vector<Vertex> vertices = {};
  std::vector<u32> indices = {};
  std::vector<UUID> materials = {};
};

struct Model {
  constexpr static auto MAX_MESHLET_INDICES = 64_sz;
  constexpr static auto MAX_MESHLET_PRIMITIVES = 64_sz;

  using Index = u32;

  struct MeshGroup {
    std::string name = {};
    std::vector<usize> child_indices = {};
    std::vector<usize> mesh_indices = {};
    std::vector<usize> light_indices = {};
    glm::vec3 translation = {0.f, 0.f, 0.f};
    glm::quat rotation = glm::quat::wxyz(1.f, 0.f, 0.f, 0.f);
    glm::vec3 scale = {1.f, 1.f, 1.f};
  };

  // LOD0 triangles kept CPU side, one entry per mesh, so physics can build a shape. The GPU blob
  // cannot give these back: positions there are half-quantized, reordered into meshlets, and the
  // buffer is device local.
  struct CollisionMesh {
    std::vector<glm::vec3> positions = {};
    std::vector<u32> indices = {};
  };

  struct IndexRange {
    u64 device_address = 0;
    u32 count = 0;
  };

  enum class LightType { Directional, Spot, Point };

  struct Light {
    std::string name;
    LightType type;
    glm::vec3 color = {1.0f, 1.0f, 1.0f};
    f32 intensity = 1.0f;
    ox::option<f32> range = ox::nullopt;
    ox::option<f32> inner_cone_angle = ox::nullopt;
    ox::option<f32> outer_cone_angle = ox::nullopt;
  };

  std::vector<UUID> textures = {};
  std::vector<UUID> materials = {};
  // nil unless the glTF carried a skin, and clips reference the same skeleton asset
  UUID skeleton_uuid = UUID(nullptr);
  std::vector<UUID> animations = {};
  // widest bind-pose reach of any bone, which is what inflates the per-instance culling bounds
  // enough to cover any pose
  f32 max_bone_influence_radius = 0.f;
  std::vector<MeshGroup> mesh_groups = {};
  std::vector<Light> lights = {};
  std::vector<u32> lod0_meshlet_counts = {};
  std::vector<IndexRange> lod0_index_ranges = {};
  std::vector<GPU::Mesh> gpu_meshes = {};
  std::vector<option<u32>> material_indices = {}; // these are per mesh, not per MeshGroup
  std::vector<vuk::Unique<vuk::Buffer>> gpu_mesh_buffers = {};
  std::vector<AccelerationStructure> mesh_blases = {};
  std::vector<CollisionMesh> collision_meshes = {};

  std::vector<std::atomic_flag> mesh_ready = {};
  u32 pending_meshes = 0;

  usize default_scene_index = 0;

  auto is_mesh_ready(this const Model& self, usize mesh_index) -> bool;
  auto is_fully_loaded(this Model& self) -> bool;

  auto get_mesh_bounds(this const Model& self) -> GPU::MeshBounds;
};

enum struct MeshInstanceID : u64 { Invalid = ~0_u64 };
struct MeshInstance {
  UUID model_uuid = UUID(nullptr);
  usize mesh_node_index = 0;
  UUID material_uuid = UUID(nullptr);
  GPU::TransformID transform_id = GPU::TransformID::Invalid;
  flecs::entity_t animator_entity = 0;
};

} // namespace ox
