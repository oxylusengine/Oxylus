#include "ParticleSystem.hpp"

namespace ox {
ParticleSystem::ParticleSystem() : particles(10000) {
  if (properties.play_on_awake)
    play();

  // TODO: m_Properties.Texture = Texture::GetBlankImage();
}

void ParticleSystem::play() {
  system_time = 0.0f;
  playing = true;
}

void ParticleSystem::stop(bool force) {
  if (force) {
    for (auto& particle : particles)
      particle.life_remaining = 0.0f;
  }

  system_time = properties.start_delay + properties.duration;
  playing = false;
}

void ParticleSystem::on_update(float ts, const glm::vec3& position) {
  ZoneScoped;
  const float simTs = ts * properties.simulation_speed;

  if (playing && !properties.looping)
    system_time += simTs;
  const float delay = properties.start_delay;
  if (playing && (properties.looping || (system_time <= delay + properties.duration && system_time > delay))) {
    // Emit particles in unit time
    spawn_time += simTs;
    if (spawn_time >= 1.0f / static_cast<float>(properties.rate_over_time)) {
      spawn_time = 0.0f;
      emit(position, 1);
    }

    // Emit particles over unit distance
    if (glm::distance2(last_spawned_position, position) > 1.0f) {
      last_spawned_position = position;
      emit(position, properties.rate_over_distance);
    }

    // Emit bursts of particles over time
    burst_time += simTs;
    if (burst_time >= properties.burst_time) {
      burst_time = 0.0f;
      emit(position, properties.burst_count);
    }
  }

  // Simulate
  active_particle_count = 0;
  for (auto& particle : particles) {
    if (particle.life_remaining <= 0.0f)
      continue;

    particle.life_remaining -= simTs;

    const float t = glm::clamp(particle.life_remaining / properties.start_lifetime, 0.0f, 1.0f);

    glm::vec3 velocity = properties.start_velocity;
    if (properties.velocity_over_lifetime.enabled)
      velocity *= properties.velocity_over_lifetime.evaluate(t);

    glm::vec3 force(0.0f);
    if (properties.force_over_lifetime.enabled)
      force = properties.force_over_lifetime.evaluate(t);

    force.y += properties.gravity_modifier * -9.8f;
    velocity += force * simTs;

    const float velocityMagnitude = glm::length(velocity);

    // Color
    particle.color = properties.start_color;
    if (properties.color_over_lifetime.enabled)
      particle.color *= properties.color_over_lifetime.evaluate(t);
    if (properties.color_by_speed.enabled)
      particle.color *= properties.color_by_speed.evaluate(velocityMagnitude);

    // Size
    particle.size = properties.start_size;
    if (properties.size_over_lifetime.enabled)
      particle.size *= properties.size_over_lifetime.evaluate(t);
    if (properties.size_by_speed.enabled)
      particle.size *= properties.size_by_speed.evaluate(velocityMagnitude);

    // Rotation
    particle.rotation = properties.start_rotation;
    if (properties.rotation_over_lifetime.enabled)
      particle.rotation += properties.rotation_over_lifetime.evaluate(t);
    if (properties.rotation_by_speed.enabled)
      particle.rotation += properties.rotation_by_speed.evaluate(velocityMagnitude);

    particle.position += velocity * simTs;
    ++active_particle_count;
  }
}

void ParticleSystem::on_render() const {
  for (const auto& particle : particles) {
    if (particle.life_remaining <= 0.0f)
      continue;

    glm::mat4 transform = glm::translate(glm::mat4(1.0f), particle.position) * glm::mat4(glm::quat(particle.rotation)) *
                          glm::scale(glm::mat4(1.0f), particle.size);

    // Renderer::SubmitQuad(transform, m_Properties.Texture, particle.Color);
  }
}

static float RandomFloat(float min, float max) {
  const float r = static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
  return min + r * (max - min);
}

void ParticleSystem::emit(const glm::vec3& position, uint32_t count) {
  if (active_particle_count >= properties.max_particles)
    return;

  for (uint32_t i = 0; i < count; ++i) {
    if (++pool_index >= 10000)
      pool_index = 0;

    auto& particle = particles[pool_index];

    particle.position = position;
    particle.position.x += RandomFloat(properties.position_start.x, properties.position_end.x);
    particle.position.y += RandomFloat(properties.position_start.y, properties.position_end.y);
    particle.position.z += RandomFloat(properties.position_start.z, properties.position_end.z);

    particle.life_remaining = properties.start_lifetime;
  }
}
} // namespace ox
