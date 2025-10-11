#pragma once

#include <glm/vec3.hpp>

namespace ox {
class RayCast {
public:
  float t_min = 0;
  float t_max = std::numeric_limits<float>::max();

  RayCast() = default;
  RayCast(const glm::vec3 ray_origin, const glm::vec3 ray_direction) : origin(ray_origin), direction(ray_direction) {}

  glm::vec3 get_point_on_ray(const float fraction) const { return origin + fraction * direction; }

  const glm::vec3& get_origin() const { return origin; }
  const glm::vec3& get_direction() const { return direction; }
  glm::vec3 get_direction_inverse() const { return 1.0f / direction; }

private:
  glm::vec3 origin = {};
  glm::vec3 direction = {};
};
} // namespace ox
