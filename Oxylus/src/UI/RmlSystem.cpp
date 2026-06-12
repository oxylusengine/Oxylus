#include "UI/RmlSystem.hpp"

namespace ox {
RmlSystem::RmlSystem() {
  cursor_map_["arrow"] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
  cursor_map_["text"] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT);
  cursor_map_["pointer"] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
  cursor_map_["cross"] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR);
  cursor_map_["size-nwse"] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NWSE_RESIZE);
  cursor_map_["size-nesw"] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NESW_RESIZE);
  cursor_map_["size-ew"] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_EW_RESIZE);
  cursor_map_["size-ns"] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NS_RESIZE);
  cursor_map_["move"] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_MOVE);
  cursor_map_["unavailable"] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NOT_ALLOWED);
}

RmlSystem::~RmlSystem() {
  for (auto& [name, cursor] : cursor_map_) {
    if (cursor) {
      SDL_DestroyCursor(cursor);
    }
  }
}

auto RmlSystem::GetElapsedTime() -> double {
  // SDL3 returns nanoseconds, we need to return seconds for RmlUi animations
  return static_cast<double>(SDL_GetTicksNS()) / 1'000'000'000.0;
}

auto RmlSystem::LogMessage(Rml::Log::Type type, const Rml::String& message) -> bool {
  switch (type) {
    case Rml::Log::LT_ERROR:
    case Rml::Log::LT_ASSERT : OX_LOG_ERROR("RmlUi: {}", message); break;
    case Rml::Log::LT_WARNING: OX_LOG_WARN("RmlUi: {}", message); break;
    case Rml::Log::LT_INFO   : OX_LOG_INFO("RmlUi: {}", message); break;
    case Rml::Log::LT_DEBUG  : OX_LOG_TRACE("RmlUi: {}", message); break;
    default                  : break;
  }
  return true;
}

auto RmlSystem::SetMouseCursor(const Rml::String& cursor_name) -> void {
  if (cursor_name.empty() || cursor_name == "auto") {
    SDL_SetCursor(SDL_GetDefaultCursor());
    return;
  }

  auto it = cursor_map_.find(cursor_name);
  if (it != cursor_map_.end() && it->second) {
    SDL_SetCursor(it->second);
  } else {
    SDL_SetCursor(SDL_GetDefaultCursor());
  }
}

auto RmlSystem::SetClipboardText(const Rml::String& text) -> void { SDL_SetClipboardText(text.c_str()); }

auto RmlSystem::GetClipboardText(Rml::String& text) -> void {
  if (SDL_HasClipboardText()) {
    char* clipboard_data = SDL_GetClipboardText();
    if (clipboard_data) {
      text = clipboard_data;
      SDL_free(clipboard_data);
    }
  }
}

