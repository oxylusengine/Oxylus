#pragma once

#include "Core/Keycodes.hpp"

namespace ox {
class VirtualKeyboard {
public:
  auto press(this const VirtualKeyboard& self, KeyCode keycode, ModCode mod = ModCode::None) -> void;
  auto release(this const VirtualKeyboard& self, KeyCode keycode, ModCode mod = ModCode::None) -> void;

  // a full "Tap" (Down then Up)
  auto tap(this const VirtualKeyboard& self, KeyCode keycode, ModCode mod = ModCode::None) -> void;

private:
  auto push_key_event(this const VirtualKeyboard& self, KeyCode keycode, ModCode mod, bool down) -> void;
};
} // namespace ox
