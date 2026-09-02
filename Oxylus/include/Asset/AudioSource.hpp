#pragma once

#include <filesystem>
#include <vector>

#include "Core/Option.hpp"
#include "Core/Types.hpp"

struct ma_sound;

namespace ox {
enum class AudioID : u64 { Invalid = std::numeric_limits<u64>::max() };

// Per-bucket amplitude envelope of a decoded sound, for drawing waveforms. Channels are mixed down
// and buckets are uniform in time, so bucket i covers [i, i + 1) / bucket_count of the duration.
struct AudioPeaks {
  std::vector<f32> min_amplitude = {};
  std::vector<f32> max_amplitude = {};
  f32 duration = 0.0f;
  u32 channels = 0;
  u32 sample_rate = 0;
};

// Decodes `path` off the audio engine's graph, so it is safe to call from a job. Returns nullopt if
// the file cannot be decoded.
auto read_audio_peaks(const std::filesystem::path& path, u32 bucket_count) -> option<AudioPeaks>;

class AudioSource {
public:
  AudioSource() = default;
  ~AudioSource();

  AudioSource(const AudioSource&) = delete;
  auto operator=(const AudioSource&) -> AudioSource& = delete;
  AudioSource(AudioSource&& other) noexcept;
  auto operator=(AudioSource&& other) noexcept -> AudioSource&;

  auto load(const std::filesystem::path& path) -> bool;
  auto unload() -> void;
  auto get_source() -> ma_sound*;

private:
  ma_sound* _sound = nullptr;
};
} // namespace ox
