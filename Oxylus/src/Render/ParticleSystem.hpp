#pragma once

#include "Utils/OxMath.hpp"

namespace ox {
class Texture;

struct Particle {
  glm::vec3 position = glm::vec3(0.0f);
  glm::vec3 rotation = glm::vec3(0.0f);
  glm::vec3 size = glm::vec3(1.0f);
  glm::vec4 color = glm::vec4(1.0f);
  float life_remaining = 0.0f;
};

template <typename T>
struct OverLifetimeModule {
  T start;
  T end;
  bool enabled = false;

  OverLifetimeModule() : start(), end() {}
  OverLifetimeModule(const T& start_, const T& end_) : start(start_), end(end_) {}

  T evaluate(float factor) { return glm::lerp(end, start, factor); }
};

template <typename T>
struct BySpeedModule {
  T start;
  T end;
  float min_speed = 0.0f;
  float max_speed = 1.0f;
  bool enabled = false;

  BySpeedModule() : start(), end() {}
  BySpeedModule(const T& start_, const T& end_) : start(start_), end(end_) {}

  T evaluate(float speed) {
    float factor = math::inverse_lerp_clamped(min_speed, max_speed, speed);
    return glm::lerp(end, start, factor);
  }
};

struct ParticleProperties {
  float duration = 3.0f;
  bool looping = true;
  float start_delay = 0.0f;
  float start_lifetime = 3.0f;
  glm::vec3 start_velocity = glm::vec3(0.0f, 2.0f, 0.0f);
  glm::vec4 start_color = glm::vec4(1.0f);
  glm::vec3 start_size = glm::vec3(1.0f);
  glm::vec3 start_rotation = glm::vec3(0.0f);
  float gravity_modifier = 0.0f;
  float simulation_speed = 1.0f;
  bool play_on_awake = true;
  uint32_t max_particles = 1000;

  uint32_t rate_over_time = 10;
  uint32_t rate_over_distance = 0;
  uint32_t burst_count = 0;
  float burst_time = 1.0f;

  glm::vec3 position_start = glm::vec3(-0.2f, 0.0f, 0.0f);
  glm::vec3 position_end = glm::vec3(0.2f, 0.0f, 0.0f);

  OverLifetimeModule<glm::vec3> velocity_over_lifetime;
  OverLifetimeModule<glm::vec3> force_over_lifetime;
  OverLifetimeModule<glm::vec4> color_over_lifetime = {{0.8f, 0.2f, 0.2f, 0.0f}, {0.2f, 0.2f, 0.75f, 1.0f}};
  BySpeedModule<glm::vec4> color_by_speed = {{0.8f, 0.2f, 0.2f, 0.0f}, {0.2f, 0.2f, 0.75f, 1.0f}};
  OverLifetimeModule<glm::vec3> size_over_lifetime = {glm::vec3(0.2f), glm::vec3(1.0f)};
  BySpeedModule<glm::vec3> size_by_speed = {glm::vec3(0.2f), glm::vec3(1.0f)};
  OverLifetimeModule<glm::vec3> rotation_over_lifetime;
  BySpeedModule<glm::vec3> rotation_by_speed;

  std::shared_ptr<Texture> texture = nullptr;
};

class ParticleSystem {
public:
  ParticleSystem();

  void play();
  void stop(bool force = false);
  void on_update(float deltaTime, const glm::vec3& position);
  void on_render() const;

  ParticleProperties& get_properties() { return properties; }
  const ParticleProperties& get_properties() const { return properties; }
  uint32_t get_active_particle_count() const { return active_particle_count; }

private:
  void emit(const glm::vec3& position, uint32_t count = 1);

  std::vector<Particle> particles;
  uint32_t pool_index = 0;
  ParticleProperties properties;

  float system_time = 0.0f;
  float burst_time = 0.0f;
  float spawn_time = 0.0f;
  glm::vec3 last_spawned_position = glm::vec3(0.0f);

  uint32_t active_particle_count = 0;
  bool playing = false;
};
} // namespace ox
