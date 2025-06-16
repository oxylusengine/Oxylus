#include "Random.hpp"

#include <random>

namespace ox {
thread_local std::mt19937 random_engine;
thread_local std::uniform_int_distribution<std::mt19937::result_type> rnd_distribution;

auto Random::init() -> std::expected<void, std::string> {
  random_engine.seed(std::random_device()());
  return {};
}

auto Random::deinit() -> std::expected<void, std::string> { return {}; }

uint32_t Random::get_uint() { return rnd_distribution(random_engine); }

uint32_t Random::get_uint(const uint32_t min, const uint32_t max) {
  return min + (rnd_distribution(random_engine) % (max - min + 1));
}

float Random::get_float() {
  return (float)rnd_distribution(random_engine) / (float)std::numeric_limits<uint32_t>::max();
}

glm::vec3 Random::get_vec3() { return {get_float(), get_float(), get_float()}; }

glm::vec3 Random::get_vec3(const float min, const float max) {
  return {get_float() * (max - min) + min, get_float() * (max - min) + min, get_float() * (max - min) + min};
}

glm::vec3 Random::in_unit_sphere() { return glm::normalize(get_vec3(-1.0f, 1.0f)); }
} // namespace ox
