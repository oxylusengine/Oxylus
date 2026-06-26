#include "Asset/Model.hpp"

namespace ox {
auto Model::get_base_aabb(this const Model& self) -> GPU::Bounds {
  ZoneScoped;

  if (self.gpu_meshes.empty()) {
    return GPU::Bounds{};
  }

  auto global_min = glm::vec3(std::numeric_limits<f32>::max());
  auto global_max = glm::vec3(-std::numeric_limits<f32>::max());

  for (const auto& mesh : self.gpu_meshes) {
    auto mesh_min = mesh.bounds.aabb_center - mesh.bounds.aabb_extent;
    auto mesh_max = mesh.bounds.aabb_center + mesh.bounds.aabb_extent;

    global_min = glm::min(global_min, mesh_min);
    global_max = glm::max(global_max, mesh_max);
  }

  auto base_bounds = GPU::Bounds{};
  base_bounds.aabb_center = (global_min + global_max) * 0.5f;
  base_bounds.aabb_extent = (global_max - global_min) * 0.5f;

  base_bounds.sphere_center = base_bounds.aabb_center;
  base_bounds.sphere_radius = glm::length(base_bounds.aabb_extent);

  return base_bounds;
}
} // namespace ox
