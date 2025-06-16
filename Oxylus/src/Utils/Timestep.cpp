#include "Timestep.hpp"

#include "Timer.hpp"

namespace ox {
Timestep::Timestep() : m_timestep(0.0), m_last_time(0.0), m_elapsed(0.0) { m_Timer = new Timer(); }

Timestep::~Timestep() { delete m_Timer; }

void Timestep::on_update() {
  ZoneScoped;

  double currentTime = m_Timer->get_elapsed_msd();
  double maxFrameTime = -1.0;
  double dt = currentTime - m_last_time;

  {
    ZoneNamedN(z, "Sleep TimeStep to target fps", true);
    while (dt < maxFrameTime) {
      currentTime = m_Timer->get_elapsed_msd();
      dt = currentTime - m_last_time;
    }
  }

  m_timestep = currentTime - m_last_time;
  m_last_time = currentTime;
  m_elapsed += m_timestep;
}
} // namespace ox
