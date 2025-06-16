#include "OxMath.hpp"

// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Geometry/AABox.h>
#include <glm/gtx/matrix_decompose.hpp>
// clang-format on

namespace ox::math {
bool decompose_transform(const glm::mat4& transform, glm::vec3& translation, glm::vec3& rotation, glm::vec3& scale) {
  ZoneScoped;
  using namespace glm;
  using T = float;

  mat4 local_matrix(transform);

  // Normalize the matrix.
  if (epsilonEqual(local_matrix[3][3], 0.0f, epsilon<T>()))
    return false;

  // First, isolate perspective.  This is the messiest.
  if (epsilonNotEqual(local_matrix[0][3], static_cast<T>(0), epsilon<T>()) ||
      epsilonNotEqual(local_matrix[1][3], static_cast<T>(0), epsilon<T>()) ||
      epsilonNotEqual(local_matrix[2][3], static_cast<T>(0), epsilon<T>())) {
    // Clear the perspective partition
    local_matrix[0][3] = local_matrix[1][3] = local_matrix[2][3] = static_cast<T>(0);
    local_matrix[3][3] = static_cast<T>(1);
  }

  // Next take care of translation (easy).
  translation = glm::vec3(local_matrix[3]);
  local_matrix[3] = vec4(0, 0, 0, local_matrix[3].w);

  glm::vec3 row[3];

  // Now get scale and shear.
  for (length_t i = 0; i < 3; ++i)
    for (length_t j = 0; j < 3; ++j)
      row[i][j] = local_matrix[i][j];

  // Compute X scale factor and normalize first row.
  scale.x = length(row[0]);
  row[0] = detail::scale(row[0], static_cast<T>(1));
  scale.y = length(row[1]);
  row[1] = detail::scale(row[1], static_cast<T>(1));
  scale.z = length(row[2]);
  row[2] = detail::scale(row[2], static_cast<T>(1));

  rotation.y = asin(-row[0][2]);
  if (cos(rotation.y) != 0.0f) {
    rotation.x = atan2(row[1][2], row[2][2]);
    rotation.z = atan2(row[0][1], row[0][0]);
  } else {
    rotation.x = atan2(-row[2][0], row[1][1]);
    rotation.z = 0;
  }

  return true;
}

float lerp(float a, float b, float t) { return a + t * (b - a); }

float inverse_lerp(float a, float b, float value) {
  ZoneScoped;
  const float den = b - a;
  if (den == 0.0f)
    return 0.0f;
  return (value - a) / den;
}

float inverse_lerp_clamped(float a, float b, float value) {
  ZoneScoped;
  const float den = b - a;
  if (den == 0.0f)
    return 0.0f;
  return glm::clamp((value - a) / den, 0.0f, 1.0f);
}

glm::vec2 world_to_screen(const glm::vec3& world_pos,
                          const glm::mat4& mvp,
                          const float width,
                          float height,
                          const float win_pos_x,
                          const float win_pos_y) {
  glm::vec4 trans = mvp * glm::vec4(world_pos, 1.0f);
  trans *= 0.5f / trans.w;
  trans += glm::vec4(0.5f, 0.5f, 0.0f, 0.0f);
  trans.y = 1.f - trans.y;
  trans.x *= width;
  trans.y *= height;
  trans.x += win_pos_x;
  trans.y += win_pos_y;
  return {trans.x, trans.y};
}

glm::vec4 transform(const glm::vec4& vec, const glm::mat4& view) {
  auto result = glm::vec4(vec.z) * view[2] + view[3];
  result = glm::vec4(vec.y) * view[1] + result;
  result = glm::vec4(vec.x) * view[0] + result;
  return result;
}
glm::vec4 transform_normal(const glm::vec4& vec, const glm::mat4& mat) {
  auto result = glm::vec4(vec.z) * mat[2];
  result = glm::vec4(vec.y) * mat[1] + result;
  result = glm::vec4(vec.x) * mat[0] + result;
  return result;
}
glm::vec4 transform_coord(const glm::vec4& vec, const glm::mat4& view) {
  auto result = glm::vec4(vec.z) * view[2] + view[3];
  result = glm::vec4(vec.y) * view[1] + result;
  result = glm::vec4(vec.x) * view[0] + result;
  result = result / glm::vec4(result.w);
  return result;
}

glm::vec3 from_jolt(const JPH::Vec3& vec) { return {vec.GetX(), vec.GetY(), vec.GetZ()}; }
JPH::Vec3 to_jolt(const glm::vec3& vec) { return {vec.x, vec.y, vec.z}; }
glm::vec4 from_jolt(const JPH::Vec4& vec) { return {vec.GetX(), vec.GetY(), vec.GetZ(), vec.GetW()}; }
JPH::Vec4 to_jolt(const glm::vec4& vec) { return {vec.x, vec.y, vec.z, vec.w}; }
AABB from_jolt(const JPH::AABox& aabb) { return {from_jolt(aabb.mMin), from_jolt(aabb.mMax)}; }
} // namespace ox::math
