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
class Timestep;
enum class GamepadButtonCode : i16;
enum class GamepadAxisCode : i16;

// a focused text field takes every key, including the letters it lets propagate
auto rml_wants_keyboard(Rml::Context* context) -> bool;

// focus is where RmlUi navigates from: on something tabbed to, or in what sets `nav`. a HUD sets neither and leaves
// the gamepad to the game, a menu takes it with `body { nav: auto; }` or an autofocus
auto rml_wants_gamepad(Rml::Context* context) -> bool;

// edits to these files, named as the documents were loaded: a document loaded from one is loaded again in its place,
// and an edited style sheet restyles every other document in place, keeping its elements and what scripts hold of them
auto reload_rml_documents(Rml::Context* context, std::span<const std::string> paths) -> void;

enum class RmlNavDirection : u8 { Up, Down, Left, Right, None };

struct RmlGamepad {
  u32 instance_id = 0;
  // -1 to 1, y down
  glm::vec2 left_stick = {};
  glm::vec2 right_stick = {};
  RmlNavDirection stick_direction = RmlNavDirection::None;
  // pressed on the UI, so their releases are the UI's too
  u32 ui_buttons = 0;
};

// Owns everything RmlUi keeps process wide: the library lifecycle, the system interface, the
// debugger, and the routing of window input to whichever view is under the cursor. Per context state
// lives in RmlView.
class RmlUI {
public:
  constexpr static auto MODULE_NAME = "RmlUI";
  using module_dependencies = std::tuple<AssetManager, Input, Renderer>;

  auto init(this RmlUI& self) -> std::expected<void, std::string>;
  auto deinit(this RmlUI& self) -> std::expected<void, std::string>;
  auto update(this RmlUI& self, const Timestep& timestep) -> void;

  // Called by RmlView's constructor and destructor. Views must outlive their registration.
  auto register_view(this RmlUI& self, RmlView* view) -> void;
  auto unregister_view(this RmlUI& self, RmlView* view) -> void;

  // Mouse follows the cursor, keyboard follows the focused view. The bool ones return whether the UI
  // took the event, in which case the game shouldn't see it
  auto process_key(this RmlUI& self, u32 key_code, u16 mods, bool down, bool repeat) -> bool;
  auto process_text(this RmlUI& self, std::string_view text) -> void;
  auto process_mouse_move(this RmlUI& self, glm::vec2 position) -> void;
  auto process_mouse_button(this RmlUI& self, u8 button, bool down) -> bool;
  // SDL's wheel, x to the right and y away from the user
  auto process_mouse_scroll(this RmlUI& self, glm::vec2 offset) -> bool;

  // Goes to the focused view, while `rml_wants_gamepad` says it is the UI's: the d-pad and the left stick move focus,
  // south presses what has it, east is escape and the right stick scrolls around it
  auto process_gamepad_button(this RmlUI& self, u32 instance_id, GamepadButtonCode button, bool down) -> bool;
  auto process_gamepad_axis(this RmlUI& self, u32 instance_id, GamepadAxisCode axis, i16 value) -> bool;
  auto wants_gamepad(this RmlUI& self) -> bool;

  // `reload_rml_documents` for every view, `paths` are virtual
  auto reload_ui_files(this RmlUI& self, std::span<const std::string> paths) -> void;

  // RmlUi's inspector, drawn over the UI it inspects. Outside distribution builds F8 toggles it on the view with
  // keyboard focus
  auto show_debugger(this RmlUI& self, Rml::Context* context) -> bool;
  auto hide_debugger(this RmlUI& self) -> void;
  auto is_debugger_visible(this const RmlUI& self, Rml::Context* context) -> bool;

  // the game holds the mouse, so a cursor nobody sees has nothing to hover or click
  bool is_mouse_locked = false;

private:
  auto keyboard_view(this RmlUI& self) -> RmlView*;
  auto gamepad(this RmlUI& self, u32 instance_id) -> RmlGamepad&;
  auto start_navigation(this RmlUI& self, RmlNavDirection direction, bool is_from_stick) -> void;
  auto stop_navigation(this RmlUI& self) -> void;
  auto scroll_focus(this RmlUI& self, Rml::Context* context, f32 seconds) -> void;
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

  ankerl::svector<RmlGamepad, 4> gamepads = {};
  // held, repeated until it is let go
  RmlNavDirection nav_direction = RmlNavDirection::None;
  bool is_nav_from_stick = false;
  f32 nav_delay = 0.0f;
  // RmlUi scrolls by whole pixels
  glm::vec2 scroll_remainder = {};

  Rml::Context* debugger_host_context = nullptr;
};
} // namespace ox
