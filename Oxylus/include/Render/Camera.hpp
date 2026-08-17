#pragma once

#include "Physics/RayCast.hpp"
#include "Render/Frustum.hpp"

namespace ox {
struct CameraComponent;
struct TransformComponent;

class Camera {
public:
  static auto update(CameraComponent& component, const TransformComponent& transform, const glm::vec2& screen_size)
    -> void;
  static auto get_frustum(const CameraComponent& component, const glm::vec3& position) -> Frustum;
  static auto get_screen_ray(
    const CameraComponent& component, const glm::vec2& screen_pos, const glm::vec2& screen_size
  ) -> RayCast;
};
} // namespace ox
