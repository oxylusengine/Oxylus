#include "Core/VirtualKeyboard.hpp"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_timer.h>

#include "Core/App.hpp"
#include "Utils/Log.hpp"

namespace ox {
auto VirtualKeyboard::press(this const VirtualKeyboard& self, KeyCode keycode, ModCode mod) -> void {
  self.push_key_event(keycode, mod, true);
}

auto VirtualKeyboard::release(this const VirtualKeyboard& self, KeyCode keycode, ModCode mod) -> void {
  self.push_key_event(keycode, mod, false);
}

// a full "Tap" (Down then Up)
void VirtualKeyboard::tap(this const VirtualKeyboard& self, KeyCode keycode, ModCode mod) {
  self.press(keycode);
  self.release(keycode);
}

void VirtualKeyboard::push_key_event(this const VirtualKeyboard& self, KeyCode keycode, ModCode mod, bool down) {
  SDL_Event event;
  SDL_zero(event);

  event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;

  event.common.timestamp = SDL_GetTicksNS();

  event.key.type = static_cast<SDL_EventType>(event.type);
  event.key.key = static_cast<SDL_Keycode>(keycode);
  event.key.mod = static_cast<SDL_Keymod>(mod);
  event.key.down = down;
  event.key.repeat = false;
  event.key.which = 0; // Should be fine for 'System Keyboard'
  auto& window = App::get_window();
  event.key.windowID = SDL_GetWindowID(reinterpret_cast<SDL_Window*>(window.get_handle()));

  if (!SDL_PushEvent(&event)) {
    OX_LOG_ERROR("Failed to push virtual key event: {}", SDL_GetError());
  }
}
} // namespace ox
