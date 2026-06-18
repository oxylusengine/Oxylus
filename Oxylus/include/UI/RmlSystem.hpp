#pragma once

#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/SystemInterface.h>
#include <SDL3/SDL.h>
#include <unordered_map>

#include "Core/Types.hpp"
#include "Utils/Log.hpp"

namespace ox {
class RmlSystem : public Rml::SystemInterface {
public:
  RmlSystem();
  ~RmlSystem() override;

  auto GetElapsedTime() -> double override;

  auto LogMessage(Rml::Log::Type type, const Rml::String& message) -> bool override;

  auto SetMouseCursor(const Rml::String& cursor_name) -> void override;

  auto SetClipboardText(const Rml::String& text) -> void override;

  auto GetClipboardText(Rml::String& text) -> void override;

  static auto convert_key(u32 sdlkey) -> Rml::Input::KeyIdentifier;
  static auto convert_mod(i16 sdl_mods) -> int;

private:
  std::unordered_map<Rml::String, SDL_Cursor*> cursor_map_;
};

} // namespace ox
