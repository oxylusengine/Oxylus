#pragma once

#include "Frustum.hpp"
#include "Physics/RayCast.hpp"

namespace ox {
struct CameraComponent;

class Camera {
public:
  static void update(CameraComponent& component, const glm::vec2& screen_size);
  static Frustum get_frustum(const CameraComponent& component, const glm::vec3& position);
  static RayCast
  get_screen_ray(const CameraComponent& component, const glm::vec2& screen_pos, const glm::vec2& screen_size);
};
} // namespace ox
