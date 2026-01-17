#include "Core/Input.hpp"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_mouse.h>

#include "Render/Window.hpp"

namespace ox {
auto Input::init() -> std::expected<void, std::string> { return {}; }

auto Input::deinit() -> std::expected<void, std::string> { return {}; }

auto Input::reset_pressed() -> void {
  ZoneScoped;

  input_data.keyboard_data.key_pressed.clear();
  input_data.keyboard_data.key_released.clear();
  input_data.mouse_data.mouse_pressed.clear();
  input_data.mouse_data.mouse_released.clear();
  input_data.mouse_data.scroll_offset_y = 0;
  input_data.mouse_data.mouse_moved = false;

  for (auto& [id, data] : input_data.gamepad_data_map) {
    data.gamepad_pressed.clear();
    data.gamepad_released.clear();
  }
}

auto Input::reset() -> void {
  ZoneScoped;

  input_data.keyboard_data = {};
  input_data.mouse_data = {};
  input_data.gamepad_data_map.clear();
}

auto Input::get_binding(this const Input& self, std::string_view action_id) -> const ActionBinding* {
  ZoneScoped;

  auto it = self.action_bindings.find(std::string(action_id));
  return it != self.action_bindings.end() ? &it->second : nullptr;
}

auto Input::get_active_binding(this const Input& self, std::string_view action_id) -> const ActionBinding* {
  ZoneScoped;

  auto binding = self.get_binding(action_id);
  if (binding && binding->context == self.active_context) {
    return binding;
  }
  return nullptr;
}

auto Input::bind_action(this Input& self, ActionBinding binding) -> std::expected<void, BindingError> {
  ZoneScoped;

  if (binding.action_id.empty()) {
    return std::unexpected(BindingError::InvalidInput);
  }

  // NOTE: Not sure if this should be an error
  if (auto conflicts = self.find_conflicts(binding); !conflicts.empty()) {
    // For now we'll allow it.
    OX_LOG_WARN("Conflicts found for action: {}", binding.action_id);
  }

  if (auto it = self.action_bindings.find(binding.action_id); it != self.action_bindings.end()) {
    self.remove_from_reverse_map(it->second);
  }

  self.add_to_reverse_map(binding);

  self.action_bindings.emplace(binding.action_id, binding);

  return {};
}

auto Input::unbind_action(this Input& self, std::string action_id) -> std::expected<void, BindingError> {
  ZoneScoped;

  auto it = self.action_bindings.find(std::string(action_id));
  if (it == self.action_bindings.end()) {
    return std::unexpected(BindingError::ActionNotFound);
  }

  self.remove_from_reverse_map(it->second);
  self.action_bindings.erase(it);

  return {};
}

auto Input::rebind_action(
  this Input& self, std::string_view action_id, std::vector<InputCode> new_primary, std::vector<InputCode> new_secondary
) -> std::expected<void, BindingError> {
  auto it = self.action_bindings.find(std::string(action_id));
  if (it == self.action_bindings.end()) {
    return std::unexpected(BindingError::ActionNotFound);
  }

  self.remove_from_reverse_map(it->second);

  it->second.primary_inputs = std::move(new_primary);
  it->second.secondary_inputs = std::move(new_secondary);

  self.add_to_reverse_map(it->second);

  return {};
}

auto Input::set_context(this Input& self, std::string_view context) -> void {
  ZoneScoped;

  self.active_context = context;
}

auto Input::push_context(this Input& self, std::string_view context) -> void {
  ZoneScoped;

  self.context_stack.emplace_back(self.active_context);
  self.active_context = context;
}

auto Input::pop_context(this Input& self) -> void {
  ZoneScoped;

  if (!self.context_stack.empty()) {
    self.active_context = self.context_stack.back();
    self.context_stack.pop_back();
  }
}

auto Input::get_active_context(this const Input& self) -> std::string_view {
  ZoneScoped;

  return self.active_context;
}

auto Input::get_action_pressed(this const Input& self, std::string_view action_id, u32 instance_id) -> bool {
  ZoneScoped;

  auto binding = self.get_active_binding(action_id);
  if (!binding) {
    return false;
  }

  for (const auto& input : binding->primary_inputs) {
    return self.check_input_active(input, instance_id, InputState::Pressed);
  }

  for (const auto& input : binding->secondary_inputs) {
    return self.check_input_active(input, instance_id, InputState::Pressed);
  }

  return false;
}

auto Input::get_action_released(this const Input& self, std::string_view action_id, u32 instance_id) -> bool {
  ZoneScoped;

  auto binding = self.get_active_binding(action_id);
  if (!binding) {
    return false;
  }

  for (const auto& input : binding->primary_inputs) {
    return self.check_input_active(input, instance_id, InputState::Released);
  }

  for (const auto& input : binding->secondary_inputs) {
    return self.check_input_active(input, instance_id, InputState::Released);
  }

  return false;
}

auto Input::get_action_held(this const Input& self, std::string_view action_id, u32 instance_id) -> bool {
  ZoneScoped;

  auto binding = self.get_active_binding(action_id);
  if (!binding) {
    return false;
  }

  for (const auto& input : binding->primary_inputs) {
    return self.check_input_active(input, instance_id, InputState::Held);
  }

  for (const auto& input : binding->secondary_inputs) {
    return self.check_input_active(input, instance_id, InputState::Held);
  }

  return false;
}

auto Input::get_action_axis(this const Input& self, std::string_view action_id, u32 instance_id) -> glm::vec2 {
  ZoneScoped;

  auto binding = self.get_active_binding(action_id);
  if (!binding) {
    return {};
  }

  const auto get_axis = [](const InputCode& input, glm::vec2 axis) {
    switch (input.mouse_axis_code) {
      case MouseAxisCode::AxisX : return glm::vec2(axis.x);
      case MouseAxisCode::AxisY : return glm::vec2(axis.y);
      case MouseAxisCode::AxisXY: return axis;
      default                   : break;
    }
    switch (input.gamepad_axis_code) {
      case GamepadAxisCode::AxisLeftX       : return glm::vec2(axis.x);
      case GamepadAxisCode::AxisLeftY       : return glm::vec2(axis.y);
      case GamepadAxisCode::AxisRightX      : return glm::vec2(axis.x);
      case GamepadAxisCode::AxisRightY      : return glm::vec2(axis.y);
      case GamepadAxisCode::AxisLeftTrigger : return glm::vec2(axis.x);
      case GamepadAxisCode::AxisRightTrigger: return glm::vec2(axis.y);
      case GamepadAxisCode::None            : break;
    }

    return glm::vec2{};
  };

  for (const auto& input : binding->primary_inputs) {
    const auto axis = self.check_input_axis(input, instance_id, input.type);

    if (axis.has_value()) {
      return get_axis(input, *axis);
    }
  }

  for (const auto& input : binding->secondary_inputs) {
    const auto axis = self.check_input_axis(input, instance_id, input.type);

    if (axis.has_value()) {
      return get_axis(input, *axis);
    }
  }

  return {};
}

auto Input::get_key_pressed(this const Input& self, const KeyCode key) -> bool {
  ZoneScoped;

  const auto it = self.input_data.keyboard_data.key_pressed.find(key);
  if (it != self.input_data.keyboard_data.key_pressed.end()) {
    return it->second;
  }

  return false;
}

auto Input::get_key_released(this const Input& self, const KeyCode key) -> bool {
  ZoneScoped;

  const auto it = self.input_data.keyboard_data.key_released.find(key);
  if (it != self.input_data.keyboard_data.key_released.end()) {
    return it->second;
  }

  return false;
}

auto Input::get_key_held(this const Input& self, const KeyCode key) -> bool {
  ZoneScoped;

  const auto it = self.input_data.keyboard_data.key_held.find(key);
  if (it != self.input_data.keyboard_data.key_held.end()) {
    return it->second;
  }

  return false;
}

auto Input::get_mouse_clicked(this const Input& self, const MouseCode key) -> bool {
  ZoneScoped;

  const auto it = self.input_data.mouse_data.mouse_pressed.find(key);
  if (it != self.input_data.mouse_data.mouse_pressed.end()) {
    return it->second;
  }

  return false;
}

auto Input::get_mouse_released(this const Input& self, const MouseCode key) -> bool {
  ZoneScoped;

  const auto it = self.input_data.mouse_data.mouse_released.find(key);
  if (it != self.input_data.mouse_data.mouse_released.end()) {
    return it->second;
  }

  return false;
}

auto Input::get_mouse_held(this const Input& self, const MouseCode key) -> bool {
  ZoneScoped;

  const auto it = self.input_data.mouse_data.mouse_held.find(key);
  if (it != self.input_data.mouse_data.mouse_held.end()) {
    return it->second;
  }

  return false;
}

auto Input::get_mouse_position(this const Input& self) -> glm::vec2 {
  ZoneScoped;

  return self.input_data.mouse_data.mouse_pos;
}

auto Input::get_mouse_position_rel(this const Input& self) -> glm::vec2 {
  ZoneScoped;

  return self.input_data.mouse_data.mouse_pos_rel;
}

auto Input::get_mouse_offset_x(this const Input& self) -> f32 {
  ZoneScoped;

  return self.input_data.mouse_data.mouse_offset_x;
}

auto Input::get_mouse_offset_y(this const Input& self) -> f32 {
  ZoneScoped;

  return self.input_data.mouse_data.mouse_offset_y;
}

auto Input::get_mouse_scroll_offset_y(this const Input& self) -> f32 {
  ZoneScoped;

  return self.input_data.mouse_data.scroll_offset_y;
}

auto Input::get_mouse_moved(this const Input& self) -> bool {
  ZoneScoped;

  return self.input_data.mouse_data.mouse_moved;
}

auto Input::set_mouse_position_global(const float x, const float y) -> void {
  ZoneScoped;

  SDL_WarpMouseGlobal(x, y);
}

auto Input::get_relative_mouse_mode_window(const Window& window) -> bool {
  ZoneScoped;

  return SDL_GetWindowRelativeMouseMode(static_cast<SDL_Window*>(window.get_handle()));
}

auto Input::set_relative_mouse_mode_window(const Window& window, bool enabled) -> void {
  ZoneScoped;

  SDL_SetWindowRelativeMouseMode(static_cast<SDL_Window*>(window.get_handle()), enabled);
}

auto Input::set_mouse_position_window(const Window& window, glm::vec2 position) -> void {
  ZoneScoped;

  SDL_WarpMouseInWindow(static_cast<SDL_Window*>(window.get_handle()), position.x, position.y);
}

auto Input::get_gamepad_button_pressed(this const Input& self, u32 instance_id, const GamepadButtonCode button)
  -> bool {
  ZoneScoped;

  const auto it = self.input_data.gamepad_data_map.find(instance_id);
  if (it != self.input_data.gamepad_data_map.end()) {
    const auto button_it = it->second.gamepad_pressed.find(button);
    if (button_it != it->second.gamepad_pressed.end()) {
      return button_it->second;
    }
  }

  return false;
}

auto Input::get_gamepad_button_released(this const Input& self, u32 instance_id, const GamepadButtonCode button)
  -> bool {
  ZoneScoped;

  const auto it = self.input_data.gamepad_data_map.find(instance_id);
  if (it != self.input_data.gamepad_data_map.end()) {
    const auto button_it = it->second.gamepad_released.find(button);
    if (button_it != it->second.gamepad_released.end()) {
      return button_it->second;
    }
  }

  return false;
}

auto Input::get_gamepad_button_held(this const Input& self, u32 instance_id, const GamepadButtonCode button) -> bool {
  ZoneScoped;

  const auto it = self.input_data.gamepad_data_map.find(instance_id);
  if (it != self.input_data.gamepad_data_map.end()) {
    const auto button_it = it->second.gamepad_held.find(button);
    if (button_it != it->second.gamepad_held.end()) {
      return button_it->second;
    }
  }

  return false;
}

auto Input::get_gamepad_axis(this const Input& self, u32 instance_id, const GamepadAxisCode axis) -> f32 {
  ZoneScoped;

  const auto it = self.input_data.gamepad_data_map.find(instance_id);
  if (it != self.input_data.gamepad_data_map.end()) {
    const auto axis_it = it->second.gamepad_axises.find(axis);
    if (axis_it != it->second.gamepad_axises.end()) {
      return axis_it->second;
    }
  }

  return 0.f;
}

auto Input::set_mod(const ModCode mod) -> void {
  ZoneScoped;

  input_data.mod_code = mod;
}

auto Input::set_default_keyboard_id(u32 instance_id) -> void {
  ZoneScoped;

  if (default_keyboard_id != DEFAULT_INSTANCE_ID) {
    return;
  }

  default_keyboard_id = instance_id;
}

auto Input::set_key_pressed(const KeyCode key, const bool state) -> void {
  ZoneScoped;

  input_data.keyboard_data.key_pressed.insert_or_assign(key, state);

  if (state) {
    for (const auto& [id, binding] : action_bindings) {
      do_callback(binding, DEFAULT_INSTANCE_ID, InputState::Pressed, InputType::Keyboard);
    }
  }
}

void Input::set_key_released(const KeyCode key, const bool state) {
  ZoneScoped;

  input_data.keyboard_data.key_released.insert_or_assign(key, state);

  if (state) {
    for (const auto& [id, binding] : action_bindings) {
      do_callback(binding, DEFAULT_INSTANCE_ID, InputState::Released, InputType::Keyboard);
    }
  }
}

void Input::set_key_held(const KeyCode key, const bool state) {
  ZoneScoped;

  input_data.keyboard_data.key_held.insert_or_assign(key, state);

  if (state) {
    for (const auto& [id, binding] : action_bindings) {
      do_callback(binding, DEFAULT_INSTANCE_ID, InputState::Held, InputType::Keyboard);
    }
  }
}

void Input::set_mouse_clicked(const MouseCode key, const bool state) {
  ZoneScoped;

  input_data.mouse_data.mouse_pressed.insert_or_assign(key, state);

  if (state) {
    for (const auto& [id, binding] : action_bindings) {
      do_callback(binding, DEFAULT_INSTANCE_ID, InputState::Pressed, InputType::MouseButton);
    }
  }
}

void Input::set_mouse_released(const MouseCode key, const bool state) {
  ZoneScoped;

  input_data.mouse_data.mouse_released.insert_or_assign(key, state);

  if (state) {
    for (const auto& [id, binding] : action_bindings) {
      do_callback(binding, DEFAULT_INSTANCE_ID, InputState::Released, InputType::MouseButton);
    }
  }
}

void Input::set_mouse_held(const MouseCode key, const bool state) {
  ZoneScoped;

  input_data.mouse_data.mouse_held.insert_or_assign(key, state);

  if (state) {
    for (const auto& [id, binding] : action_bindings) {
      do_callback(binding, DEFAULT_INSTANCE_ID, InputState::Held, InputType::MouseButton);
    }
  }
}

void Input::set_mouse_position(const glm::vec2& position) {
  ZoneScoped;

  input_data.mouse_data.mouse_pos = position;

  for (const auto& [id, binding] : action_bindings) {
    do_callback(binding, DEFAULT_INSTANCE_ID, InputState::None, InputType::MouseAxis);
  }
}

void Input::set_mouse_position_rel(const glm::vec2& position) {
  ZoneScoped;

  input_data.mouse_data.mouse_pos_rel = position;
}

void Input::set_mouse_scroll_offset_y(const f32 offset) {
  ZoneScoped;

  input_data.mouse_data.scroll_offset_y = offset;
}

void Input::set_mouse_moved(bool state) {
  ZoneScoped;

  input_data.mouse_data.mouse_moved = state;
}

auto Input::set_gamepad_button_pressed(u32 instance_id, const GamepadButtonCode button, const bool state) -> void {
  ZoneScoped;

  const auto it = input_data.gamepad_data_map.find(instance_id);
  if (it != input_data.gamepad_data_map.end()) {
    it->second.gamepad_pressed.insert_or_assign(button, state);
    it->second.gamepad_held.insert_or_assign(button, true);
  } else {
    auto data = InputData::GamepadData{};
    data.gamepad_pressed.insert_or_assign(button, state);
    data.gamepad_held.insert_or_assign(button, true);
    input_data.gamepad_data_map.insert_or_assign(instance_id, data);
  }

  if (state) {
    for (const auto& [id, binding] : action_bindings) {
      do_callback(binding, instance_id, InputState::Pressed, InputType::GamepadButton);
    }
  }
}

auto Input::set_gamepad_button_released(u32 instance_id, const GamepadButtonCode button, const bool state) -> void {
  ZoneScoped;

  const auto it = input_data.gamepad_data_map.find(instance_id);
  if (it != input_data.gamepad_data_map.end()) {
    it->second.gamepad_released.insert_or_assign(button, state);
    it->second.gamepad_held.insert_or_assign(button, false);
  } else {
    auto data = InputData::GamepadData{};
    data.gamepad_released.insert_or_assign(button, state);
    data.gamepad_held.insert_or_assign(button, false);
    input_data.gamepad_data_map.insert_or_assign(instance_id, data);
  }

  if (state) {
    for (const auto& [id, binding] : action_bindings) {
      do_callback(binding, instance_id, InputState::Pressed, InputType::GamepadButton);
    }
  }
}

auto Input::set_gamepad_axis(u32 instance_id, const GamepadAxisCode axis, f32 value) -> void {
  ZoneScoped;

  const auto it = input_data.gamepad_data_map.find(instance_id);
  if (it != input_data.gamepad_data_map.end()) {
    it->second.gamepad_axises.insert_or_assign(axis, value);
  } else {
    auto data = InputData::GamepadData{};
    data.gamepad_axises.insert_or_assign(axis, value);
    input_data.gamepad_data_map.insert_or_assign(instance_id, data);
  }

  for (const auto& [id, binding] : action_bindings) {
    do_callback(binding, instance_id, InputState::None, InputType::GamepadAxis);
  }
}

auto Input::check_input_active(
  this const Input& self, const InputCode& input, const u32 instance_id, InputState check_state
) -> bool {
  auto is_axis = input.type == InputType::GamepadAxis || input.type == InputType::MouseAxis;
  if (is_axis) {
    return false;
  }

  const auto check = [&self, &input, instance_id, check_state] {
    switch (input.type) {
      case InputType::Any     : OX_ASSERT(false, "Input type can not be Any!"); break;
      case InputType::Keyboard: {
        switch (check_state) {
          case InputState::Pressed : return self.get_key_pressed(input.key_code);
          case InputState::Released: return self.get_key_released(input.key_code);
          case InputState::Held    : return self.get_key_held(input.key_code);
          case InputState::None    : return false;
        }
        break;
      }
      case InputType::MouseButton: {
        switch (check_state) {
          case InputState::Pressed : return self.get_mouse_clicked(input.mouse_code);
          case InputState::Released: return self.get_mouse_released(input.mouse_code);
          case InputState::Held    : return self.get_mouse_held(input.mouse_code);
          case InputState::None    : return false;
        }
        break;
      }
      case InputType::MouseAxis    : break;
      case InputType::GamepadButton: {
        switch (check_state) {
          case InputState::Pressed : return self.get_gamepad_button_pressed(instance_id, input.gamepad_button_code);
          case InputState::Released: return self.get_gamepad_button_released(instance_id, input.gamepad_button_code);
          case InputState::Held    : return self.get_gamepad_button_held(instance_id, input.gamepad_button_code);
          case InputState::None    : return false;
        }
        break;
      }
      case InputType::GamepadAxis: break;
    }

    return false;
  };

  bool input_active = check();
  if (!input_active) {
    return false;
  }

  if (input.mod_code != ModCode::None) {
    return mod_matches(self.input_data.mod_code, input.mod_code);
  }

  return true;
}

auto Input::do_callback(
  this const Input& self,
  const ActionBinding& binding,
  const u32 instance_id,
  const InputState state,
  InputType callback_type
) -> void {
  bool active = false;
  option<glm::vec2> axis = nullopt;

  const auto axis_callback = callback_type == InputType::MouseAxis || callback_type == InputType::GamepadAxis;

  for (const auto& input : binding.primary_inputs) {
    if (input.type != callback_type)
      continue;

    if (axis_callback) {
      axis = self.check_input_axis(input, instance_id, input.type);
      active = true;
      break;
    }
    if (self.check_input_active(input, instance_id, state)) {
      active = true;
      break;
    }
  }

  if (!active) {
    for (const auto& input : binding.secondary_inputs) {
      if (input.type != callback_type)
        continue;

      if (axis_callback) {
        axis = self.check_input_axis(input, instance_id, input.type);
        active = true;
        break;
      }
      if (self.check_input_active(input, instance_id, state)) {
        active = true;
        break;
      }
    }
  }

  if (active) {
    if (axis.has_value()) {
      if (callback_type == InputType::MouseAxis) {
        if (binding.on_mouse_axis_callback) {
          binding.on_mouse_axis_callback(
            ActionContext{.action_id = binding.action_id, .instance_id = instance_id, .axis_value = *axis}
          );
        }
        return;
      }
      if (callback_type == InputType::GamepadAxis) {
        if (binding.on_gamepad_axis_callback) {
          binding.on_gamepad_axis_callback(
            ActionContext{.action_id = binding.action_id, .instance_id = instance_id, .axis_value = *axis}
          );
        }
        return;
      }
    }

    switch (state) {
      case InputState::None   : break;
      case InputState::Pressed: {
        if (binding.on_pressed_callback) {
          binding.on_pressed_callback(ActionContext{.action_id = binding.action_id, .instance_id = instance_id});
        }
        break;
      }
      case InputState::Released: {
        if (binding.on_released_callback) {
          binding.on_released_callback(ActionContext{.action_id = binding.action_id, .instance_id = instance_id});
        }
        break;
      }
      case InputState::Held: {
        if (binding.on_held_callback) {
          binding.on_held_callback(ActionContext{.action_id = binding.action_id, .instance_id = instance_id});
        }
        break;
      }
    }
  }
}

auto Input::check_input_axis(
  this const Input& self, const InputCode& input, const u32 instance_id, InputType check_type
) -> option<glm::vec2> {
  ZoneScoped;

  switch (check_type) {
    case InputType::MouseAxis: {
      switch (input.mouse_axis_code) {
        case MouseAxisCode::None  : return nullopt;
        case MouseAxisCode::AxisX : return glm::vec2(self.get_mouse_position().x);
        case MouseAxisCode::AxisY : return glm::vec2(self.get_mouse_position().y);
        case MouseAxisCode::AxisXY: return self.get_mouse_position();
      }
    }
    case InputType::GamepadAxis: return glm::vec2(self.get_gamepad_axis(instance_id, input.gamepad_axis_code));
    default                    : return nullopt;
  }

  return nullopt;
}

auto Input::find_conflicts(this const Input& self, const ActionBinding& binding) -> std::vector<std::string> {
  ZoneScoped;

  std::vector<std::string> conflicts;

  auto check_inputs = [&self, &conflicts, &binding](const std::vector<InputCode>& inputs) {
    for (const auto& input : inputs) {
      auto range = self.input_to_actions.equal_range(input);
      for (auto it = range.first; it != range.second; ++it) {
        const auto& existing_action = it->second;
        // Only conflict if same context
        if (auto existing_binding = self.get_binding(existing_action);
            existing_binding && existing_binding->context == binding.context && existing_action != binding.action_id) {
          conflicts.push_back(existing_action);
        }
      }
    }
  };

  check_inputs(binding.primary_inputs);
  check_inputs(binding.secondary_inputs);

  std::ranges::sort(conflicts);
  auto [first, last] = std::ranges::unique(conflicts);
  conflicts.erase(first, last);

  return conflicts;
}

auto Input::add_to_reverse_map(this Input& self, const ActionBinding& binding) -> void {
  ZoneScoped;

  auto add_inputs = [&self, &binding](const std::vector<InputCode>& inputs) {
    for (const auto& input : inputs) {
      self.input_to_actions.emplace(input, binding.action_id);
    }
  };

  add_inputs(binding.primary_inputs);
  add_inputs(binding.secondary_inputs);
}

auto Input::remove_from_reverse_map(this Input& self, const ActionBinding& binding) -> void {
  ZoneScoped;

  auto remove_inputs = [&self, binding](const std::vector<InputCode>& inputs) {
    for (const auto& input : inputs) {
      auto range = self.input_to_actions.equal_range(input);
      for (auto it = range.first; it != range.second;) {
        if (it->second == binding.action_id) {
          it = self.input_to_actions.erase(it);
        } else {
          ++it;
        }
      }
    }
  };

  remove_inputs(binding.primary_inputs);
  remove_inputs(binding.secondary_inputs);
}
} // namespace ox
