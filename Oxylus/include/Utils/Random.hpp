#pragma once

#include <glm/ext/vector_float3.hpp>

#include "Core/Types.hpp"

namespace ox {
class Random {
public:
  Random();

  static u32 get_uint();
  static u32 get_uint(u32 min, u32 max);
  static f32 get_float();
  static glm::vec3 get_vec3();
  static glm::vec3 get_vec3(f32 min, f32 max);
  static glm::vec3 in_unit_sphere();
};
} // namespace ox
