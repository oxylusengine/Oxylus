#include "Asset/AudioSource.hpp"

#include <miniaudio.h>
#include <utility>

#include "Audio/AudioEngine.hpp"
#include "Core/App.hpp"

namespace ox {
// Big enough that decoding a long file is not dominated by call overhead, small enough to stay off
// the job thread's stack budget for the scratch buffer below.
constexpr auto PEAK_DECODE_CHUNK_FRAMES = 4096_u64;

auto read_audio_peaks(const std::filesystem::path& path, const u32 bucket_count) -> option<AudioPeaks> {
  ZoneScoped;

  if (bucket_count == 0) {
    return nullopt;
  }

  auto config = ma_decoder_config_init(ma_format_f32, 0, 0);
  auto decoder = ma_decoder{};
  const auto path_str = path.string();
  if (ma_decoder_init_file(path_str.c_str(), &config, &decoder) != MA_SUCCESS) {
    return nullopt;
  }
  OX_DEFER(&) { ma_decoder_uninit(&decoder); };

  auto total_frames = ma_uint64{0};
  if (ma_decoder_get_length_in_pcm_frames(&decoder, &total_frames) != MA_SUCCESS || total_frames == 0) {
    return nullopt;
  }

  const auto channels = decoder.outputChannels;
  auto peaks = AudioPeaks{
    .min_amplitude = std::vector<f32>(bucket_count, 0.0f),
    .max_amplitude = std::vector<f32>(bucket_count, 0.0f),
    .duration = static_cast<f32>(total_frames) / static_cast<f32>(decoder.outputSampleRate),
    .channels = channels,
    .sample_rate = decoder.outputSampleRate,
  };

  auto samples = std::vector<f32>(PEAK_DECODE_CHUNK_FRAMES * channels);
  auto frame_cursor = ma_uint64{0};
  while (frame_cursor < total_frames) {
    auto frames_read = ma_uint64{0};
    if (
      ma_decoder_read_pcm_frames(&decoder, samples.data(), PEAK_DECODE_CHUNK_FRAMES, &frames_read) != MA_SUCCESS ||
      frames_read == 0
    ) {
      break;
    }

    for (auto frame = ma_uint64{0}; frame < frames_read; ++frame) {
      // Mixing down keeps the envelope honest for stereo files: taking each channel separately
      // would draw the louder one only.
      auto mixed = 0.0f;
      for (auto channel = 0_u32; channel < channels; ++channel) {
        mixed += samples[frame * channels + channel];
      }
      mixed /= static_cast<f32>(channels);

      const auto bucket = ox::min(
        static_cast<usize>((frame_cursor + frame) * bucket_count / total_frames),
        static_cast<usize>(bucket_count - 1)
      );
      peaks.min_amplitude[bucket] = ox::min(peaks.min_amplitude[bucket], mixed);
      peaks.max_amplitude[bucket] = ox::max(peaks.max_amplitude[bucket], mixed);
    }

    frame_cursor += frames_read;
  }

  return peaks;
}

AudioSource::~AudioSource() { unload(); }

AudioSource::AudioSource(AudioSource&& other) noexcept : _sound(std::exchange(other._sound, nullptr)) {}

auto AudioSource::operator=(AudioSource&& other) noexcept -> AudioSource& {
  if (this != &other) {
    unload();
    _sound = std::exchange(other._sound, nullptr);
  }

  return *this;
}

auto AudioSource::load(const std::filesystem::path& path) -> bool {
  ZoneScoped;

  unload();

  _sound = new ma_sound;
  auto* engine = App::mod<AudioEngine>().get_engine();
  auto path_str = path.string();
  const ma_result result =
    ma_sound_init_from_file(engine, path_str.c_str(), MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, nullptr, _sound);
  if (result != MA_SUCCESS) {
    OX_LOG_ERROR("Failed to load sound: {}", path);
    // init failed, so there is nothing to uninit
    delete _sound;
    _sound = nullptr;
    return false;
  }

  return true;
}

auto AudioSource::unload() -> void {
  ZoneScoped;

  if (_sound == nullptr) {
    return;
  }

  ma_sound_uninit(_sound);
  delete _sound;
  _sound = nullptr;
}

auto AudioSource::get_source() -> ma_sound* { return _sound; }
} // namespace ox
