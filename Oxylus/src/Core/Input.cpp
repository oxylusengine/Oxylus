#include "Input.hpp"

#include "App.hpp"

namespace ox {
Input* Input::_instance = nullptr;

auto Input::init() -> std::expected<void, std::string> { return {}; }

auto Input::deinit() -> std::expected<void, std::string> { return {}; }

void Input::set_instance() {
  if (_instance == nullptr)
    _instance = App::get_system<Input>(EngineSystems::Input);
}

void Input::reset_pressed() {
  ZoneScoped;

  memset(input_data.key_pressed, 0, MAX_KEYS);
  memset(input_data.mouse_clicked, 0, MAX_BUTTONS);
  input_data.scroll_offset_y = 0;
  input_data.mouse_moved = false;
}

void Input::reset() {
  memset(input_data.key_held, 0, MAX_KEYS);
  memset(input_data.key_pressed, 0, MAX_KEYS);
  memset(input_data.mouse_clicked, 0, MAX_BUTTONS);
  memset(input_data.mouse_held, 0, MAX_BUTTONS);

  input_data.scroll_offset_y = 0;
  input_data.mouse_moved = false;
}

glm::vec2 Input::get_mouse_position() { return _instance->input_data.mouse_pos; }

glm::vec2 Input::get_mouse_position_rel() { return _instance->input_data.mouse_pos_rel; }

float Input::get_mouse_offset_x() { return _instance->input_data.mouse_offset_x; }

float Input::get_mouse_offset_y() { return _instance->input_data.mouse_offset_y; }

float Input::get_mouse_scroll_offset_y() { return _instance->input_data.scroll_offset_y; }

bool Input::get_mouse_moved() { return _instance->input_data.mouse_moved; }

void Input::set_mouse_position(const float x, const float y) { SDL_WarpMouseGlobal(x, y); }

KeyCode Input::to_keycode(SDL_Keycode keycode, SDL_Scancode scancode) {
  switch (scancode) {
    case SDL_SCANCODE_KP_0       : return KeyCode::KP0;
    case SDL_SCANCODE_KP_1       : return KeyCode::KP1;
    case SDL_SCANCODE_KP_2       : return KeyCode::KP2;
    case SDL_SCANCODE_KP_3       : return KeyCode::KP3;
    case SDL_SCANCODE_KP_4       : return KeyCode::KP4;
    case SDL_SCANCODE_KP_5       : return KeyCode::KP5;
    case SDL_SCANCODE_KP_6       : return KeyCode::KP6;
    case SDL_SCANCODE_KP_7       : return KeyCode::KP7;
    case SDL_SCANCODE_KP_8       : return KeyCode::KP8;
    case SDL_SCANCODE_KP_9       : return KeyCode::KP9;
    case SDL_SCANCODE_KP_PERIOD  : return KeyCode::KPDecimal;
    case SDL_SCANCODE_KP_DIVIDE  : return KeyCode::KPDivide;
    case SDL_SCANCODE_KP_MULTIPLY: return KeyCode::KPMultiply;
    case SDL_SCANCODE_KP_MINUS   : return KeyCode::KPSubtract;
    case SDL_SCANCODE_KP_PLUS    : return KeyCode::KPAdd;
    case SDL_SCANCODE_KP_ENTER   : return KeyCode::KPEnter;
    case SDL_SCANCODE_KP_EQUALS  : return KeyCode::KPEqual;
    default                      : break;
  }

  switch (keycode) {
    case SDLK_TAB         : return KeyCode::Tab;
    case SDLK_LEFT        : return KeyCode::Left;
    case SDLK_RIGHT       : return KeyCode::Right;
    case SDLK_UP          : return KeyCode::Up;
    case SDLK_DOWN        : return KeyCode::Down;
    case SDLK_PAGEUP      : return KeyCode::PageUp;
    case SDLK_PAGEDOWN    : return KeyCode::PageDown;
    case SDLK_HOME        : return KeyCode::Home;
    case SDLK_END         : return KeyCode::End;
    case SDLK_INSERT      : return KeyCode::Insert;
    case SDLK_DELETE      : return KeyCode::Delete;
    case SDLK_BACKSPACE   : return KeyCode::Backspace;
    case SDLK_SPACE       : return KeyCode::Space;
    case SDLK_RETURN      : return KeyCode::Return;
    case SDLK_ESCAPE      : return KeyCode::Escape;
    case SDLK_APOSTROPHE  : return KeyCode::Apostrophe;
    case SDLK_COMMA       : return KeyCode::Comma;
    case SDLK_MINUS       : return KeyCode::Minus;
    case SDLK_PERIOD      : return KeyCode::Period;
    case SDLK_SLASH       : return KeyCode::Slash;
    case SDLK_SEMICOLON   : return KeyCode::Semicolon;
    case SDLK_EQUALS      : return KeyCode::Equal;
    case SDLK_LEFTBRACKET : return KeyCode::LeftBracket;
    case SDLK_BACKSLASH   : return KeyCode::Backslash;
    case SDLK_RIGHTBRACKET: return KeyCode::RightBracket;
    case SDLK_GRAVE       : return KeyCode::GraveAccent;
    case SDLK_CAPSLOCK    : return KeyCode::CapsLock;
    case SDLK_SCROLLLOCK  : return KeyCode::ScrollLock;
    case SDLK_NUMLOCKCLEAR: return KeyCode::NumLock;
    case SDLK_PRINTSCREEN : return KeyCode::PrintScreen;
    case SDLK_PAUSE       : return KeyCode::Pause;

    case SDLK_LCTRL       : return KeyCode::LeftControl;
    case SDLK_LSHIFT      : return KeyCode::LeftShift;
    case SDLK_LALT        : return KeyCode::LeftAlt;
    case SDLK_LGUI        : return KeyCode::LeftSuper;
    case SDLK_RCTRL       : return KeyCode::RightControl;
    case SDLK_RSHIFT      : return KeyCode::RightShift;
    case SDLK_RALT        : return KeyCode::RightAlt;
    case SDLK_RGUI        : return KeyCode::RightSuper;
    case SDLK_APPLICATION : return KeyCode::Menu;

    case SDLK_0           : return KeyCode::D0;
    case SDLK_1           : return KeyCode::D1;
    case SDLK_2           : return KeyCode::D2;
    case SDLK_3           : return KeyCode::D3;
    case SDLK_4           : return KeyCode::D4;
    case SDLK_5           : return KeyCode::D5;
    case SDLK_6           : return KeyCode::D6;
    case SDLK_7           : return KeyCode::D7;
    case SDLK_8           : return KeyCode::D8;
    case SDLK_9           : return KeyCode::D9;

    case SDLK_A           : return KeyCode::A;
    case SDLK_B           : return KeyCode::B;
    case SDLK_C           : return KeyCode::C;
    case SDLK_D           : return KeyCode::D;
    case SDLK_E           : return KeyCode::E;
    case SDLK_F           : return KeyCode::F;
    case SDLK_G           : return KeyCode::G;
    case SDLK_H           : return KeyCode::H;
    case SDLK_I           : return KeyCode::I;
    case SDLK_J           : return KeyCode::J;
    case SDLK_K           : return KeyCode::K;
    case SDLK_L           : return KeyCode::L;
    case SDLK_M           : return KeyCode::M;
    case SDLK_N           : return KeyCode::N;
    case SDLK_O           : return KeyCode::O;
    case SDLK_P           : return KeyCode::P;
    case SDLK_Q           : return KeyCode::Q;
    case SDLK_R           : return KeyCode::R;
    case SDLK_S           : return KeyCode::S;
    case SDLK_T           : return KeyCode::T;
    case SDLK_U           : return KeyCode::U;
    case SDLK_V           : return KeyCode::V;
    case SDLK_W           : return KeyCode::W;
    case SDLK_X           : return KeyCode::X;
    case SDLK_Y           : return KeyCode::Y;
    case SDLK_Z           : return KeyCode::Z;

    case SDLK_F1          : return KeyCode::F1;
    case SDLK_F2          : return KeyCode::F2;
    case SDLK_F3          : return KeyCode::F3;
    case SDLK_F4          : return KeyCode::F4;
    case SDLK_F5          : return KeyCode::F5;
    case SDLK_F6          : return KeyCode::F6;
    case SDLK_F7          : return KeyCode::F7;
    case SDLK_F8          : return KeyCode::F8;
    case SDLK_F9          : return KeyCode::F9;
    case SDLK_F10         : return KeyCode::F10;
    case SDLK_F11         : return KeyCode::F11;
    case SDLK_F12         : return KeyCode::F12;
    case SDLK_F13         : return KeyCode::F13;
    case SDLK_F14         : return KeyCode::F14;
    case SDLK_F15         : return KeyCode::F15;
    case SDLK_F16         : return KeyCode::F16;
    case SDLK_F17         : return KeyCode::F17;
    case SDLK_F18         : return KeyCode::F18;
    case SDLK_F19         : return KeyCode::F19;
    case SDLK_F20         : return KeyCode::F20;
    case SDLK_F21         : return KeyCode::F21;
    case SDLK_F22         : return KeyCode::F22;
    case SDLK_F23         : return KeyCode::F23;
    case SDLK_F24         : return KeyCode::F24;

    default               : break;
  }
  return KeyCode::None;
}

MouseCode Input::to_mouse_code(SDL_MouseButtonFlags key) {
  switch (key) {
    case SDL_BUTTON_LEFT  : return MouseCode::ButtonLeft;
    case SDL_BUTTON_RIGHT : return MouseCode::ButtonRight;
    case SDL_BUTTON_MIDDLE: return MouseCode::ButtonMiddle;
    case SDL_BUTTON_X1    : return MouseCode::ButtonForward;
    case SDL_BUTTON_X2    : return MouseCode::ButtonBackward;
    default               : break;
  }

  return MouseCode::None;
}

} // namespace ox
