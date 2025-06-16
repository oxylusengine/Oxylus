#include "AudioEngine.hpp"

#include "Utils/Profiler.hpp"

#define MINIAUDIO_IMPLEMENTATION

#include <miniaudio.h>

namespace ox {
static ma_attenuation_model get_attenuation_model(const AttenuationModelType model) {
  switch (model) {
    case AttenuationModelType::None       : return ma_attenuation_model_none;
    case AttenuationModelType::Inverse    : return ma_attenuation_model_inverse;
    case AttenuationModelType::Linear     : return ma_attenuation_model_linear;
    case AttenuationModelType::Exponential: return ma_attenuation_model_exponential;
  }

  return ma_attenuation_model_none;
}

auto AudioEngine::init() -> std::expected<void, std::string> {
  ZoneScoped;
  ma_engine_config config = ma_engine_config_init();
  config.listenerCount = 1;

  engine = new ma_engine();
  const ma_result result = ma_engine_init(&config, engine);
  if (result != MA_SUCCESS)
    return std::unexpected{"ma_engine_init failed!"};

  return {};
}

auto AudioEngine::deinit() -> std::expected<void, std::string> {
  ma_engine_uninit(engine);
  delete engine;
  return {};
}

auto AudioEngine::get_engine() const -> ma_engine* { return engine; }

auto AudioEngine::play_source(ma_sound* sound) -> void {
  ZoneScoped;
  ma_sound_seek_to_pcm_frame(sound, 0);
  ma_sound_start(sound);
}

auto AudioEngine::pause_source(ma_sound* sound) -> void {
  ZoneScoped;
  ma_sound_stop(sound);
}

auto AudioEngine::unpause_source(ma_sound* sound) -> void {
  ZoneScoped;
  ma_sound_start(sound);
}

auto AudioEngine::stop_source(ma_sound* sound) -> void {
  ZoneScoped;
  ma_sound_stop(sound);
  ma_sound_seek_to_pcm_frame(sound, 0);
}

auto AudioEngine::is_source_playing(ma_sound* sound) -> bool {
  ZoneScoped;
  return ma_sound_is_playing(sound);
}

auto AudioEngine::set_source_volume(ma_sound* sound, f32 volume) -> void {
  ZoneScoped;
  ma_sound_set_volume(sound, volume);
}

auto AudioEngine::set_source_pitch(ma_sound* sound, f32 pitch) -> void {
  ZoneScoped;
  ma_sound_set_pitch(sound, pitch);
}

auto AudioEngine::set_source_looping(ma_sound* sound, bool state) -> void {
  ZoneScoped;
  ma_sound_set_looping(sound, state);
}

auto AudioEngine::set_source_spatialization(ma_sound* sound, bool state) -> void {
  ZoneScoped;
  ma_sound_set_spatialization_enabled(sound, state);
}

auto AudioEngine::set_source_attenuation_model(ma_sound* sound, AttenuationModelType type) -> void {
  ZoneScoped;
  ma_sound_set_attenuation_model(sound, get_attenuation_model(type));
}

auto AudioEngine::set_source_roll_off(ma_sound* sound, f32 roll_off) -> void {
  ZoneScoped;
  ma_sound_set_rolloff(sound, roll_off);
}

auto AudioEngine::set_source_min_gain(ma_sound* sound, f32 min_gain) -> void {
  ZoneScoped;
  ma_sound_set_min_gain(sound, min_gain);
}

auto AudioEngine::set_source_max_gain(ma_sound* sound, f32 max_gain) -> void {
  ZoneScoped;
  ma_sound_set_max_gain(sound, max_gain);
}

auto AudioEngine::set_source_min_distance(ma_sound* sound, f32 min_distance) -> void {
  ZoneScoped;
  ma_sound_set_min_distance(sound, min_distance);
}

auto AudioEngine::set_source_max_distance(ma_sound* sound, f32 max_distance) -> void {
  ZoneScoped;
  ma_sound_set_max_distance(sound, max_distance);
}

auto AudioEngine::set_source_cone(ma_sound* sound, f32 inner_angle, f32 outer_angle, f32 outer_gain) -> void {
  ZoneScoped;
  ma_sound_set_cone(sound, inner_angle, outer_angle, outer_gain);
}

auto AudioEngine::set_source_doppler_factor(ma_sound* sound, f32 factor) -> void {
  ZoneScoped;
  ma_sound_set_doppler_factor(sound, glm::max(factor, 0.0f));
}

auto AudioEngine::set_source_position(ma_sound* sound, const glm::vec3& position) -> void {
  ZoneScoped;
  ma_sound_set_position(sound, position.x, position.y, position.z);
}

auto AudioEngine::set_source_direction(ma_sound* sound, const glm::vec3& forward) -> void {
  ZoneScoped;
  ma_sound_set_direction(sound, forward.x, forward.y, forward.z);
}

auto AudioEngine::set_source_velocity(ma_sound* sound, const glm::vec3& velocity) -> void {
  ZoneScoped;
  ma_sound_set_velocity(sound, velocity.x, velocity.y, velocity.z);
}

auto AudioEngine::set_listener_cone(u32 listener_index, f32 cone_inner_angle, f32 cone_outer_angle, f32 cone_outer_gain)
    -> void {
  ma_engine_listener_set_cone(engine, listener_index, cone_inner_angle, cone_outer_angle, cone_outer_gain);
}

auto AudioEngine::set_listener_position(u32 listener_index, const glm::vec3& position) -> void {
  ma_engine_listener_set_position(engine, listener_index, position.x, position.y, position.z);

  static bool setup_world_up = false;
  if (!setup_world_up) {
    ma_engine_listener_set_world_up(engine, listener_index, 0, 1, 0);
    setup_world_up = true;
  }
}

auto AudioEngine::set_listener_direction(u32 listener_index, const glm::vec3& forward) -> void {
  ma_engine_listener_set_direction(engine, listener_index, forward.x, forward.y, forward.z);
}
} // namespace ox
