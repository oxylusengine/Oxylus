#include "Render/Camera.hpp"

#include "Scene/Components.hpp"

namespace ox {
auto Camera::update(CameraComponent& component, const TransformComponent& transform, const glm::vec2& screen_size)
  -> void {
  ZoneScoped;

  component.jitter_prev = component.jitter;
  component.matrices_prev.projection_matrix = component.matrices.projection_matrix;
  component.matrices_prev.view_matrix = component.matrices.view_matrix;

  component.position = transform.position;

  const auto rotation = glm::normalize(transform.rotation);
  component.forward = glm::normalize(rotation * glm::vec3(0.f, 0.f, -1.f));
  component.right = glm::normalize(rotation * glm::vec3(1.f, 0.f, 0.f));
  component.up = glm::normalize(rotation * glm::vec3(0.f, 1.f, 0.f));

  if (component.tilt != 0.f) {
    const auto roll = glm::angleAxis(component.tilt, component.forward);
    component.right = glm::normalize(roll * component.right);
    component.up = glm::normalize(roll * component.up);
  }

  component.matrices
    .view_matrix = glm::lookAt(component.position, component.position + component.forward, component.up);

  const auto extent = screen_size;
  if (extent.x != 0)
    component.aspect = extent.x / extent.y;
  else
    component.aspect = 1.0f;

  if (component.projection == CameraComponent::Projection::Perspective) {
    component.matrices.projection_matrix = glm::perspective(
      glm::radians(component.fov),
      component.aspect,
      component.far_clip,
      component.near_clip
    ); // reversed-z
  } else {
    component.matrices.projection_matrix = glm::ortho(
      -component.aspect * component.zoom,
      component.aspect * component.zoom,
      -component.zoom,
      component.zoom,
      100.0f,
      -100.0f
    ); // reversed-z
  }

  component.matrices.projection_matrix[1][1] *= -1.0f;
}

auto Camera::get_frustum(const CameraComponent& component, const glm::vec3& position) -> Frustum {
  const float half_v_side = component.far_clip * tanf(glm::radians(component.fov) * .5f);
  const float half_h_side = half_v_side * component.aspect;
  const glm::vec3 forward_far = component.far_clip * component.forward;

  Frustum frustum = {
    .top_face = {position, cross(component.right, forward_far - component.up * half_v_side)},
    .bottom_face = {position, cross(forward_far + component.up * half_v_side, component.right)},
    .right_face = {position, cross(forward_far - component.right * half_h_side, component.up)},
    .left_face = {position, cross(component.up, forward_far + component.right * half_h_side)},
    .far_face = {position + forward_far, -component.forward},
    .near_face = {position + component.near_clip * component.forward, component.forward},
  };

  frustum.init();

  return frustum;
}

auto Camera::get_screen_ray(const CameraComponent& component, const glm::vec2& screen_pos, const glm::vec2& screen_size)
  -> RayCast {
  const glm::mat4 view_inverse = inverse(component.matrices.view_matrix);
  const glm::mat4 proj_inverse = inverse(component.matrices.projection_matrix);

  float screen_x = screen_pos.x / screen_size.x;
  float screen_y = screen_pos.y / screen_size.y;

  screen_x = 2.0f * screen_x - 1.0f;
  screen_y = 2.0f * screen_y - 1.0f;

  // Transform screen coordinates to view space
  glm::vec4 ray_view_near = proj_inverse * glm::vec4(screen_x, screen_y, 0.0f, 1.0f);
  glm::vec4 ray_view_far = proj_inverse * glm::vec4(screen_x, screen_y, 1.0f, 1.0f);

  ray_view_near /= ray_view_near.w;
  ray_view_far /= ray_view_far.w;

  // Transform to world space
  glm::vec3 ray_world_near = glm::vec3(view_inverse * ray_view_near);
  glm::vec3 ray_world_far = glm::vec3(view_inverse * ray_view_far);

  return {ray_world_near, glm::normalize(ray_world_far - ray_world_near)};
}
} // namespace ox
