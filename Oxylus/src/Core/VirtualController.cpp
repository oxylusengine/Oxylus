#include "Core/VirtualController.hpp"

#include <SDL3/SDL_gamepad.h>
#include <algorithm>

#include "Utils/Log.hpp"

namespace ox {
auto as_joystick_handle(void* handle) -> SDL_Joystick* { return reinterpret_cast<SDL_Joystick*>(handle); }

auto VirtualController::connect(this VirtualController& self) -> void {
  SDL_VirtualJoystickDesc desc = {};
  SDL_INIT_INTERFACE(&desc);
  desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
  desc.nbuttons = 15; // Standard Xbox/PS layout
  desc.naxes = 6;
  desc.vendor_id = 0x0001;
  desc.product_id = 0x0001;
  desc.name = "Virtual Debug Controller";

  self.instance_id = SDL_AttachVirtualJoystick(&desc);

  if (self.instance_id == 0) {
    OX_LOG_ERROR("Failed to create virtual joystick: {}", SDL_GetError());
  }

  self.joystick_handle = SDL_OpenJoystick(self.instance_id);
}

auto VirtualController::disconnect(this VirtualController& self) -> void {
  if (self.instance_id) {
    SDL_DetachVirtualJoystick(self.instance_id);
    if (self.joystick_handle)
      SDL_CloseJoystick(as_joystick_handle(self.joystick_handle));
  }
}

auto VirtualController::simulate_button(this VirtualController& self, GamepadButtonCode button, bool down) -> void {
  if (self.instance_id == 0)
    return;

  // This injects the event into SDL's internal queue
  SDL_SetJoystickVirtualButton(as_joystick_handle(self.joystick_handle), (i32)button, down);
}

void VirtualController::simulate_axis(this VirtualController& self, GamepadAxisCode axis, f32 value) {
  if (self.instance_id == 0 || !self.joystick_handle)
    return;

  // Clamp float to -1.0 to 1.0
  value = std::clamp(value, -1.0f, 1.0f);

  i16 raw_value;

  // SPECIAL CASE: Triggers (Index 4 and 5 on standard Gamepads)
  // They need to map -1.0 (Released) -> -32768 raw
  // and 1.0 (Pressed) -> 32767 raw
  if ((i32)axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || (i32)axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
    // Map 0.0 (released) to 1.0 (pressed) input range to -32768 to 32767
    // If you pass in 0.0f as "released", we need to convert that.
    // Let's assume input `value` is 0.0 (released) to 1.0 (pressed) for ease of use:
    f32 remapped = (value * 65535.0f) - 32768.0f;
    raw_value = (i16)remapped;
  } else {
    // Standard Sticks: -1.0 to 1.0 maps directly to -32768 to 32767
    raw_value = (i16)(value * 32767.0f);
  }

  SDL_SetJoystickVirtualAxis(as_joystick_handle(self.joystick_handle), (i32)axis, raw_value);
}
} // namespace ox
