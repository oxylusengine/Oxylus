#pragma once

#include "Core/Types.hpp"

namespace ox {
class Timer;

class Timestep {
public:
  Timestep();
  ~Timestep();

  Timestep(const Timestep& other) = delete;
  Timestep(Timestep&& other) = delete;
  Timestep& operator=(const Timestep& other) = delete;
  Timestep& operator=(Timestep&& other) = delete;

  auto on_update(this Timestep& self) -> void;
  auto get_millis(this const Timestep& self) -> f64 { return self.timestep; }
  auto get_elapsed_millis(this const Timestep& self) -> f64 { return self.elapsed; }

  auto get_seconds(this const Timestep& self) -> f64 { return self.timestep * 0.001; }
  auto get_elapsed_seconds(this const Timestep& self) -> f64 { return self.elapsed * 0.001; }

  auto get_max_frame_time(this const Timestep& self) -> f64 { return self.max_frame_time; }
  auto set_max_frame_time(this Timestep& self, f64 value) -> void { self.max_frame_time = value; }
  auto reset_max_frame_time(this Timestep& self) -> void { self.max_frame_time = -1.0; }

  explicit operator float() const { return (float)timestep; }

private:
  f64 timestep = 0; // Stored as MilliSeconds
  f64 last_time = 0;
  f64 elapsed = 0;
  f64 max_frame_time = -1.0;

  Timer* timer = nullptr;
};
} // namespace ox
