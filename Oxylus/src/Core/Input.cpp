#include "Core/Input.hpp"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>

#include "Render/Window.hpp"

namespace ox {
auto Input::init() -> std::expected<void, std::string> { return {}; }

auto Input::deinit() -> std::expected<void, std::string> { return {}; }

auto Input::reset_pressed() -> void {
  ZoneScoped;

  input_data.key_pressed.clear();
  input_data.mouse_pressed.clear();
  input_data.scroll_offset_y = 0;
  input_data.mouse_moved = false;
}

auto Input::reset() -> void {
  ZoneScoped;

  input_data.key_pressed.clear();
  input_data.key_held.clear();
  input_data.key_released.clear();
  input_data.mouse_pressed.clear();
  input_data.mouse_held.clear();
  input_data.mouse_released.clear();

  input_data.scroll_offset_y = 0;
  input_data.mouse_moved = false;
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

auto Input::do_callback(const ActionBinding& binding, const InputState state) -> void {
  bool active = false;

  for (const auto& input : binding.primary_inputs) {
    if (check_input_active(input, state)) {
      active = true;
      break;
    }
  }

  if (!active) {
    for (const auto& input : binding.secondary_inputs) {
      if (check_input_active(input, state)) {
        active = true;
        break;
      }
    }
  }

  if (active) {
    switch (state) {
      case InputState::None   : break;
      case InputState::Pressed: {
        if (binding.on_pressed_callback) {
          binding.on_pressed_callback(ActionContext{.action_id = binding.action_id});
        }
        break;
      }
      case InputState::Released: {
        if (binding.on_released_callback) {
          binding.on_released_callback(ActionContext{.action_id = binding.action_id});
        }
        break;
      }
      case InputState::Held: {
        if (binding.on_held_callback) {
          binding.on_held_callback(ActionContext{.action_id = binding.action_id});
        }
        break;
      }
    }
  }
}

auto Input::check_input_active(this const Input& self, const InputCode& input, InputState check_state) -> bool {
  auto is_axis = input.type == InputType::GamepadAxis || input.type == InputType::MouseAxis;
  if (is_axis) {
    return false;
  }

  const auto check = [&self, &input, check_state] {
    switch (input.type) {
      case InputType::Keyboard: {
        switch (check_state) {
          case InputState::Pressed : return self.get_key_pressed(input.key_code);
          case InputState::Released: return self.get_key_released(input.key_code);
          case InputState::Held    : return self.get_key_held(input.key_code);
          case InputState::None    : return false;
        }
      }
      case InputType::MouseButton: {
        switch (check_state) {
          case InputState::Pressed : return self.get_mouse_clicked(input.mouse_code);
          case InputState::Released: return self.get_mouse_released(input.mouse_code);
          case InputState::Held    : return self.get_mouse_held(input.mouse_code);
          case InputState::None    : return false;
        }
      }
      case InputType::MouseAxis:
      case InputType::GamepadButton:
      case InputType::GamepadAxis:
        /* TODO: */ break;
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

auto Input::get_action_pressed(this const Input& self, std::string_view action_id) -> bool {
  ZoneScoped;

  auto binding = self.get_active_binding(action_id);
  if (!binding) {
    return false;
  }

  for (const auto& input : binding->primary_inputs) {
    return self.check_input_active(input, InputState::Pressed);
  }

  for (const auto& input : binding->secondary_inputs) {
    return self.check_input_active(input, InputState::Pressed);
  }

  return false;
}

auto Input::get_action_released(this const Input& self, std::string_view action_id) -> bool {
  ZoneScoped;

  auto binding = self.get_active_binding(action_id);
  if (!binding) {
    return false;
  }

  for (const auto& input : binding->primary_inputs) {
    return self.check_input_active(input, InputState::Released);
  }

  for (const auto& input : binding->secondary_inputs) {
    return self.check_input_active(input, InputState::Released);
  }

  return false;
}

auto Input::get_action_held(this const Input& self, std::string_view action_id) -> bool {
  ZoneScoped;

  auto binding = self.get_active_binding(action_id);
  if (!binding) {
    return false;
  }

  for (const auto& input : binding->primary_inputs) {
    return self.check_input_active(input, InputState::Held);
  }

  for (const auto& input : binding->secondary_inputs) {
    return self.check_input_active(input, InputState::Held);
  }

  return false;
}

auto Input::get_action_axis(this const Input& self, std::string_view action_id) -> f32 {
  ZoneScoped;

  // TODO:

  return 0.f;
}

auto Input::get_key_pressed(this const Input& self, const KeyCode key) -> bool {
  ZoneScoped;

  auto it = self.input_data.key_pressed.find(key);
  if (it != self.input_data.key_pressed.end()) {
    return it->second;
  }

  return false;
}

auto Input::get_key_released(this const Input& self, const KeyCode key) -> bool {
  ZoneScoped;

  auto it = self.input_data.key_released.find(key);
  if (it != self.input_data.key_released.end()) {
    return it->second;
  }

  return false;
}

auto Input::get_key_held(this const Input& self, const KeyCode key) -> bool {
  ZoneScoped;

  auto it = self.input_data.key_held.find(key);
  if (it != self.input_data.key_held.end()) {
    return it->second;
  }

  return false;
}

auto Input::get_mouse_clicked(this const Input& self, const MouseCode key) -> bool {
  ZoneScoped;

  auto it = self.input_data.mouse_pressed.find(key);
  if (it != self.input_data.mouse_pressed.end()) {
    return it->second;
  }

  return false;
}

auto Input::get_mouse_released(this const Input& self, const MouseCode key) -> bool {
  ZoneScoped;

  auto it = self.input_data.mouse_released.find(key);
  if (it != self.input_data.mouse_released.end()) {
    return it->second;
  }

  return false;
}

auto Input::get_mouse_held(this const Input& self, const MouseCode key) -> bool {
  ZoneScoped;

  auto it = self.input_data.mouse_held.find(key);
  if (it != self.input_data.mouse_held.end()) {
    return it->second;
  }

  return false;
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

auto Input::get_mouse_position(this const Input& self) -> glm::vec2 { return self.input_data.mouse_pos; }

auto Input::get_mouse_position_rel(this const Input& self) -> glm::vec2 { return self.input_data.mouse_pos_rel; }

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

auto Input::get_mouse_offset_x() -> f32 { return input_data.mouse_offset_x; }

auto Input::get_mouse_offset_y() -> f32 { return input_data.mouse_offset_y; }

auto Input::get_mouse_scroll_offset_y() -> f32 { return input_data.scroll_offset_y; }

auto Input::get_mouse_moved() -> bool { return input_data.mouse_moved; }

auto Input::set_mod(const ModCode mod) -> void {
  ZoneScoped;

  input_data.mod_code = mod;
}

auto Input::set_key_pressed(const KeyCode key, const bool state) -> void {
  ZoneScoped;

  input_data.key_pressed.insert_or_assign(key, state);

  if (state) {
    for (const auto& [id, binding] : action_bindings) {
      do_callback(binding, InputState::Pressed);
    }
  }
}

void Input::set_key_released(const KeyCode key, const bool state) {
  ZoneScoped;

  input_data.key_released.insert_or_assign(key, state);

  if (state) {
    for (const auto& [id, binding] : action_bindings) {
      do_callback(binding, InputState::Released);
    }
  }
}

void Input::set_key_held(const KeyCode key, const bool state) {
  ZoneScoped;

  input_data.key_held.insert_or_assign(key, state);

  if (state) {
    for (const auto& [id, binding] : action_bindings) {
      do_callback(binding, InputState::Held);
    }
  }
}

void Input::set_mouse_clicked(const MouseCode key, const bool state) {
  ZoneScoped;

  input_data.mouse_pressed.insert_or_assign(key, state);

  if (state) {
    for (const auto& [id, binding] : action_bindings) {
      do_callback(binding, InputState::Pressed);
    }
  }
}

void Input::set_mouse_released(const MouseCode key, const bool state) {
  ZoneScoped;

  input_data.mouse_released.insert_or_assign(key, state);

  if (state) {
    for (const auto& [id, binding] : action_bindings) {
      do_callback(binding, InputState::Released);
    }
  }
}

void Input::set_mouse_held(const MouseCode key, const bool state) {
  ZoneScoped;

  input_data.mouse_held.insert_or_assign(key, state);

  if (state) {
    for (const auto& [id, binding] : action_bindings) {
      do_callback(binding, InputState::Held);
    }
  }
}

auto Input::set_gamepad_button_pressed(u32 instance_id, const GamepadButtonCode button, const bool state) -> void {
  ZoneScoped;

  input_data.gamepad_data_array[instance_id].gamepad_pressed.insert_or_assign(button, state);

  if (state) {
    for (const auto& [id, binding] : action_bindings) {
      do_callback(binding, InputState::Pressed);
    }
  }
}

auto Input::set_gamepad_button_released(u32 instance_id, const GamepadButtonCode button, const bool state) -> void {
  ZoneScoped;

  input_data.gamepad_data_array[instance_id].gamepad_released.insert_or_assign(button, state);

  if (state) {
    for (const auto& [id, binding] : action_bindings) {
      do_callback(binding, InputState::Pressed);
    }
  }
}
} // namespace ox
