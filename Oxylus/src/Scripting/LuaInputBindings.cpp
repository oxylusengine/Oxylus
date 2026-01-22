#include "Scripting/LuaInputBindings.hpp"

#include <sol/state.hpp>
#include <sol/string_view.hpp>

#include "Core/Input.hpp"
#include "Core/Keycodes.hpp"
#include "Scripting/LuaHelpers.hpp"

namespace ox {
auto InputBinding::bind(sol::state* state) -> void {
  ZoneScoped;
  auto input = state->new_usertype<Input>("Input");

  SET_TYPE_FUNCTION(input, Input, set_context);
  SET_TYPE_FUNCTION(input, Input, push_context);
  SET_TYPE_FUNCTION(input, Input, pop_context);
  SET_TYPE_FUNCTION(input, Input, get_active_context);

  SET_TYPE_FUNCTION(input, Input, get_action_pressed);
  SET_TYPE_FUNCTION(input, Input, get_action_released);
  SET_TYPE_FUNCTION(input, Input, get_action_held);
  SET_TYPE_FUNCTION(input, Input, get_action_axis);

  SET_TYPE_FUNCTION(input, Input, get_key_pressed);
  SET_TYPE_FUNCTION(input, Input, get_key_released);
  SET_TYPE_FUNCTION(input, Input, get_key_held);

  SET_TYPE_FUNCTION(input, Input, get_mouse_clicked);
  SET_TYPE_FUNCTION(input, Input, get_mouse_held);
  SET_TYPE_FUNCTION(input, Input, get_mouse_scroll_offset_y);
  SET_TYPE_FUNCTION(input, Input, get_mouse_position);
  SET_TYPE_FUNCTION(input, Input, set_mouse_position_global);

  SET_TYPE_FUNCTION(input, Input, get_gamepad_button_pressed);
  SET_TYPE_FUNCTION(input, Input, get_gamepad_button_released);
  SET_TYPE_FUNCTION(input, Input, get_gamepad_button_held);
  SET_TYPE_FUNCTION(input, Input, get_gamepad_axis);

  const std::initializer_list<std::pair<sol::string_view, Input::CursorState>> cursor_states = {
    {"Disabled", Input::CursorState::Disabled},
    {"Normal", Input::CursorState::Normal},
    {"Hidden", Input::CursorState::Hidden}
  };
  state->new_enum<Input::CursorState, true>("CursorState", cursor_states);

  const std::initializer_list<std::pair<sol::string_view, KeyCode>> key_items = {
    {"A", KeyCode::A},
    {"B", KeyCode::B},
    {"C", KeyCode::C},
    {"D", KeyCode::D},
    {"E", KeyCode::E},
    {"F", KeyCode::F},
    {"H", KeyCode::G},
    {"G", KeyCode::H},
    {"I", KeyCode::I},
    {"J", KeyCode::J},
    {"K", KeyCode::K},
    {"L", KeyCode::L},
    {"M", KeyCode::M},
    {"N", KeyCode::N},
    {"O", KeyCode::O},
    {"P", KeyCode::P},
    {"Q", KeyCode::Q},
    {"R", KeyCode::R},
    {"S", KeyCode::S},
    {"T", KeyCode::T},
    {"U", KeyCode::U},
    {"V", KeyCode::V},
    {"W", KeyCode::W},
    {"X", KeyCode::X},
    {"Y", KeyCode::Y},
    {"Z", KeyCode::Z},
    {"Space", KeyCode::Space},
    {"Escape", KeyCode::Escape},
    {"Apostrophe", KeyCode::Apostrophe},
    {"Comma", KeyCode::Comma},
    {"Minus", KeyCode::Minus},
    {"Period", KeyCode::Period},
    {"Slash", KeyCode::Slash},
    {"Semicolon", KeyCode::Semicolon},
    {"Equals", KeyCode::Equals},
    {"LeftBracket", KeyCode::LeftBracket},
    {"Backslash", KeyCode::Backslash},
    {"RightBracket", KeyCode::RightBracket},
    {"Return", KeyCode::Return},
    {"Tab", KeyCode::Tab},
    {"Backspace", KeyCode::Backspace},
    {"Insert", KeyCode::Insert},
    {"Delete", KeyCode::Delete},
    {"Right", KeyCode::Right},
    {"Left", KeyCode::Left},
    {"Down", KeyCode::Down},
    {"Up", KeyCode::Up},
    {"PageUp", KeyCode::PageUp},
    {"PageDown", KeyCode::PageDown},
    {"Home", KeyCode::Home},
    {"End", KeyCode::End},
    {"CapsLock", KeyCode::CapsLock},
    {"ScrollLock", KeyCode::ScrollLock},
    {"NumLock", KeyCode::NumLock},
    {"PrintScreen", KeyCode::PrintScreen},
    {"Pasue", KeyCode::Pause},
    {"LeftShift", KeyCode::LeftShift},
    {"LeftControl", KeyCode::LeftControl},
    {"LeftAlt", KeyCode::LeftAlt},
    {"LeftSuper", KeyCode::LeftSuper},
    {"RightShift", KeyCode::RightShift},
    {"RightControl", KeyCode::RightControl},
    {"RightAlt", KeyCode::RightAlt},
    {"RightSuper", KeyCode::RightSuper},
    {"Menu", KeyCode::Menu},
    {"F1", KeyCode::F1},
    {"F2", KeyCode::F2},
    {"F3", KeyCode::F3},
    {"F4", KeyCode::F4},
    {"F5", KeyCode::F5},
    {"F6", KeyCode::F6},
    {"F7", KeyCode::F7},
    {"F8", KeyCode::F8},
    {"F9", KeyCode::F9},
    {"F10", KeyCode::F10},
    {"F11", KeyCode::F11},
    {"F12", KeyCode::F12},
    {"Period", KeyCode::Period},
    {"Divide", KeyCode::Slash},
  };
  state->new_enum<KeyCode, true>("KeyCode", key_items);

  const std::initializer_list<std::pair<sol::string_view, ModCode>> mod_codes = {
    {"None", ModCode::None},
    {"LeftShift", ModCode::LeftShift},
    {"RightShift", ModCode::RightShift},
    {"Level5", ModCode::Level5},
    {"LeftControl", ModCode::LeftControl},
    {"RightControl", ModCode::RightControl},
    {"LeftAlt", ModCode::LeftAlt},
    {"RightAlt", ModCode::RightAlt},
    {"LeftSuper", ModCode::LeftSuper},
    {"RightSuper", ModCode::RightSuper},
    {"NumLock", ModCode::NumLock},
    {"CapsLock", ModCode::CapsLock},
    {"AltGr", ModCode::AltGr},
    {"ScrollLock", ModCode::ScrollLock},
    {"AnyControl", ModCode::AnyControl},
    {"AnyShift", ModCode::AnyShift},
    {"AnyAlt", ModCode::AnyAlt},
    {"AnySuper", ModCode::AnySuper}
  };
  state->new_enum<ModCode, true>("ModCode", mod_codes);

  const std::initializer_list<std::pair<sol::string_view, MouseCode>> mouse_items = {
    {"Left", MouseCode::Left},
    {"Right", MouseCode::Right},
    {"Middle", MouseCode::Middle},
  };
  state->new_enum<MouseCode, true>("MouseButton", mouse_items);

  const std::initializer_list<std::pair<sol::string_view, MouseAxisCode>> mouse_axis_codes = {
    {"None", MouseAxisCode::None},
    {"AxisX", MouseAxisCode::AxisX},
    {"AxisY", MouseAxisCode::AxisY},
    {"AxisXY", MouseAxisCode::AxisXY},
  };
  state->new_enum<MouseAxisCode, true>("MouseAxisCode", mouse_axis_codes);

  const std::initializer_list<std::pair<sol::string_view, GamepadButtonCode>> gamepad_button_codes = {
    {"None", GamepadButtonCode::None},
    {"South", GamepadButtonCode::South},
    {"East", GamepadButtonCode::East},
    {"West", GamepadButtonCode::West},
    {"North", GamepadButtonCode::North},
    {"Back", GamepadButtonCode::Back},
    {"Guide", GamepadButtonCode::Guide},
    {"Start", GamepadButtonCode::Start},
    {"LeftStick", GamepadButtonCode::LeftStick},
    {"RightStick", GamepadButtonCode::RightStick},
    {"LeftShoulder", GamepadButtonCode::LeftShoulder},
    {"RightShoulder", GamepadButtonCode::RightShoulder},
    {"DPadUp", GamepadButtonCode::DPadUp},
    {"DPadDown", GamepadButtonCode::DPadDown},
    {"DPadLeft", GamepadButtonCode::DPadLeft},
    {"DPadRight", GamepadButtonCode::DPadRight},
    {"Misc1", GamepadButtonCode::Misc1},
    {"RightPaddle1", GamepadButtonCode::RightPaddle1},
    {"LeftPaddle1", GamepadButtonCode::LeftPaddle1},
    {"RightPaddle2", GamepadButtonCode::RightPaddle2},
    {"LeftPaddle2", GamepadButtonCode::LeftPaddle2},
    {"Touchpad", GamepadButtonCode::Touchpad},
    {"Misc2", GamepadButtonCode::Misc2},
    {"Misc3", GamepadButtonCode::Misc3},
    {"Misc4", GamepadButtonCode::Misc4},
    {"Misc5", GamepadButtonCode::Misc5},
    {"Misc6", GamepadButtonCode::Misc6},
  };
  state->new_enum<GamepadButtonCode, true>("GamepadButtonCode", gamepad_button_codes);

  const std::initializer_list<std::pair<sol::string_view, GamepadAxisCode>> gamepad_axis_code = {
    {"None", GamepadAxisCode::None},
    {"AxisLeftX", GamepadAxisCode::AxisLeftX},
    {"AxisLeftY", GamepadAxisCode::AxisLeftY},
    {"AxisRightX", GamepadAxisCode::AxisRightX},
    {"AxisRightY", GamepadAxisCode::AxisRightY},
    {"AxisLeftTrigger", GamepadAxisCode::AxisLeftTrigger},
    {"AxisRightTrigger", GamepadAxisCode::AxisRightTrigger},
  };
  state->new_enum<GamepadAxisCode, true>("GamepadAxisCode", gamepad_axis_code);
}
} // namespace ox
