#pragma once

#include "Core/Keycodes.hpp"

namespace ox {
class VirtualMouse {
public:
  auto press(this const VirtualMouse& self, MouseCode code) -> void;
  auto release(this const VirtualMouse& self, MouseCode code) -> void;

  // a full "Tap" (Down then Up)
  auto tap(this const VirtualMouse& self, MouseCode code) -> void;

private:
  auto push_key_event(this const VirtualMouse& self, MouseCode code, bool down) -> void;
};
} // namespace ox
