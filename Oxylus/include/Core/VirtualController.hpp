#pragma once

#include "Core/Keycodes.hpp"
#include "Core/Types.hpp"

namespace ox {
class VirtualController {
public:
  auto connect(this VirtualController& self) -> void;
  auto disconnect(this VirtualController& self) -> void;

  auto simulate_button(this VirtualController& self, GamepadButtonCode button, bool down) -> void;
  void simulate_axis(this VirtualController& self, GamepadAxisCode axis, f32 value);

  auto get_instance_id(this const VirtualController& self) -> u32 { return self.instance_id; }

private:
  u32 instance_id = 0;
  void* joystick_handle = nullptr;
};
} // namespace ox
