#include "LuaInputBindings.hpp"

#include <sol/state.hpp>
#include <sol/string_view.hpp>

#include "Core/Input.hpp"
#include "Core/Keycodes.hpp"
#include "LuaHelpers.hpp"

namespace ox::LuaBindings {
void bind_input(const std::shared_ptr<sol::state>& state) {
  ZoneScoped;
  auto input = state->create_table("Input");

  SET_TYPE_FUNCTION(input, Input, get_key_pressed);
  SET_TYPE_FUNCTION(input, Input, get_key_held);
  SET_TYPE_FUNCTION(input, Input, get_mouse_clicked);
  SET_TYPE_FUNCTION(input, Input, get_mouse_held);
  SET_TYPE_FUNCTION(input, Input, get_mouse_scroll_offset_y);
  SET_TYPE_FUNCTION(input, Input, get_mouse_position);
  SET_TYPE_FUNCTION(input, Input, set_mouse_position);

  const std::initializer_list<std::pair<sol::string_view, Input::CursorState>> cursor_states = {
      {"Disabled", Input::CursorState::Disabled},
      {"Normal", Input::CursorState::Normal},
      {"Hidden", Input::CursorState::Hidden}};
  state->new_enum<Input::CursorState, true>("CursorState", cursor_states);

  // TODO: controller support
  // input.set_function("get_controller_axis", [](int id, int axis) -> float { return Input::get_controller_axis(id,
  // axis); }); input.set_function("get_controller_name", [](int id) -> std::string { return
  // Input::get_controller_name(id); }); input.set_function("get_controller_hat", [](int id, int hat) -> int { return
  // Input::get_controller_hat(id, hat); }); input.set_function("is_controller_button_pressed", [](int id, int button)
  // -> bool { return Input::is_controller_button_pressed(id, button); });

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
      {"Equal", KeyCode::Equal},
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
      {"D0", KeyCode::D0},
      {"D1", KeyCode::D1},
      {"D2", KeyCode::D2},
      {"D3", KeyCode::D3},
      {"D4", KeyCode::D4},
      {"D5", KeyCode::D5},
      {"D6", KeyCode::D6},
      {"D7", KeyCode::D7},
      {"D8", KeyCode::D8},
      {"D9", KeyCode::D9},
      {"Period", KeyCode::Period},
      {"Divide", KeyCode::Slash},
      {"KPMultiply", KeyCode::KPMultiply},
      {"Minus", KeyCode::Minus},
      {"KPAdd", KeyCode::KPAdd},
      {"KPEqual", KeyCode::KPEqual},
  };
  state->new_enum<KeyCode, true>("Key", key_items);

  const std::initializer_list<std::pair<sol::string_view, MouseCode>> mouse_items = {
      {"Left", MouseCode::ButtonLeft},
      {"Right", MouseCode::ButtonRight},
      {"Middle", MouseCode::ButtonMiddle},
  };
  state->new_enum<MouseCode, true>("MouseButton", mouse_items);
}
} // namespace ox::LuaBindings
