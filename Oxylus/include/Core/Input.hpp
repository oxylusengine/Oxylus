#pragma once

#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>
#include <expected>
#include <glm/ext/vector_float2.hpp>
#include <string>

#include "Core/Keycodes.hpp"

namespace ox {
struct Window;

class Input {
public:
  constexpr static auto MODULE_NAME = "Input";

  enum class CursorState { Disabled = 0x00034003, Normal = 0x00034001, Hidden = 0x00034002 };

  auto init() -> std::expected<void, std::string>;
  auto deinit() -> std::expected<void, std::string>;

  static void set_instance();

  void reset_pressed();
  void reset();

  static KeyCode to_keycode(SDL_Keycode keycode, SDL_Scancode scancode);
  static MouseCode to_mouse_code(SDL_MouseButtonFlags key);

  /// Keyboard
  static bool get_key_pressed(const KeyCode key) { return _instance->input_data.key_pressed[int(key)]; }
  static bool get_key_released(const KeyCode key) { return _instance->input_data.key_released[int(key)]; }
  static bool get_key_held(const KeyCode key) { return _instance->input_data.key_held[int(key)]; }

  /// Mouse
  static bool get_mouse_clicked(const MouseCode key) { return _instance->input_data.mouse_clicked[int(key)]; }
  static bool get_mouse_released(const MouseCode key) { return _instance->input_data.mouse_released[int(key)]; }
  static bool get_mouse_held(const MouseCode key) { return _instance->input_data.mouse_held[int(key)]; }
  static glm::vec2 get_mouse_position();
  static glm::vec2 get_mouse_position_rel();

  static void set_mouse_position_global(float x, float y);
  static void set_mouse_position_window(const Window& window, glm::vec2 position);

  static bool get_relative_mouse_mode_window(const Window& window);
  static void set_relative_mouse_mode_window(const Window& window, bool enabled);

  static float get_mouse_offset_x();
  static float get_mouse_offset_y();
  static float get_mouse_scroll_offset_y();
  static bool get_mouse_moved();

private:
#define MAX_KEYS 512
#define MAX_BUTTONS 32

  static Input* _instance;
  friend class App;

  struct InputData {
    bool key_pressed[MAX_KEYS] = {};
    bool key_released[MAX_KEYS] = {};
    bool key_held[MAX_KEYS] = {};
    bool mouse_held[MAX_BUTTONS] = {};
    bool mouse_clicked[MAX_BUTTONS] = {};
    bool mouse_released[MAX_BUTTONS] = {};

    float mouse_offset_x = {};
    float mouse_offset_y = {};
    float scroll_offset_y = {};
    glm::vec2 mouse_pos = {};
    glm::vec2 mouse_pos_rel = {};
    bool mouse_moved = false;
  } input_data = {};

  CursorState cursor_state = CursorState::Normal;

  void set_key_pressed(const KeyCode key, const bool a) { input_data.key_pressed[int(key)] = a; }
  void set_key_released(const KeyCode key, const bool a) { input_data.key_released[int(key)] = a; }
  void set_key_held(const KeyCode key, const bool a) { input_data.key_held[int(key)] = a; }
  void set_mouse_clicked(const MouseCode key, const bool a) { input_data.mouse_clicked[int(key)] = a; }
  void set_mouse_released(const MouseCode key, const bool a) { input_data.mouse_released[int(key)] = a; }
  void set_mouse_held(const MouseCode key, const bool a) { input_data.mouse_held[int(key)] = a; }
};
} // namespace ox
