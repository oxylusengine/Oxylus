#pragma once

#include <algorithm>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <limits>

#include "Asset/Model.hpp"
#include "Render/BoundingVolume.hpp"

namespace ox {

class ThumbnailCamera {
public:
  struct CameraTransform {
    glm::vec3 position;
    glm::quat rotation;
    f32 pitch;
    f32 yaw;
    f32 near_clip;
    f32 far_clip;
  };

  static auto calculate(const AABB& model_aabb, f32 fov_y_degrees = 45.0f, f32 aspect_ratio = 1.0f) -> CameraTransform {
    CameraTransform transform{};

    const glm::vec3 center = model_aabb.get_center();
    glm::vec3 extents = model_aabb.get_extents();

    extents.x = std::max(extents.x, 0.01f);
    extents.y = std::max(extents.y, 0.01f);
    extents.z = std::max(extents.z, 0.01f);

    const glm::vec3 min = center - extents;
    const glm::vec3 max = center + extents;

    const glm::vec3 corners[8] = {
      glm::vec3(min.x, min.y, min.z),
      glm::vec3(max.x, min.y, min.z),
      glm::vec3(min.x, max.y, min.z),
      glm::vec3(max.x, max.y, min.z),
      glm::vec3(min.x, min.y, max.z),
      glm::vec3(max.x, min.y, max.z),
      glm::vec3(min.x, max.y, max.z),
      glm::vec3(max.x, max.y, max.z)
    };

    transform.pitch = glm::radians(-25.0f);
    transform.yaw = glm::radians(-130.0f);

    const f32 cos_pitch = glm::cos(transform.pitch);
    const f32 sin_pitch = glm::sin(transform.pitch);
    const f32 cos_yaw = glm::cos(transform.yaw);
    const f32 sin_yaw = glm::sin(transform.yaw);

    glm::vec3 forward;
    forward.x = cos_yaw * cos_pitch;
    forward.y = sin_pitch;
    forward.z = sin_yaw * cos_pitch;
    forward = glm::normalize(forward);

    const glm::vec3 world_up = glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::vec3 right = glm::normalize(glm::cross(forward, world_up));
    const glm::vec3 up = glm::normalize(glm::cross(right, forward));

    // Projects all 8 corners onto the camera's local coordinate axes relative to the model center
    // Finds the max distance required to fit every single corner in the frustum bounds
    //
    f32 x_local[8];
    f32 y_local[8];
    f32 z_local[8];
    for (i32 i = 0; i < 8; ++i) {
      const glm::vec3 relative_pos = corners[i] - center;
      x_local[i] = glm::dot(relative_pos, right);
      y_local[i] = glm::dot(relative_pos, up);
      z_local[i] = glm::dot(relative_pos, forward);
    }

    const f32 safe_aspect = std::max(aspect_ratio, 0.01f);
    const f32 half_fov_y_rad = glm::radians(std::clamp(fov_y_degrees, 1.0f, 179.0f) * 0.5f);
    const f32 tan_half_fov_y = std::max(glm::tan(half_fov_y_rad), 0.0001f);
    const f32 tan_half_fov_x = std::max(tan_half_fov_y * safe_aspect, 0.0001f);

    f32 max_distance = 0.0f;
    for (i32 i = 0; i < 8; ++i) {
      const f32 dist_x = std::abs(x_local[i]) / tan_half_fov_x;
      const f32 dist_y = std::abs(y_local[i]) / tan_half_fov_y;

      const f32 required_dist_for_corner = std::max(dist_x, dist_y) - z_local[i];
      max_distance = std::max(max_distance, required_dist_for_corner);
    }

    const f32 frame_padding = 1.10f;
    max_distance *= frame_padding;

    transform.position = center - forward * max_distance;

    const glm::mat4 view_matrix = glm::lookAt(transform.position, center, glm::vec3(0.0f, 1.0f, 0.0f));
    transform.rotation = glm::quat_cast(glm::inverse(view_matrix));

    f32 min_depth_local = std::numeric_limits<f32>::max();
    f32 max_depth_local = std::numeric_limits<f32>::lowest();
    for (i32 i = 0; i < 8; ++i) {
      const f32 corner_depth = z_local[i] + max_distance;
      min_depth_local = std::min(min_depth_local, corner_depth);
      max_depth_local = std::max(max_depth_local, corner_depth);
    }

    transform.near_clip = std::max(0.01f, min_depth_local * 0.95f);
    transform.far_clip = max_depth_local * 1.05f;

    return transform;
  }

