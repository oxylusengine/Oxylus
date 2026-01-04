#pragma once

#include <ankerl/unordered_dense.h>
#include <expected>
#include <glm/ext/vector_float2.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "Core/Keycodes.hpp"

namespace ox {
struct Window;

enum class BindingError { ActionNotFound, InvalidInput, ContextNotFound };

enum class InputType : u8 { Keyboard, MouseButton, MouseAxis, GamepadButton, GamepadAxis };

struct InputCode {
  KeyCode key_code = KeyCode::None;
  MouseCode mouse_code = MouseCode::Left;
  GamepadButtonCode gamepad_button_code = GamepadButtonCode::None;
  GamepadAxisCode gamepad_axis_code = GamepadAxisCode::None;
  ModCode mod_code = ModCode::None;

  // For axes: which direction counts as "pressed"
  enum class AxisDirection { Positive, Negative, Both };
  AxisDirection axis_direction = AxisDirection::Both;

  InputType type;

  InputCode(KeyCode key_code_ = KeyCode::None, ModCode mod_code_ = ModCode::None)
      : key_code(key_code_),
        mod_code(mod_code_),
        type(InputType::Keyboard) {}

  InputCode(MouseCode mouse_code_ = MouseCode::Left, ModCode mod_code_ = ModCode::None)
      : mouse_code(mouse_code_),
        mod_code(mod_code_),
        type(InputType::MouseButton) {}

  InputCode(GamepadButtonCode gamepad_button_code_ = GamepadButtonCode::None, ModCode mod_code_ = ModCode::None)
      : gamepad_button_code(gamepad_button_code_),
        mod_code(mod_code_),
        type(InputType::GamepadButton) {}

  InputCode(
    GamepadAxisCode gamepad_axis_code_ = GamepadAxisCode::None,
    ModCode mod_code_ = ModCode::None,
    AxisDirection axis_direction_ = AxisDirection::Both
  )
      : gamepad_axis_code(gamepad_axis_code_),
        mod_code(mod_code_),
        axis_direction(axis_direction_),
        type(InputType::GamepadAxis) {}

  bool operator==(const InputCode&) const = default;

  struct Hash {
    usize operator()(const InputCode& ic) const {
      usize h1 = std::hash<i32>{}(static_cast<int>(ic.type));
      usize h2 = {};
      switch (ic.type) {
        case InputType::Keyboard     : h2 = std::hash<u16>{}(static_cast<u16>(ic.key_code)); break;
        case InputType::MouseButton  : h2 = std::hash<u16>{}(static_cast<u16>(ic.mouse_code)); break;
        case InputType::MouseAxis    : break;
        case InputType::GamepadButton: h2 = std::hash<u16>{}(static_cast<i16>(ic.gamepad_button_code)); break;
        case InputType::GamepadAxis  : h2 = std::hash<u16>{}(static_cast<i16>(ic.gamepad_axis_code)); break;
      }
      usize h3 = std::hash<u16>{}(static_cast<u16>(ic.mod_code));
      return h1 ^ (h2 << 1) ^ (h3 << 2);
    }
  };
};

enum class ActionType {
  Button, // Digital on/off
  Axis    // Analog -1 to 1
};

struct ActionBinding {
  std::string action_id = {};
  std::vector<InputCode> primary_inputs = {};
  std::vector<InputCode> secondary_inputs = {};
  std::string context = "default";

  // Axis-specific settings
  f32 dead_zone = 0.15f;
  f32 sensitivity = 1.0f;
  bool invert = false;
};

class Input {
public:
  constexpr static auto MODULE_NAME = "Input";

  enum class CursorState { Disabled = 0x00034003, Normal = 0x00034001, Hidden = 0x00034002 };

  auto init() -> std::expected<void, std::string>;
  auto deinit() -> std::expected<void, std::string>;

  auto reset_pressed() -> void;
  auto reset() -> void;

