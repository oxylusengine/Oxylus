#include "Asset/Model.hpp"

namespace ox {
auto Model::get_base_aabb(this const Model& self) -> GPU::MeshBounds {
  ZoneScoped;

  if (self.gpu_meshes.empty()) {
    return GPU::MeshBounds{};
  }

  auto global_min = glm::vec3(std::numeric_limits<f32>::max());
  auto global_max = glm::vec3(std::numeric_limits<f32>::lowest());

  for (const auto& mesh : self.gpu_meshes) {
    auto mesh_min = mesh.bounds.aabb_center - mesh.bounds.aabb_extent * 0.5f;
    auto mesh_max = mesh.bounds.aabb_center + mesh.bounds.aabb_extent * 0.5f;

    global_min = glm::min(global_min, mesh_min);
    global_max = glm::max(global_max, mesh_max);
  }

  auto base_bounds = GPU::MeshBounds{};
  base_bounds.aabb_center = (global_min + global_max) * 0.5f;
  base_bounds.aabb_extent = global_max - global_min;

  return base_bounds;
}
} // namespace ox
