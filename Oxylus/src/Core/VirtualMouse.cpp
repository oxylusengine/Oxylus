#include "Core/VirtualMouse.hpp"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_timer.h>

#include "Core/App.hpp"

namespace ox {

auto VirtualMouse::press(this const VirtualMouse& self, MouseCode code) -> void { self.push_key_event(code, true); }

auto VirtualMouse::release(this const VirtualMouse& self, MouseCode code) -> void { self.push_key_event(code, false); }

auto VirtualMouse::tap(this const VirtualMouse& self, MouseCode code) -> void {
  self.press(code);
  self.release(code);
}

auto VirtualMouse::push_key_event(this const VirtualMouse& self, MouseCode code, bool down) -> void {
  SDL_Event event;
  SDL_zero(event);

  event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;

  event.common.timestamp = SDL_GetTicksNS();

  event.button.type = static_cast<SDL_EventType>(event.type);
  event.button.button = static_cast<SDL_Keycode>(code);
  event.button.down = down;
  event.button.which = 0;
  auto& window = App::get_window();
  event.key.windowID = SDL_GetWindowID(reinterpret_cast<SDL_Window*>(window.get_handle()));

  if (!SDL_PushEvent(&event)) {
    OX_LOG_ERROR("Failed to push virtual key event: {}", SDL_GetError());
  }
}
} // namespace ox
