#pragma once

typedef std::chrono::time_point<std::chrono::high_resolution_clock> TimeStamp;

namespace ox {
class Timer {
public:
  Timer() : _start(now()), _frequency() { _last_time = _start; }

  ~Timer() = default;

  auto get_timed_ms() -> f32;

  static TimeStamp now();

  static auto duration(TimeStamp start, TimeStamp end, f64 time_resolution = 1.0) -> f64;
  static auto duration(TimeStamp start, TimeStamp end, f32 time_resolution) -> f32;

  auto get_elapsed_ms() const -> f32 { return get_elapsed(1000.0f); }
  auto get_elapsed_s() const -> f32 { return get_elapsed(1.0f); }
  auto get_elapsed_msd() const -> f64 { return get_elapsed(1000.0); }
  auto get_elapsed_sd() const -> f64 { return get_elapsed(1.0); }

protected:
  auto get_elapsed(const float time_resolution) const -> f32 { return duration(_start, now(), time_resolution); }

  auto get_elapsed(const double time_resolution = 1.0) const -> f64 { return duration(_start, now(), time_resolution); }

  TimeStamp _start;     // Start of timer
  TimeStamp _frequency; // Ticks Per Second
  TimeStamp _last_time; // Last time GetTimedMS was called
};
} // namespace ox
