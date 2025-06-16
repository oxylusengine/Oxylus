#pragma once

namespace ox {
class Timer;

class Timestep {
public:
  Timestep();
  ~Timestep();

  void on_update();
  double get_millis() const { return m_timestep; }
  double get_elapsed_millis() const { return m_elapsed; }

  double get_seconds() const { return m_timestep * 0.001; }
  double get_elapsed_seconds() const { return m_elapsed * 0.001; }

  explicit operator float() const { return (float)m_timestep; }

private:
  double m_timestep; // MilliSeconds
  double m_last_time;
  double m_elapsed;

  Timer* m_Timer;
};
} // namespace ox
