#pragma once
#include <miniaudio.h>

namespace ox {
enum class AudioID : u64 { Invalid = std::numeric_limits<u64>::max() };
class AudioSource {
public:
  AudioSource() = default;
  ~AudioSource();

  auto load(const std::string& path) -> bool;
  auto unload() -> void;
  auto get_source() -> ma_sound*;

private:
  ma_sound _sound;
};
} // namespace ox
