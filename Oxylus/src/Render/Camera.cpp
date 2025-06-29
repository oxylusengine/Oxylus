#include "Render/Camera.hpp"

#include "Scene/ECSModule/Core.hpp"

namespace ox {
void Camera::update(CameraComponent& component, const glm::vec2& screen_size) {
  ZoneScoped;

  component.jitter_prev = component.jitter;
  component.matrices_prev.projection_matrix = component.matrices.projection_matrix;
  component.matrices_prev.view_matrix = component.matrices.view_matrix;

  const float cos_pitch = glm::cos(component.pitch); // x
  const float sin_pitch = glm::sin(component.pitch); // x
  const float cos_yaw = glm::cos(component.yaw);     // y
  const float sin_yaw = glm::sin(component.yaw);     // y

  component.forward.x = cos_yaw * cos_pitch;
  component.forward.y = sin_pitch;
  component.forward.z = sin_yaw * cos_pitch;

  component.forward = glm::normalize(component.forward);
  component.right = glm::normalize(glm::cross(component.forward, {component.tilt, 1, component.tilt}));
  component.up = glm::normalize(glm::cross(component.right, component.forward));

  component.matrices.view_matrix = glm::lookAt(
      component.position, component.position + component.forward, component.up);

  const auto extent = screen_size;
  if (extent.x != 0)
    component.aspect = extent.x / extent.y;
  else
    component.aspect = 1.0f;

  if (component.projection == CameraComponent::Projection::Perspective) {
    component.matrices.projection_matrix = glm::perspective(glm::radians(component.fov),
                                                            component.aspect,
                                                            component.near_clip,
                                                            component.far_clip); // reversed-z
  } else {
    component.matrices.projection_matrix = glm::ortho(-component.aspect * component.zoom,
                                                      component.aspect * component.zoom,
                                                      -component.zoom,
                                                      component.zoom,
                                                      100.0f,
                                                      -100.0f); // reversed-z
  }

  component.matrices.projection_matrix[1][1] *= -1.0f;
}

Frustum Camera::get_frustum(const CameraComponent& component, const glm::vec3& position) {
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

RayCast
Camera::get_screen_ray(const CameraComponent& component, const glm::vec2& screen_pos, const glm::vec2& screen_size) {
  const glm::mat4 view_proj_inverse = inverse(component.matrices.projection_matrix * component.matrices.view_matrix);

  float screen_x = screen_pos.x / screen_size.x;
  float screen_y = screen_pos.y / screen_size.y;

  screen_x = 2.0f * screen_x - 1.0f;
  screen_y = 2.0f * screen_y - 1.0f;

  glm::vec4 n = view_proj_inverse * glm::vec4(screen_x, screen_y, 0.0f, 1.0f);
  n /= n.w;

  glm::vec4 f = view_proj_inverse * glm::vec4(screen_x, screen_y, 1.0f, 1.0f);
  f /= f.w;

  return {glm::vec3(n), glm::normalize(glm::vec3(f) - glm::vec3(n))};
}
} // namespace ox
