#pragma once

#include <simdjson.h>
namespace ox {

template <glm::length_t N, typename T>
bool json_to_vec(simdjson::ondemand::value& o, glm::vec<N, T>& vec) {
  using U = glm::vec<N, T>;
  for (i32 i = 0; i < U::length(); i++) {
    constexpr static std::string_view components[] = {"x", "y", "z", "w"};
    vec[i] = static_cast<T>(o[components[i]].get_double().value_unsafe());
  }

  return true;
}

inline bool json_to_quat(simdjson::ondemand::value& o, glm::quat& quat) {
  for (i32 i = 0; i < glm::quat::length(); i++) {
    constexpr static std::string_view components[] = {"x", "y", "z", "w"};
    quat[i] = static_cast<f32>(o[components[i]].get_double().value_unsafe());
  }

  return true;
}

} // namespace ox
