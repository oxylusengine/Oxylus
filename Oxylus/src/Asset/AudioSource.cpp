#include "Asset/AudioSource.hpp"

#include <miniaudio.h>

#include "Audio/AudioEngine.hpp"
#include "Core/App.hpp"

namespace ox {
AudioSource::~AudioSource() { unload(); }

auto AudioSource::load(const std::string& path) -> bool {
  ZoneScoped;

  _sound = new ma_sound;
  auto* engine = App::mod<AudioEngine>().get_engine();
  const ma_result result = ma_sound_init_from_file(
      engine, path.c_str(), MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, nullptr, _sound);
  if (result != MA_SUCCESS) {
    OX_LOG_ERROR("Failed to load sound: {}", path);
    return false;
  }

  return true;
}

auto AudioSource::unload() -> void {
  ZoneScoped;

  ma_sound_uninit(_sound);
  delete _sound;
}

auto AudioSource::get_source() -> ma_sound* { return _sound; }
} // namespace ox
