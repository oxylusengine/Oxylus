#pragma once

namespace ox {
struct Plane {
  glm::vec3 normal = {0.f, 1.f, 0.f}; // unit vector
  float distance = 0.f;               // Distance with origin

  Plane() = default;
  Plane(glm::vec3 norm) : normal(glm::normalize(norm)) {}

  Plane(const glm::vec3& p1, const glm::vec3& norm) : normal(normalize(norm)), distance(dot(normal, p1)) {}

  float get_distance(const glm::vec3& point) const { return dot(normal, point) - distance; }

  bool intersect(const Plane other) const {
    const auto d = glm::cross(normal, other.normal);
    return (glm::dot(d, d) > glm::epsilon<float>()); // EPSILON = 0.0001f
  }
};

struct Frustum {
  Plane top_face;
  Plane bottom_face;

  Plane right_face;
  Plane left_face;

  Plane far_face;
  Plane near_face;

  Plane* planes[6] = {};

  void init() {
    planes[0] = &top_face;
    planes[1] = &bottom_face;
    planes[2] = &right_face;
    planes[3] = &left_face;
    planes[4] = &far_face;
    planes[5] = &near_face;
  }

  bool is_inside(const glm::vec3& point) const {
    for (const auto plane : planes) {
      if (plane->get_distance(point) < 0.0f) {
        return false;
      }
    }

    return true;
  }

  bool intersects(const Frustum& other) const {
    if (top_face.intersect(other.top_face) || bottom_face.intersect(other.bottom_face) ||
        right_face.intersect(other.right_face) || left_face.intersect(other.left_face) ||
        far_face.intersect(other.far_face) || near_face.intersect(other.near_face)) {
      return true;
    }

    return false;
  }

  static Frustum from_matrix(const glm::mat4& view_projection) {
    Frustum frustum = {};

    frustum.near_face = Plane(view_projection[2]);

    // Far plane:
    frustum.far_face = Plane(view_projection[3] - view_projection[2]);

    // Left plane:
    frustum.left_face = Plane(view_projection[3] + view_projection[0]);

    // Right plane:
    frustum.right_face = Plane(view_projection[3] - view_projection[0]);

    // Top plane:
    frustum.top_face = Plane(view_projection[3] - view_projection[1]);

    // Bottom plane:
    frustum.bottom_face = Plane(view_projection[3] + view_projection[1]);

    frustum.init();

    return frustum;
  }
};
} // namespace ox
