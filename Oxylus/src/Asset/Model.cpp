#include "Asset/Model.hpp"

namespace ox {
auto Model::is_mesh_ready(this const Model& self, usize mesh_index) -> bool {
  if (mesh_index >= self.mesh_ready.size()) {
    return false;
  }

  return self.mesh_ready[mesh_index].test(std::memory_order_acquire);
}

auto Model::is_fully_loaded(this Model& self) -> bool {
  return std::atomic_ref(self.pending_meshes).load(std::memory_order_acquire) == 0;
}

auto Model::get_mesh_bounds(this const Model& self) -> GPU::MeshBounds {
  ZoneScoped;

  if (self.gpu_meshes.empty()) {
    return GPU::MeshBounds{};
  }

  auto global_min = glm::vec3(std::numeric_limits<f32>::max());
  auto global_max = glm::vec3(std::numeric_limits<f32>::lowest());

  auto any_ready = false;
  for (auto mesh_index = 0_sz; mesh_index < self.gpu_meshes.size(); mesh_index++) {
    if (!self.is_mesh_ready(mesh_index)) {
      continue;
    }

    const auto& mesh = self.gpu_meshes[mesh_index];
    any_ready = true;
    auto mesh_min = mesh.bounds.aabb_center - mesh.bounds.aabb_extent * 0.5f;
    auto mesh_max = mesh.bounds.aabb_center + mesh.bounds.aabb_extent * 0.5f;

    global_min = glm::min(global_min, mesh_min);
    global_max = glm::max(global_max, mesh_max);
  }

  if (!any_ready) {
    return GPU::MeshBounds{};
  }

  auto base_bounds = GPU::MeshBounds{};
  base_bounds.aabb_center = (global_min + global_max) * 0.5f;
  base_bounds.aabb_extent = global_max - global_min;

  return base_bounds;
}
} // namespace ox
