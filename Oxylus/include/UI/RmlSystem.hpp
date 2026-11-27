#pragma once

#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/SystemInterface.h>
#include <SDL3/SDL.h>
#include <array>

#include "Core/Types.hpp"
#include "Utils/Log.hpp"

namespace ox {
class RmlSystem : public Rml::SystemInterface {
public:
  ~RmlSystem() override;

  auto GetElapsedTime() -> double override;

  auto LogMessage(Rml::Log::Type type, const Rml::String& message) -> bool override;

  auto SetMouseCursor(const Rml::String& cursor_name) -> void override;

  auto SetClipboardText(const Rml::String& text) -> void override;

  auto GetClipboardText(Rml::String& text) -> void override;

  static auto convert_key(u32 sdlkey) -> Rml::Input::KeyIdentifier;
  static auto convert_mod(i16 sdl_mods) -> int;

private:
  // made on first use, SDL makes none before it is up and this is constructed earlier
  std::array<SDL_Cursor*, SDL_SYSTEM_CURSOR_COUNT> cursors = {};
};

} // namespace ox
