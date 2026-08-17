#pragma once

#include <RmlUi/Core/Types.h>
#include <ankerl/svector.h>
#include <expected>
#include <glm/vec2.hpp>
#include <string>
#include <string_view>

#include "UI/RmlSystem.hpp"

namespace ox {
class Renderer;
class Input;
class RmlView;

// Owns everything RmlUi keeps process wide: the library lifecycle, the system interface, the
// debugger, and the routing of window input to whichever view is under the cursor. Per context state
// lives in RmlView.
class RmlUI {
public:
  constexpr static auto MODULE_NAME = "RmlUI";
  using module_dependencies = std::tuple<Input, Renderer>;

  auto init(this RmlUI& self) -> std::expected<void, std::string>;
  auto deinit(this RmlUI& self) -> std::expected<void, std::string>;

  // Called by RmlView's constructor and destructor. Views must outlive their registration.
  auto register_view(this RmlUI& self, RmlView* view) -> void;
  auto unregister_view(this RmlUI& self, RmlView* view) -> void;

  // Mouse follows the cursor, keyboard follows the focused view.
  auto process_key(this RmlUI& self, u32 key_code, u16 mods, bool down) -> void;
  auto process_text(this RmlUI& self, std::string_view text) -> void;
  auto process_mouse_move(this RmlUI& self, glm::vec2 position) -> void;
  auto process_mouse_button(this RmlUI& self, u8 button, bool down) -> void;
  auto process_mouse_scroll(this RmlUI& self, f32 offset) -> void;

private:
  auto keyboard_view(this RmlUI& self) -> RmlView*;
  // Resolves which view owns `position`, sends the leave and the move, and returns it.
  auto update_hover(this RmlUI& self, glm::vec2 position) -> RmlView*;

  RmlSystem rml_system = {};
  ankerl::svector<RmlView*, 4> views = {};

  RmlView* hovered_view = nullptr;
  // While a button is held the press target keeps the mouse, so dragging past the view's edge still
  // delivers the release.
  RmlView* capture_view = nullptr;
  glm::vec2 last_mouse_position = {};
  u8 held_buttons = 0;

  Rml::Context* debugger_host_context = nullptr;
};
} // namespace ox
