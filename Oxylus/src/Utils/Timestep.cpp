#include "Utils/Timestep.hpp"

#include <thread>

#include "Utils/Timer.hpp"

namespace ox {
Timestep::Timestep() : timestep(0.0), last_time(0.0), elapsed(0.0) { timer = new Timer(); }

Timestep::~Timestep() { delete timer; }

void Timestep::on_update(this Timestep& self) {
  ZoneScoped;

  f64 current_time = self.timer->get_elapsed_msd();
  if (self.last_time == 0.0f) {
    self.last_time = current_time;
  }

  f64 dt = current_time - self.last_time;

  {
    ZoneNamedN(z, "Sleep TimeStep to target fps", true);
    constexpr f64 spin_threshold = 1.0;

    while (dt < self.max_frame_time) {
      f64 time_remaining = self.max_frame_time - dt;
      if (time_remaining > spin_threshold) {
        auto sleep_duration = std::chrono::duration<f64, std::milli>(time_remaining - spin_threshold);
        std::this_thread::sleep_for(sleep_duration);
      }

      current_time = self.timer->get_elapsed_msd();
      dt = current_time - self.last_time;
    }
  }

  self.timestep = current_time - self.last_time;
  self.last_time = current_time;
  self.elapsed += self.timestep;
}
} // namespace ox
