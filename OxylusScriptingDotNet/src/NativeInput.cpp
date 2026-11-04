#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_stdinc.h>
#include <algorithm>
#include <string_view>

#include "Core/App.hpp"
#include "Core/Input.hpp"
#include "DotNetHosts.hpp"
#include "Render/Window.hpp"

namespace ox {
static auto to_view(OxStringView view) -> std::string_view { return std::string_view(view.data, view.length); }

static auto input() -> Input* { return App::get() && App::has_mod<Input>() ? &App::mod<Input>() : nullptr; }

static auto to_input_code(const OxInputCode& code) -> option<InputCode> {
  const auto mod = static_cast<ModCode>(code.mod);
  switch (static_cast<InputType>(code.type)) {
    case InputType::Keyboard     : return InputCode(static_cast<ScanCode>(code.code), mod);
    case InputType::MouseButton  : return InputCode(static_cast<MouseCode>(code.code), mod);
    case InputType::MouseAxis    : return InputCode(static_cast<MouseAxisCode>(code.code));
    case InputType::GamepadButton: return InputCode(static_cast<GamepadButtonCode>(code.code), mod);
    case InputType::GamepadAxis:
      if (code.axis_direction > static_cast<u8>(InputCode::AxisDirection::Both)) {
        return nullopt;
      }

      return InputCode(
        static_cast<GamepadAxisCode>(code.code),
        static_cast<InputCode::AxisDirection>(code.axis_direction)
      );
    case InputType::Any: return nullopt;
  }

  return nullopt;
}

static auto input_key(u32 scan_code, OxInputState state) -> bool {
  const auto* in = input();
  if (!in) {
    return false;
  }

  const auto key = static_cast<ScanCode>(scan_code);
  switch (state) {
    case OxInputState::Pressed : return in->get_key_pressed(key);
    case OxInputState::Released: return in->get_key_released(key);
    case OxInputState::Held    : return in->get_key_held(key);
  }

  return false;
}

static auto input_mouse_button(u32 mouse_code, OxInputState state) -> bool {
  const auto* in = input();
  if (!in) {
    return false;
  }

  const auto button = static_cast<MouseCode>(mouse_code);
  switch (state) {
    case OxInputState::Pressed : return in->get_mouse_clicked(button);
    case OxInputState::Released: return in->get_mouse_released(button);
    case OxInputState::Held    : return in->get_mouse_held(button);
  }

  return false;
}

static auto input_mouse(OxMouseState* state) -> void {
  *state = {};
  const auto* in = input();
  if (!in) {
    return;
  }

  const auto position = in->get_mouse_position();
  const auto delta = in->get_mouse_position_rel();
  *state = OxMouseState{
    .position_x = position.x,
    .position_y = position.y,
    .delta_x = delta.x,
    .delta_y = delta.y,
    .scroll = in->get_mouse_scroll_offset_y(),
    .is_moved = in->get_mouse_moved() ? 1_u32 : 0_u32,
  };
}

static auto input_modifiers() -> u16 {
  const auto* in = input();
  return in ? static_cast<u16>(in->get_mod_state()) : 0_u16;
}

static auto input_gamepad_button(u32 gamepad, i32 button, OxInputState state) -> bool {
  const auto* in = input();
  if (!in) {
    return false;
  }

  const auto code = static_cast<GamepadButtonCode>(button);
  switch (state) {
    case OxInputState::Pressed : return in->get_gamepad_button_pressed(gamepad, code);
    case OxInputState::Released: return in->get_gamepad_button_released(gamepad, code);
    case OxInputState::Held    : return in->get_gamepad_button_held(gamepad, code);
  }

  return false;
}

static auto input_gamepad_axis(u32 gamepad, i32 axis) -> f32 {
  const auto* in = input();
  if (!in) {
    return 0.0f;
  }

  // Input keeps SDL's raw -32768 to 32767
  const auto raw = in->get_gamepad_axis(gamepad, static_cast<GamepadAxisCode>(axis));
  return std::clamp(raw / static_cast<f32>(SDL_JOYSTICK_AXIS_MAX), -1.0f, 1.0f);
}

static auto input_gamepads(u32* gamepads, i32 capacity) -> i32 {
  auto count = 0;
  auto* ids = SDL_GetGamepads(&count);
  for (i32 i = 0; i < std::min(count, capacity); i++) {
    gamepads[i] = ids[i];
  }

  SDL_free(ids);
  return count;
}

static auto input_action(OxStringView action, u32 gamepad, OxInputState state) -> bool {
  const auto* in = input();
  if (!in) {
    return false;
  }

  switch (state) {
    case OxInputState::Pressed : return in->get_action_pressed(to_view(action), gamepad);
    case OxInputState::Released: return in->get_action_released(to_view(action), gamepad);
    case OxInputState::Held    : return in->get_action_held(to_view(action), gamepad);
  }

  return false;
}

static auto input_action_axis(OxStringView action, u32 gamepad, f32* axis) -> void {
  const auto* in = input();
  const auto value = in ? in->get_action_axis(to_view(action), gamepad) : glm::vec2(0.0f);
  axis[0] = value.x;
  axis[1] = value.y;
}

static auto input_bind_action(OxStringView action, OxStringView context, const OxInputCode* inputs, i32 input_count)
  -> OxStatus {
  ZoneScoped;

  auto* in = input();
  if (!in) {
    return OxStatus::NotFound;
  }

  auto binding = ActionBinding{.action_id = std::string(to_view(action))};
  if (context.length) {
    binding.context = std::string(to_view(context));
  }

  for (const auto& code : std::span(inputs, static_cast<usize>(std::max(input_count, 0)))) {
    auto input_code = to_input_code(code);
    if (!input_code) {
      return OxStatus::InvalidArgument;
    }

    binding.primary_inputs.push_back(*input_code);
  }

  return in->bind_action(std::move(binding)) ? OxStatus::Ok : OxStatus::InvalidArgument;
}

static auto input_unbind_action(OxStringView action) -> OxStatus {
  auto* in = input();
  if (!in) {
    return OxStatus::NotFound;
  }

  return in->unbind_action(std::string(to_view(action))) ? OxStatus::Ok : OxStatus::NotFound;
}

static auto input_context(OxInputContextOp op, OxStringView context) -> void {
  auto* in = input();
  if (!in) {
    return;
  }

  switch (op) {
    case OxInputContextOp::Set : in->set_context(to_view(context)); break;
    case OxInputContextOp::Push: in->push_context(to_view(context)); break;
    case OxInputContextOp::Pop : in->pop_context(); break;
  }
}

static auto input_warp_mouse(f32 x, f32 y) -> void {
  auto* in = input();
  if (in && App::has_window()) {
    in->set_mouse_position_window(App::get_window(), {x, y});
  }
}

static auto input_set_relative_mouse(bool is_enabled) -> void {
  auto* in = input();
  if (in && App::has_window()) {
    in->set_relative_mouse_mode_window(App::get_window(), is_enabled);
  }
}

auto fill_input_api(OxNativeApi& api) -> void {
  api.input_key = input_key;
  api.input_mouse_button = input_mouse_button;
  api.input_mouse = input_mouse;
  api.input_modifiers = input_modifiers;
  api.input_gamepad_button = input_gamepad_button;
  api.input_gamepad_axis = input_gamepad_axis;
  api.input_gamepads = input_gamepads;
  api.input_action = input_action;
  api.input_action_axis = input_action_axis;
  api.input_bind_action = input_bind_action;
  api.input_unbind_action = input_unbind_action;
  api.input_context = input_context;
  api.input_warp_mouse = input_warp_mouse;
  api.input_set_relative_mouse = input_set_relative_mouse;
}
} // namespace ox
