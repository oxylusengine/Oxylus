#pragma once
#include "Core/ESystem.hpp"
struct ma_engine;
struct ma_sound;

namespace ox {
enum class AttenuationModelType { None = 0, Inverse, Linear, Exponential };

class AudioEngine : public ESystem {
public:
  auto init() -> std::expected<void, std::string> override;
  auto deinit() -> std::expected<void, std::string> override;

  auto get_engine() const -> ma_engine*;

  // -- Source --
  auto play_source(ma_sound* sound) -> void;
  auto pause_source(ma_sound* sound) -> void;
  auto unpause_source(ma_sound* sound) -> void;
  auto stop_source(ma_sound* sound) -> void;
  auto is_source_playing(ma_sound* sound) -> bool;
  auto set_source_volume(ma_sound* sound, f32 volume) -> void;
  auto set_source_pitch(ma_sound* sound, f32 pitch) -> void;
  auto set_source_looping(ma_sound* sound, bool state) -> void;
  auto set_source_spatialization(ma_sound* sound, bool state) -> void;
  auto set_source_attenuation_model(ma_sound* sound, AttenuationModelType type) -> void;
  auto set_source_roll_off(ma_sound* sound, f32 rollOff) -> void;
  auto set_source_min_gain(ma_sound* sound, f32 minGain) -> void;
  auto set_source_max_gain(ma_sound* sound, f32 maxGain) -> void;
  auto set_source_min_distance(ma_sound* sound, f32 minDistance) -> void;
  auto set_source_max_distance(ma_sound* sound, f32 maxDistance) -> void;
  auto set_source_cone(ma_sound* sound, f32 innerAngle, f32 outerAngle, f32 outerGain) -> void;
  auto set_source_doppler_factor(ma_sound* sound, f32 factor) -> void;
  auto set_source_position(ma_sound* sound, const glm::vec3& position) -> void;
  auto set_source_direction(ma_sound* sound, const glm::vec3& forward) -> void;
  auto set_source_velocity(ma_sound* sound, const glm::vec3& velocity) -> void;

  // -- Listener --
  auto set_listener_cone(u32 listener_index, f32 cone_inner_angle, f32 cone_outer_angle, f32 cone_outer_gain) -> void;
  auto set_listener_position(u32 listener_index, const glm::vec3& position) -> void;
  auto set_listener_direction(u32 listener_index, const glm::vec3& forward) -> void;

private:
  ma_engine* engine = nullptr;
};
} // namespace ox
