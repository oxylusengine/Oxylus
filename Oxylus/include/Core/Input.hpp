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

  auto reset_pressed() -> void;
  auto reset() -> void;

  static auto to_keycode(u32 keycode_, u32 scancode_) -> KeyCode;
  static auto to_mouse_code(u32 key) -> MouseCode;

  /// Keyboard
  auto get_key_pressed(const KeyCode key) -> bool { return input_data.key_pressed[int(key)]; }
  auto get_key_released(const KeyCode key) -> bool { return input_data.key_released[int(key)]; }
  auto get_key_held(const KeyCode key) -> bool { return input_data.key_held[int(key)]; }

  /// Mouse
  auto get_mouse_clicked(const MouseCode key) -> bool { return input_data.mouse_clicked[int(key)]; }
  auto get_mouse_released(const MouseCode key) -> bool { return input_data.mouse_released[int(key)]; }
  auto get_mouse_held(const MouseCode key) -> bool { return input_data.mouse_held[int(key)]; }
  auto get_mouse_position() -> glm::vec2;
  auto get_mouse_position_rel() -> glm::vec2;

  auto set_mouse_position_global(float x, float y) -> void;
  auto set_mouse_position_window(const Window& window, glm::vec2 position) -> void;

  auto get_relative_mouse_mode_window(const Window& window) -> bool;
  auto set_relative_mouse_mode_window(const Window& window, bool enabled) -> void;

  auto get_mouse_offset_x() -> f32;
  auto get_mouse_offset_y() -> f32;
  auto get_mouse_scroll_offset_y() -> f32;
  auto get_mouse_moved() -> bool;

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
  };

  InputData input_data = {};

  CursorState cursor_state = CursorState::Normal;

  void set_key_pressed(const KeyCode key, const bool a) { input_data.key_pressed[int(key)] = a; }
  void set_key_released(const KeyCode key, const bool a) { input_data.key_released[int(key)] = a; }
  void set_key_held(const KeyCode key, const bool a) { input_data.key_held[int(key)] = a; }
  void set_mouse_clicked(const MouseCode key, const bool a) { input_data.mouse_clicked[int(key)] = a; }
  void set_mouse_released(const MouseCode key, const bool a) { input_data.mouse_released[int(key)] = a; }
  void set_mouse_held(const MouseCode key, const bool a) { input_data.mouse_held[int(key)] = a; }
};
} // namespace ox
