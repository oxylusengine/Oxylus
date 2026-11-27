#pragma once

#include <atomic>
#include <glm/gtx/quaternion.hpp>

#include "Animation/Fwd.hpp"
#include "Core/UUID.hpp"
#include "Render/Fwd.hpp"
#include "Scene/SceneGPU.hpp"

// repeating the alias avoids dragging flecs.h into every consumer of this header
namespace flecs {
using entity_t = std::uint64_t;
}

namespace ox {
enum class ModelID : u64 { Invalid = std::numeric_limits<u64>::max() };

struct Model {
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
  // nullptr unless the glTF carried a skin, and clips reference the same skeleton asset
  UUID skeleton_uuid = UUID(nullptr);
  std::vector<UUID> animations = {};
  // widest bind-pose reach of any bone, which is what inflates the per-instance culling bounds
  // enough to cover any pose
  f32 max_bone_influence_radius = 0.f;
  std::vector<MeshGroup> mesh_groups = {};
  std::vector<Light> lights = {};
  std::vector<option<u32>> material_indices = {}; // these are per mesh, not per MeshGroup

  // A published model is const, but its meshes stream in after it. These are sized before it is
  // published and never resized; each mesh's entries are written once, by that mesh's upload job,
  // before it sets `mesh_ready[i]`. Read a mesh's entries only after `is_mesh_ready(i)`.
  mutable std::vector<u32> lod0_meshlet_counts = {};
  mutable std::vector<IndexRange> lod0_index_ranges = {};
  mutable std::vector<GPU::Mesh> gpu_meshes = {};
  mutable std::vector<BufferID> gpu_mesh_buffers = {};
  mutable std::vector<CollisionMesh> collision_meshes = {};
  mutable std::vector<std::atomic_flag> mesh_ready = {};
  mutable u32 pending_meshes = 0;

  // built lazily by `AssetManager::resolve_mesh_blas`, each slot claimed with an atomic exchange
  mutable std::vector<AccelerationStructureID> mesh_blases = {};

  usize default_scene_index = 0;

  auto is_mesh_ready(this const Model& self, usize mesh_index) -> bool;
  // for the model's last owner, when its storage slot destroys it
  auto destroy_gpu_resources(this const Model& self, RenderContext& render_context) -> void;
  auto is_fully_loaded(this const Model& self) -> bool;

  auto get_mesh_bounds(this const Model& self) -> GPU::MeshBounds;
};

enum struct MeshInstanceID : u64 { Invalid = ~0_u64 };
struct MeshInstance {
  UUID model_uuid = UUID(nullptr);
  usize mesh_node_index = 0;
  UUID material_uuid = UUID(nullptr);
  GPU::TransformID transform_id = GPU::TransformID::Invalid;
  flecs::entity_t animator_entity = 0;
  bool cast_shadows = true;
  u32 layer_mask = 1;
};

} // namespace ox