  /// Binding
  auto get_binding(this const Input& self, std::string_view action_id) -> const ActionBinding*;
  auto get_active_binding(this const Input& self, std::string_view action_id) -> const ActionBinding*;
  auto bind_action(this Input& self, ActionBinding binding) -> std::expected<void, BindingError>;
  auto unbind_action(this Input& self, std::string action_id) -> std::expected<void, BindingError>;
  auto rebind_action(
    this Input& self,
    std::string_view action_id,
    std::vector<InputCode> new_primary,
    std::vector<InputCode> new_secondary = {}
  ) -> std::expected<void, BindingError>;

  auto set_context(this Input& self, std::string_view context) -> void;
  auto push_context(this Input& self, std::string_view context) -> void;
  auto pop_context(this Input& self) -> void;
  auto get_active_context(this const Input& self) -> std::string_view;

  auto get_action_pressed(this const Input& self, std::string_view action_id) -> bool;
  auto get_action_released(this const Input& self, std::string_view action_id) -> bool;
  auto get_action_held(this const Input& self, std::string_view action_id) -> bool;
  auto get_action_axis(this const Input& self, std::string_view action_id) -> f32;

  /// Keyboard
  auto get_key_pressed(this const Input& self, const KeyCode key) -> bool;
  auto get_key_released(this const Input& self, const KeyCode key) -> bool;
  auto get_key_held(this const Input& self, const KeyCode key) -> bool;

  /// Mouse
  auto get_mouse_clicked(this const Input& self, const MouseCode key) -> bool;
  auto get_mouse_released(this const Input& self, const MouseCode key) -> bool;
  auto get_mouse_held(this const Input& self, const MouseCode key) -> bool;
  auto get_mouse_position(this const Input& self) -> glm::vec2;
  auto get_mouse_position_rel(this const Input& self) -> glm::vec2;

  auto set_mouse_position_global(float x, float y) -> void;
  auto set_mouse_position_window(const Window& window, glm::vec2 position) -> void;

  auto get_relative_mouse_mode_window(const Window& window) -> bool;
  auto set_relative_mouse_mode_window(const Window& window, bool enabled) -> void;

  auto get_mouse_offset_x() -> f32;
  auto get_mouse_offset_y() -> f32;
  auto get_mouse_scroll_offset_y() -> f32;
  auto get_mouse_moved() -> bool;

private:
  friend struct Window;

  struct InputData {
    ModCode mod_code = {};
    ankerl::unordered_dense::map<KeyCode, bool> key_pressed = {};
    ankerl::unordered_dense::map<KeyCode, bool> key_released = {};
    ankerl::unordered_dense::map<KeyCode, bool> key_held = {};
    ankerl::unordered_dense::map<MouseCode, bool> mouse_pressed = {};
    ankerl::unordered_dense::map<MouseCode, bool> mouse_released = {};
    ankerl::unordered_dense::map<MouseCode, bool> mouse_held = {};

    f32 mouse_offset_x = {};
    f32 mouse_offset_y = {};
    f32 scroll_offset_y = {};
    glm::vec2 mouse_pos = {};
    glm::vec2 mouse_pos_rel = {};
    bool mouse_moved = false;
  };

  InputData input_data = {};

  CursorState cursor_state = CursorState::Normal;

  std::string active_context = "default";
  std::vector<std::string> context_stack = {};
  std::unordered_multimap<std::string, ActionBinding> action_bindings = {};
  std::unordered_multimap<InputCode, std::string, InputCode::Hash> input_to_actions = {};

  auto find_conflicts(this const Input& self, const ActionBinding& binding) -> std::vector<std::string>;
  auto add_to_reverse_map(this Input& self, const ActionBinding& binding) -> void;
  auto remove_from_reverse_map(this Input& self, const ActionBinding& binding) -> void;

  enum class InputState { Pressed, Released, Held };
  auto check_input_active(this const Input& self, const InputCode& input, InputState check_state) -> bool;

  auto set_mod(const ModCode mod) -> void;
  auto set_key_pressed(const KeyCode key, const bool a) -> void;
  void set_key_released(const KeyCode key, const bool a);
  void set_key_held(const KeyCode key, const bool a);
  void set_mouse_clicked(const MouseCode key, const bool a);
  void set_mouse_released(const MouseCode key, const bool a);
  void set_mouse_held(const MouseCode key, const bool a);
};
} // namespace ox
