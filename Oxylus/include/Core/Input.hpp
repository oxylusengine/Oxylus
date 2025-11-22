#pragma once

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

  void reset_pressed();
  void reset();

  static KeyCode to_keycode(u32 keycode_, u32 scancode_);
  static MouseCode to_mouse_code(u32 key);

  /// Keyboard
  bool get_key_pressed(const KeyCode key) { return input_data.key_pressed[int(key)]; }
  bool get_key_released(const KeyCode key) { return input_data.key_released[int(key)]; }
  bool get_key_held(const KeyCode key) { return input_data.key_held[int(key)]; }

  /// Mouse
  bool get_mouse_clicked(const MouseCode key) { return input_data.mouse_clicked[int(key)]; }
  bool get_mouse_released(const MouseCode key) { return input_data.mouse_released[int(key)]; }
  bool get_mouse_held(const MouseCode key) { return input_data.mouse_held[int(key)]; }
  glm::vec2 get_mouse_position();
  glm::vec2 get_mouse_position_rel();

  void set_mouse_position_global(float x, float y);
  void set_mouse_position_window(const Window& window, glm::vec2 position);

  bool get_relative_mouse_mode_window(const Window& window);
  void set_relative_mouse_mode_window(const Window& window, bool enabled);

  float get_mouse_offset_x();
  float get_mouse_offset_y();
  float get_mouse_scroll_offset_y();
  bool get_mouse_moved();

private:
#define MAX_KEYS 512
#define MAX_BUTTONS 32

  friend struct Window;

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
