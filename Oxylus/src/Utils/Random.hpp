#pragma once

#include "Core/ESystem.hpp"

namespace ox {
class Random : public ESystem {
public:
  Random() = default;

  auto init() -> std::expected<void, std::string> override;
  auto deinit() -> std::expected<void, std::string> override;

  static uint32_t get_uint();
  static uint32_t get_uint(uint32_t min, uint32_t max);
  static float get_float();
  static glm::vec3 get_vec3();
  static glm::vec3 get_vec3(float min, float max);
  static glm::vec3 in_unit_sphere();
};
} // namespace ox
