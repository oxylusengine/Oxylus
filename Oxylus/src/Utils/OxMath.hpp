#pragma once

#include "Render/BoundingVolume.hpp"

namespace JPH {
class AABox;
class Vec3;
class Vec4;
} // namespace JPH

namespace ox::math {
inline u32 flooru32(float value) {
  i64 value_container = (i64)floorf(value);
  const u32 v = (u32)value_container;
  return v;
}

inline u32 pack_u16(u16 low, u16 high) {
  u32 result = 0;
  result |= low;
  result |= high << 16u;
  return result;
}

inline u16 unpack_u32_low(u32 packed) { return packed & 0xFFFF; }
inline u16 unpack_u32_high(u32 packed) { return (packed >> 16) & 0xFFFF; }

inline glm::vec2 sign_not_zero(glm::vec2 v) { return {(v.x >= 0.0f) ? +1.0f : -1.0f, (v.y >= 0.0f) ? +1.0f : -1.0f}; }

inline glm::vec2 float32x3_to_oct(glm::vec3 v) {
  const glm::vec2 p = glm::vec2{v.x, v.y} * (1.0f / (glm::abs(v.x) + glm::abs(v.y) + glm::abs(v.z)));
  return (v.z <= 0.0f) ? ((1.0f - glm::abs(glm::vec2{p.y, p.x})) * sign_not_zero(p)) : p;
}

constexpr uint32_t previous_power2(uint32_t x) {
  uint32_t v = 1;
  while ((v << 1) < x) {
    v <<= 1;
  }
  return v;
}

inline glm::vec3 unproject_uv_zo(float depth, glm::vec2 uv, const glm::mat4& invXProj) {
  glm::vec4 ndc = glm::vec4(uv * 2.0f - 1.0f, depth, 1.0f);
  glm::vec4 world = invXProj * ndc;
  return glm::vec3(world) / world.w;
}

inline auto calc_frustum_planes(glm::mat4& view_proj_mat, glm::vec4 (&planes)[6]) -> void {
  ZoneScoped;

  for (auto i = 0; i < 4; ++i) {
    planes[0][i] = view_proj_mat[i][3] + view_proj_mat[i][0];
  }
  for (auto i = 0; i < 4; ++i) {
    planes[1][i] = view_proj_mat[i][3] - view_proj_mat[i][0];
  }
  for (auto i = 0; i < 4; ++i) {
    planes[2][i] = view_proj_mat[i][3] + view_proj_mat[i][1];
  }
  for (auto i = 0; i < 4; ++i) {
    planes[3][i] = view_proj_mat[i][3] - view_proj_mat[i][1];
  }
  for (auto i = 0; i < 4; ++i) {
    planes[4][i] = view_proj_mat[i][3] + view_proj_mat[i][2];
  }
  for (auto i = 0; i < 4; ++i) {
    planes[5][i] = view_proj_mat[i][3] - view_proj_mat[i][2];
  }

  for (auto& plane : planes) {
    plane /= glm::length(glm::vec3(plane));
    plane.w = -plane.w;
  }
}

bool decompose_transform(const glm::mat4& transform, glm::vec3& translation, glm::vec3& rotation, glm::vec3& scale);

template <typename T>
static T smooth_damp(const T& current,
                     const T& target,
                     T& current_velocity,
                     float smooth_time,
                     const float max_speed,
                     float delta_time) {
  ZoneScoped;
  // Based on Game Programming Gems 4 Chapter 1.10
  smooth_time = glm::max(0.0001F, smooth_time);
  const float omega = 2.0f / smooth_time;

  const float x = omega * delta_time;
  const float exp = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);

  T change = current - target;
  const T original_to = target;

  // Clamp maximum speed
  const float max_change = max_speed * smooth_time;

  const float max_change_sq = max_change * max_change;
  const float sq_dist = glm::length2(change);
  if (sq_dist > max_change_sq) {
    const float mag = glm::sqrt(sq_dist);
    change = change / mag * max_change;
  }

  const T new_target = current - change;
  const T temp = (current_velocity + omega * change) * delta_time;

  current_velocity = (current_velocity - omega * temp) * exp;

  T output = new_target + (change + temp) * exp;

  // Prevent overshooting
  const T orig_minus_current = original_to - current;
  const T out_minus_orig = output - original_to;

  if (glm::compAdd(orig_minus_current * out_minus_orig) > 0.0f) {
    output = original_to;
    current_velocity = (output - original_to) / delta_time;
  }
  return output;
}

float lerp(float a, float b, float t);
float inverse_lerp(float a, float b, float value);
float inverse_lerp_clamped(float a, float b, float value);
glm::vec2 world_to_screen(
    const glm::vec3& world_pos, const glm::mat4& mvp, float width, float height, float win_pos_x, float win_pos_y);

glm::vec4 transform(const glm::vec4& vec, const glm::mat4& view);
glm::vec4 transform_normal(const glm::vec4& vec, const glm::mat4& mat);
glm::vec4 transform_coord(const glm::vec4& vec, const glm::mat4& view);

glm::vec3 from_jolt(const JPH::Vec3& vec);
JPH::Vec3 to_jolt(const glm::vec3& vec);
glm::vec4 from_jolt(const JPH::Vec4& vec);
JPH::Vec4 to_jolt(const glm::vec4& vec);
AABB from_jolt(const JPH::AABox& aabb);
} // namespace ox::math