auto RmlSystem::convert_key(u32 sdlkey) -> Rml::Input::KeyIdentifier {
  constexpr auto key_a = SDLK_A;
  constexpr auto key_b = SDLK_B;
  constexpr auto key_c = SDLK_C;
  constexpr auto key_d = SDLK_D;
  constexpr auto key_e = SDLK_E;
  constexpr auto key_f = SDLK_F;
  constexpr auto key_g = SDLK_G;
  constexpr auto key_h = SDLK_H;
  constexpr auto key_i = SDLK_I;
  constexpr auto key_j = SDLK_J;
  constexpr auto key_k = SDLK_K;
  constexpr auto key_l = SDLK_L;
  constexpr auto key_m = SDLK_M;
  constexpr auto key_n = SDLK_N;
  constexpr auto key_o = SDLK_O;
  constexpr auto key_p = SDLK_P;
  constexpr auto key_q = SDLK_Q;
  constexpr auto key_r = SDLK_R;
  constexpr auto key_s = SDLK_S;
  constexpr auto key_t = SDLK_T;
  constexpr auto key_u = SDLK_U;
  constexpr auto key_v = SDLK_V;
  constexpr auto key_w = SDLK_W;
  constexpr auto key_x = SDLK_X;
  constexpr auto key_y = SDLK_Y;
  constexpr auto key_z = SDLK_Z;
  constexpr auto key_grave = SDLK_GRAVE;
  constexpr auto key_dblapostrophe = SDLK_DBLAPOSTROPHE;

  // clang-format off
	switch (sdlkey)
	{
	case SDLK_UNKNOWN:      return Rml::Input::KI_UNKNOWN;
	case SDLK_ESCAPE:       return Rml::Input::KI_ESCAPE;
	case SDLK_SPACE:        return Rml::Input::KI_SPACE;
	case SDLK_0:            return Rml::Input::KI_0;
	case SDLK_1:            return Rml::Input::KI_1;
	case SDLK_2:            return Rml::Input::KI_2;
	case SDLK_3:            return Rml::Input::KI_3;
	case SDLK_4:            return Rml::Input::KI_4;
	case SDLK_5:            return Rml::Input::KI_5;
	case SDLK_6:            return Rml::Input::KI_6;
	case SDLK_7:            return Rml::Input::KI_7;
	case SDLK_8:            return Rml::Input::KI_8;
	case SDLK_9:            return Rml::Input::KI_9;
	case key_a:             return Rml::Input::KI_A;
	case key_b:             return Rml::Input::KI_B;
	case key_c:             return Rml::Input::KI_C;
	case key_d:             return Rml::Input::KI_D;
	case key_e:             return Rml::Input::KI_E;
	case key_f:             return Rml::Input::KI_F;
	case key_g:             return Rml::Input::KI_G;
	case key_h:             return Rml::Input::KI_H;
	case key_i:             return Rml::Input::KI_I;
	case key_j:             return Rml::Input::KI_J;
	case key_k:             return Rml::Input::KI_K;
	case key_l:             return Rml::Input::KI_L;
	case key_m:             return Rml::Input::KI_M;
	case key_n:             return Rml::Input::KI_N;
	case key_o:             return Rml::Input::KI_O;
	case key_p:             return Rml::Input::KI_P;
	case key_q:             return Rml::Input::KI_Q;
	case key_r:             return Rml::Input::KI_R;
	case key_s:             return Rml::Input::KI_S;
	case key_t:             return Rml::Input::KI_T;
	case key_u:             return Rml::Input::KI_U;
	case key_v:             return Rml::Input::KI_V;
	case key_w:             return Rml::Input::KI_W;
	case key_x:             return Rml::Input::KI_X;
	case key_y:             return Rml::Input::KI_Y;
	case key_z:             return Rml::Input::KI_Z;
	case SDLK_SEMICOLON:    return Rml::Input::KI_OEM_1;
	case SDLK_PLUS:         return Rml::Input::KI_OEM_PLUS;
	case SDLK_COMMA:        return Rml::Input::KI_OEM_COMMA;
	case SDLK_MINUS:        return Rml::Input::KI_OEM_MINUS;
	case SDLK_PERIOD:       return Rml::Input::KI_OEM_PERIOD;
	case SDLK_SLASH:        return Rml::Input::KI_OEM_2;
	case key_grave:         return Rml::Input::KI_OEM_3;
	case SDLK_LEFTBRACKET:  return Rml::Input::KI_OEM_4;
	case SDLK_BACKSLASH:    return Rml::Input::KI_OEM_5;
	case SDLK_RIGHTBRACKET: return Rml::Input::KI_OEM_6;
	case key_dblapostrophe: return Rml::Input::KI_OEM_7;
	case SDLK_KP_0:         return Rml::Input::KI_NUMPAD0;
	case SDLK_KP_1:         return Rml::Input::KI_NUMPAD1;
	case SDLK_KP_2:         return Rml::Input::KI_NUMPAD2;
	case SDLK_KP_3:         return Rml::Input::KI_NUMPAD3;
	case SDLK_KP_4:         return Rml::Input::KI_NUMPAD4;
	case SDLK_KP_5:         return Rml::Input::KI_NUMPAD5;
	case SDLK_KP_6:         return Rml::Input::KI_NUMPAD6;
	case SDLK_KP_7:         return Rml::Input::KI_NUMPAD7;
	case SDLK_KP_8:         return Rml::Input::KI_NUMPAD8;
	case SDLK_KP_9:         return Rml::Input::KI_NUMPAD9;
	case SDLK_KP_ENTER:     return Rml::Input::KI_NUMPADENTER;
	case SDLK_KP_MULTIPLY:  return Rml::Input::KI_MULTIPLY;
	case SDLK_KP_PLUS:      return Rml::Input::KI_ADD;
	case SDLK_KP_MINUS:     return Rml::Input::KI_SUBTRACT;
	case SDLK_KP_PERIOD:    return Rml::Input::KI_DECIMAL;
	case SDLK_KP_DIVIDE:    return Rml::Input::KI_DIVIDE;
	case SDLK_KP_EQUALS:    return Rml::Input::KI_OEM_NEC_EQUAL;
	case SDLK_BACKSPACE:    return Rml::Input::KI_BACK;
	case SDLK_TAB:          return Rml::Input::KI_TAB;
	case SDLK_CLEAR:        return Rml::Input::KI_CLEAR;
	case SDLK_RETURN:       return Rml::Input::KI_RETURN;
	case SDLK_PAUSE:        return Rml::Input::KI_PAUSE;
	case SDLK_CAPSLOCK:     return Rml::Input::KI_CAPITAL;
	case SDLK_PAGEUP:       return Rml::Input::KI_PRIOR;
	case SDLK_PAGEDOWN:     return Rml::Input::KI_NEXT;
	case SDLK_END:          return Rml::Input::KI_END;
	case SDLK_HOME:         return Rml::Input::KI_HOME;
	case SDLK_LEFT:         return Rml::Input::KI_LEFT;
	case SDLK_UP:           return Rml::Input::KI_UP;
	case SDLK_RIGHT:        return Rml::Input::KI_RIGHT;
	case SDLK_DOWN:         return Rml::Input::KI_DOWN;
	case SDLK_INSERT:       return Rml::Input::KI_INSERT;
	case SDLK_DELETE:       return Rml::Input::KI_DELETE;
	case SDLK_HELP:         return Rml::Input::KI_HELP;
	case SDLK_F1:           return Rml::Input::KI_F1;
	case SDLK_F2:           return Rml::Input::KI_F2;
	case SDLK_F3:           return Rml::Input::KI_F3;
	case SDLK_F4:           return Rml::Input::KI_F4;
	case SDLK_F5:           return Rml::Input::KI_F5;
	case SDLK_F6:           return Rml::Input::KI_F6;
	case SDLK_F7:           return Rml::Input::KI_F7;
	case SDLK_F8:           return Rml::Input::KI_F8;
	case SDLK_F9:           return Rml::Input::KI_F9;
	case SDLK_F10:          return Rml::Input::KI_F10;
	case SDLK_F11:          return Rml::Input::KI_F11;
	case SDLK_F12:          return Rml::Input::KI_F12;
	case SDLK_F13:          return Rml::Input::KI_F13;
	case SDLK_F14:          return Rml::Input::KI_F14;
	case SDLK_F15:          return Rml::Input::KI_F15;
	case SDLK_NUMLOCKCLEAR: return Rml::Input::KI_NUMLOCK;
	case SDLK_SCROLLLOCK:   return Rml::Input::KI_SCROLL;
	case SDLK_LSHIFT:       return Rml::Input::KI_LSHIFT;
	case SDLK_RSHIFT:       return Rml::Input::KI_RSHIFT;
	case SDLK_LCTRL:        return Rml::Input::KI_LCONTROL;
	case SDLK_RCTRL:        return Rml::Input::KI_RCONTROL;
	case SDLK_LALT:         return Rml::Input::KI_LMENU;
	case SDLK_RALT:         return Rml::Input::KI_RMENU;
	case SDLK_LGUI:         return Rml::Input::KI_LMETA;
	case SDLK_RGUI:         return Rml::Input::KI_RMETA;
	/*
	case SDLK_LSUPER:       return Rml::Input::KI_LWIN;
	case SDLK_RSUPER:       return Rml::Input::KI_RWIN;
	*/
	default: break;
	}
  // clang-format on

  return Rml::Input::KI_UNKNOWN;
}

auto RmlSystem::convert_mod(i16 sdl_mods) -> int {
  constexpr auto mod_ctrl = SDL_KMOD_CTRL;
  constexpr auto mod_shift = SDL_KMOD_SHIFT;
  constexpr auto mod_alt = SDL_KMOD_ALT;
  constexpr auto mod_num = SDL_KMOD_NUM;
  constexpr auto mod_caps = SDL_KMOD_CAPS;

  int retval = 0;

  if (sdl_mods & mod_ctrl)
    retval |= Rml::Input::KM_CTRL;

  if (sdl_mods & mod_shift)
    retval |= Rml::Input::KM_SHIFT;

  if (sdl_mods & mod_alt)
    retval |= Rml::Input::KM_ALT;

  if (sdl_mods & mod_num)
    retval |= Rml::Input::KM_NUMLOCK;

  if (sdl_mods & mod_caps)
    retval |= Rml::Input::KM_CAPSLOCK;

  return retval;
}

} // namespace ox
