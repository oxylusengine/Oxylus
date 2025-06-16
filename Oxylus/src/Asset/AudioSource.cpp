#include "AudioSource.hpp"

#include "Audio/AudioEngine.hpp"
#include "Core/App.hpp"
#include "Utils/Profiler.hpp"

namespace ox {
AudioSource::~AudioSource() { ma_sound_uninit(&_sound); }

auto AudioSource::load(const std::string& path) -> bool {
  ZoneScoped;
  auto* engine = App::get_system<AudioEngine>(EngineSystems::AudioEngine)->get_engine();
  const ma_result result = ma_sound_init_from_file(
      engine, path.c_str(), MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, nullptr, &_sound);
  if (result != MA_SUCCESS) {
    OX_LOG_ERROR("Failed to load sound: {}", path);
    return false;
  }

  return true;
}

auto AudioSource::unload() -> void {
  ZoneScoped;
  ma_sound_uninit(&_sound);
}

auto AudioSource::get_source() -> ma_sound* { return &_sound; }
} // namespace ox
