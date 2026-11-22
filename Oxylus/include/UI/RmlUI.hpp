#pragma once

#include <RmlUi/Core/Types.h>
#include <ankerl/svector.h>
#include <expected>
#include <glm/vec2.hpp>
#include <span>
#include <string>
#include <string_view>

#include "UI/RmlFileInterface.hpp"
#include "UI/RmlSystem.hpp"

namespace ox {
class AssetManager;
class Renderer;
class Input;
class RmlView;

// a focused text field takes every key, including the letters it lets propagate
auto rml_wants_keyboard(Rml::Context* context) -> bool;

// edits to these files, named as the documents were loaded: a document loaded from one is loaded again in its place,
// and an edited style sheet restyles every other document in place, keeping its elements and what scripts hold of them
auto reload_rml_documents(Rml::Context* context, std::span<const std::string> paths) -> void;

// Owns everything RmlUi keeps process wide: the library lifecycle, the system interface, the
// debugger, and the routing of window input to whichever view is under the cursor. Per context state
// lives in RmlView.
class RmlUI {
public:
  constexpr static auto MODULE_NAME = "RmlUI";
  using module_dependencies = std::tuple<AssetManager, Input, Renderer>;

  auto init(this RmlUI& self) -> std::expected<void, std::string>;
  auto deinit(this RmlUI& self) -> std::expected<void, std::string>;

  // Called by RmlView's constructor and destructor. Views must outlive their registration.
  auto register_view(this RmlUI& self, RmlView* view) -> void;
  auto unregister_view(this RmlUI& self, RmlView* view) -> void;

  // Mouse follows the cursor, keyboard follows the focused view. The bool ones return whether the UI
  // took the event, in which case the game shouldn't see it
  auto process_key(this RmlUI& self, u32 key_code, u16 mods, bool down) -> bool;
  auto process_text(this RmlUI& self, std::string_view text) -> void;
  auto process_mouse_move(this RmlUI& self, glm::vec2 position) -> void;
  auto process_mouse_button(this RmlUI& self, u8 button, bool down) -> bool;
  // SDL's wheel, x to the right and y away from the user
  auto process_mouse_scroll(this RmlUI& self, glm::vec2 offset) -> bool;

  // `reload_rml_documents` for every view, `paths` are virtual
  auto reload_ui_files(this RmlUI& self, std::span<const std::string> paths) -> void;

private:
  auto keyboard_view(this RmlUI& self) -> RmlView*;
  // Resolves which view owns `position`, sends the leave and the move, and returns it.
  auto update_hover(this RmlUI& self, glm::vec2 position) -> RmlView*;

  RmlSystem rml_system = {};
  RmlFileInterface rml_files = {};
  ankerl::svector<RmlView*, 4> views = {};

  RmlView* hovered_view = nullptr;
  // While a button is held the press target keeps the mouse, so dragging past the view's edge still
  // delivers the release.
  RmlView* capture_view = nullptr;
  glm::vec2 last_mouse_position = {};
  u8 held_buttons = 0;
  // pressed on an element, so the release is the UI's too
  u8 ui_buttons = 0;

  Rml::Context* debugger_host_context = nullptr;
};
} // namespace ox