  static auto calculate_from_model(const Model& model, f32 fov_y_degrees = 45.0f, f32 aspect_ratio = 1.0f)
    -> CameraTransform {
    struct WorldAABB {
      glm::vec3 min = glm::vec3(std::numeric_limits<f32>::max());
      glm::vec3 max = glm::vec3(std::numeric_limits<f32>::lowest());
      bool valid = false;

      void expand(const glm::vec3& p) noexcept {
        min = glm::min(min, p);
        max = glm::max(max, p);
        valid = true;
      }
    } world_aabb;

    if (!model.mesh_groups.empty()) {
      get_transformed_aabb_recursive(model, 0, glm::mat4(1.0f), world_aabb);
    }

    AABB final_aabb{};
    if (world_aabb.valid && glm::length(world_aabb.max - world_aabb.min) > 0.001f) {
      final_aabb.min = world_aabb.min;
      final_aabb.max = world_aabb.max;
    } else {
      auto base_aabb = AABB(model.get_base_aabb());
      final_aabb.min = base_aabb.min;
      final_aabb.max = base_aabb.max;

      if (glm::length(final_aabb.max - final_aabb.min) < 0.001f) {
        final_aabb.min = glm::vec3(-1.0f);
        final_aabb.max = glm::vec3(1.0f);
      }
    }

    return calculate(final_aabb, fov_y_degrees, aspect_ratio);
  }

private:
  static auto get_transformed_aabb_recursive(
    const Model& model, size_t group_index, const glm::mat4& parent_transform, auto& out_aabb
  ) -> void {
    if (group_index >= model.mesh_groups.size())
      return;
    const auto& group = model.mesh_groups[group_index];

    glm::vec3 scale = group.scale;
    if (glm::length(scale) < 0.001f) {
      scale = glm::vec3(1.0f);
    }

    glm::mat4 local_transform = glm::mat4(1.0f);
    local_transform = glm::translate(local_transform, group.translation);
    local_transform = local_transform * glm::mat4_cast(group.rotation);
    local_transform = glm::scale(local_transform, scale);

    glm::mat4 world_transform = parent_transform * local_transform;

    for (size_t mesh_index : group.mesh_indices) {
      if (mesh_index >= model.gpu_meshes.size())
        continue;
      const auto& mesh = model.gpu_meshes[mesh_index];

      glm::vec3 half_extent = mesh.bounds.aabb_extent * 0.5f;
      glm::vec3 local_min = mesh.bounds.aabb_center - half_extent;
      glm::vec3 local_max = mesh.bounds.aabb_center + half_extent;

      glm::vec3 local_corners[8] = {
        glm::vec3(local_min.x, local_min.y, local_min.z),
        glm::vec3(local_max.x, local_min.y, local_min.z),
        glm::vec3(local_min.x, local_max.y, local_min.z),
        glm::vec3(local_max.x, local_max.y, local_min.z),
        glm::vec3(local_min.x, local_min.y, local_max.z),
        glm::vec3(local_max.x, local_min.y, local_max.z),
        glm::vec3(local_min.x, local_max.y, local_max.z),
        glm::vec3(local_max.x, local_max.y, local_max.z)
      };

      for (i32 i = 0; i < 8; ++i) {
        glm::vec4 world_corner = world_transform * glm::vec4(local_corners[i], 1.0f);
        out_aabb.expand(glm::vec3(world_corner));
      }
    }

    for (size_t child_index : group.child_indices) {
      get_transformed_aabb_recursive(model, child_index, world_transform, out_aabb);
    }
  }
};

} // namespace ox
