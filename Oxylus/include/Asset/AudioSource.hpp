#pragma once

#include <filesystem>

#include "Core/Types.hpp"

struct ma_sound;

namespace ox {
enum class AudioID : u64 { Invalid = std::numeric_limits<u64>::max() };
class AudioSource {
public:
  AudioSource() = default;
  ~AudioSource();

  auto load(const std::filesystem::path& path) -> bool;
  auto unload() -> void;
  auto get_source() -> ma_sound*;

private:
  ma_sound* _sound = nullptr;
};
} // namespace ox
