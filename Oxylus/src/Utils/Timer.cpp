#include "Timer.hpp"

namespace ox {
auto Timer::get_timed_ms() -> f32 {
  const f32 time = duration(_last_time, now(), 1000.0f);
  _last_time = now();
  return time;
}

TimeStamp Timer::now() { return std::chrono::high_resolution_clock::now(); }

auto Timer::duration(const TimeStamp start, const TimeStamp end, const f64 time_resolution) -> f64 {
  return std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count() * time_resolution;
}

auto Timer::duration(const TimeStamp start, const TimeStamp end, const f32 time_resolution) -> f32 {
  return (float)std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count() * time_resolution;
}
} // namespace ox
