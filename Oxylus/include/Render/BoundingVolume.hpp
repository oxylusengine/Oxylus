#pragma once

#include <glm/vec3.hpp>
#include <glm/mat4x4.hpp>

namespace ox {
class RayCast;
}
namespace ox {
struct Plane;
struct Frustum;

enum Intersection { Outside = 0, Intersects = 1, Inside = 2 };

struct AABB {
  glm::vec3 min = {};
  glm::vec3 max = {};

  AABB() = default;
  ~AABB() = default;
  AABB(const AABB& other) = default;
  AABB(const glm::vec3 min_, const glm::vec3 max_) : min(min_), max(max_) {}

  glm::vec3 get_center() const { return (max + min) * 0.5f; }
  glm::vec3 get_extents() const { return max - min; }
  glm::vec3 get_size() const { return get_extents(); }

  void translate(const glm::vec3& translation);
  void scale(const glm::vec3& scale);
  void rotate(const glm::mat3& rotation);
  void transform(const glm::mat4& transform);
  AABB get_transformed(const glm::mat4& transform) const;

  void merge(const AABB& other);

  bool is_on_or_forward_plane(const Plane& plane) const;
  bool is_on_frustum(const Frustum& frustum) const;
  bool intersects(const glm::vec3& point) const;
  Intersection intersects(const AABB& box) const;
  bool intersects_fast(const AABB& box) const;
  bool intersects(const RayCast& ray) const;
};

struct Sphere {
  glm::vec3 center = {};
  float radius = {};

  Sphere() = default;
  ~Sphere() = default;
  Sphere(const Sphere& other) = default;
  Sphere(const glm::vec3 center_, const float radius_) : center(center_), radius(radius_) {}

  bool intersects(const AABB& b) const;
  bool intersects(const Sphere& b) const;
  bool intersects(const Sphere& b, float& dist) const;
  bool intersects(const Sphere& b, float& dist, glm::vec3& direction) const;
  bool intersects(const RayCast& ray) const;
  bool intersects(const RayCast& ray, float& dist) const;
  bool intersects(const RayCast& ray, float& dist, glm::vec3& direction) const;
};
} // namespace ox
