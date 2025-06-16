#pragma once

#define TOML_HEADER_ONLY 0
#include <toml++/toml.hpp>

namespace ox {
inline toml::array get_toml_array(const glm::vec2& vec) { return toml::array{vec.x, vec.y}; }

inline toml::array get_toml_array(const glm::vec3& vec) { return toml::array{vec.x, vec.y, vec.z}; }

inline toml::array get_toml_array(const glm::vec4& vec) { return toml::array{vec.x, vec.y, vec.z, vec.w}; }

inline glm::vec2 get_vec2_toml_array(toml::array* array) {
  return {array->get(0)->as_floating_point()->get(), array->get(1)->as_floating_point()->get()};
}

inline glm::vec3 get_vec3_toml_array(toml::array* array) {
  return {array->get(0)->as_floating_point()->get(),
          array->get(1)->as_floating_point()->get(),
          array->get(2)->as_floating_point()->get()};
}

inline glm::vec4 get_vec4_toml_array(toml::array* array) {
  return {array->get(0)->as_floating_point()->get(),
          array->get(1)->as_floating_point()->get(),
          array->get(2)->as_floating_point()->get(),
          array->get(3)->as_floating_point()->get()};
}
} // namespace ox
