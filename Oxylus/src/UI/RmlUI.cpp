#include "UI/RmlUI.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Debugger.h>
#include <SDL3/SDL_keyboard.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <glm/common.hpp>
#include <ranges>
#include <utility>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Core/Keycodes.hpp"
#include "Memory/Stack.hpp"
#include "UI/RmlView.hpp"
#include "Utils/Log.hpp"
#include "Utils/Timestep.hpp"

namespace ox {
constexpr static auto NAV_REPEAT_DELAY = 0.4f;
constexpr static auto NAV_REPEAT_INTERVAL = 0.12f;
// a stick lets go later than it engages, so one resting near the edge doesn't flutter
constexpr static auto STICK_ENGAGE = 0.6f;
constexpr static auto STICK_RELEASE = 0.4f;
constexpr static auto STICK_AXIS_BIAS = 1.5f;
constexpr static auto SCROLL_DEAD_ZONE = 0.2f;
// dp per second at full tilt
constexpr static auto SCROLL_SPEED = 1000.0f;

// mouse events carry no modifiers, so they are read when the event arrives: shift-click extends a text selection
static auto key_modifiers() -> i32 { return RmlSystem::convert_mod(static_cast<i16>(SDL_GetModState())); }

// an owner may render before RmlUI's update or after it, so a shown view is up to one frame behind
static auto is_placed(const RmlView& view) -> bool { return view.frames_unplaced <= 1; }

// Reverse order so the view registered last wins where two overlap.
static auto hit_test(std::span<RmlView* const> views, glm::vec2 position) -> RmlView* {
  for (auto* view : std::ranges::reverse_view(views)) {
    if (!is_placed(*view) || !view->accepts_mouse) {
      continue;
    }

    if (view->viewport_size.x <= 0.0f || view->viewport_size.y <= 0.0f) {
      continue;
    }

    const auto local = position - view->viewport_origin;
    if (local.x >= 0.0f && local.y >= 0.0f && local.x < view->viewport_size.x && local.y < view->viewport_size.y) {
      return view;
    }
  }

  return nullptr;
}

static auto to_surface_coords(const RmlView& view, glm::vec2 position) -> glm::ivec2 {
  const auto local = position - view.viewport_origin;

  return {
    static_cast<i32>(local.x * static_cast<f32>(view.surface_size.x) / view.viewport_size.x),
    static_cast<i32>(local.y * static_cast<f32>(view.surface_size.y) / view.viewport_size.y),
  };
}

auto rml_wants_keyboard(Rml::Context* context) -> bool {
  auto* focus = context ? context->GetFocusElement() : nullptr;
  if (!focus) {
    return false;
  }

  const auto& tag = focus->GetTagName();
  if (tag == "textarea") {
    return true;
  }

  if (tag != "input") {
    return false;
  }

  // RmlUi makes a text field of every input type it doesn't know
  const auto type = focus->GetAttribute<Rml::String>("type", "text");
  return type != "radio" && type != "checkbox" && type != "range" && type != "submit" && type != "button";
}

static auto is_horizontal(RmlNavDirection direction) -> bool {
  return direction == RmlNavDirection::Left || direction == RmlNavDirection::Right;
}

static auto nav_key(RmlNavDirection direction) -> Rml::Input::KeyIdentifier {
  switch (direction) {
    case RmlNavDirection::Up   : return Rml::Input::KI_UP;
    case RmlNavDirection::Down : return Rml::Input::KI_DOWN;
    case RmlNavDirection::Left : return Rml::Input::KI_LEFT;
    case RmlNavDirection::Right: return Rml::Input::KI_RIGHT;
    case RmlNavDirection::None : break;
  }

  return Rml::Input::KI_UNKNOWN;
}

static auto dpad_direction(GamepadButtonCode button) -> RmlNavDirection {
  switch (button) {
    case GamepadButtonCode::DPadUp   : return RmlNavDirection::Up;
    case GamepadButtonCode::DPadDown : return RmlNavDirection::Down;
    case GamepadButtonCode::DPadLeft : return RmlNavDirection::Left;
    case GamepadButtonCode::DPadRight: return RmlNavDirection::Right;
    default                          : return RmlNavDirection::None;
  }
}

static auto stick_direction(glm::vec2 stick, RmlNavDirection current) -> RmlNavDirection {
  auto horizontal = std::abs(stick.x);
  auto vertical = std::abs(stick.y);
  const auto is_engaged = current != RmlNavDirection::None;
  if (std::max(horizontal, vertical) < (is_engaged ? STICK_RELEASE : STICK_ENGAGE)) {
    return RmlNavDirection::None;
  }

  // a diagonal keeps the axis it is on
  if (is_engaged) {
    (is_horizontal(current) ? horizontal : vertical) *= STICK_AXIS_BIAS;
  }

  if (horizontal > vertical) {
    return stick.x < 0.0f ? RmlNavDirection::Left : RmlNavDirection::Right;
  }

  return stick.y < 0.0f ? RmlNavDirection::Up : RmlNavDirection::Down;
}

static auto has_navigation(Rml::Element& element) -> bool {
  return std::ranges::any_of(
    std::array{Rml::PropertyId::NavUp, Rml::PropertyId::NavDown, Rml::PropertyId::NavLeft, Rml::PropertyId::NavRight},
    [&element](Rml::PropertyId id) {
      const auto* property = element.GetLocalProperty(id);
      return property &&
             (property->unit != Rml::Unit::KEYWORD || property->Get<i32>() != static_cast<i32>(Rml::Style::Nav::None));
    }
  );
}

static auto is_tabbed_to(Rml::Element& element) -> bool {
  return element.IsVisible(true) && element.GetComputedValues().tab_index() == Rml::Style::TabIndex::Auto;
}

auto rml_wants_gamepad(Rml::Context* context) -> bool {
  // a click focuses whatever it hits and a hidden panel keeps the focus in it, RmlUi navigates from what that is inside
  for (auto* element = context ? context->GetFocusElement() : nullptr; element; element = element->GetParentNode()) {
    if (is_tabbed_to(*element) || (element->IsVisible(true) && has_navigation(*element))) {
      return true;
    }
  }

  return false;
}

// what RmlUi's own navigation leaves alone, a control without `nav`, goes along the tab order
static auto navigate(Rml::Context& context, RmlNavDirection direction) -> void {
  auto* from = context.GetFocusElement();
  while (from && !is_tabbed_to(*from)) {
    from = from->GetParentNode();
  }

  const auto is_in_tab_order = from && !has_navigation(*from);
  const auto is_propagated = context.ProcessKeyDown(nav_key(direction), 0);
  if (!is_propagated || !is_in_tab_order) {
    return;
  }

  const auto is_backward = direction == RmlNavDirection::Up || direction == RmlNavDirection::Left;
  context.ProcessKeyDown(Rml::Input::KI_TAB, is_backward ? Rml::Input::KM_SHIFT : 0);
}

static auto normalized_path(std::string_view path) -> std::string {
  auto normalized = std::filesystem::path(path).lexically_normal().generic_string();
  // a document keeps a drive letter the way old urls spelled it, `C:/a` comes back as `C|/a`
  if (normalized.size() >= 2 && normalized[1] == '|' && std::isalpha(static_cast<unsigned char>(normalized[0]))) {
    normalized[1] = ':';
  }

  return normalized;
}

auto reload_rml_documents(Rml::Context* context, std::span<const std::string> paths) -> void {
  ZoneScoped;

  if (!context || paths.empty()) {
    return;
  }

  const auto is_style_changed = std::ranges::any_of(paths, [](std::string_view path) {
    return path.ends_with(".rcss");
  });
  const auto is_markup_changed = std::ranges::any_of(paths, [](std::string_view path) {
    return path.ends_with(".rml");
  });
  if (is_style_changed) {
    Rml::Factory::ClearStyleSheetCache();
  }

  // a template reaches a document only when the document is loaded again
  if (is_markup_changed) {
    Rml::Factory::ClearTemplateCache();
  }

  memory::ScopedStack stack;
  // loading and closing reorders the context's documents
  auto documents = stack.alloc<Rml::ElementDocument*>(static_cast<usize>(context->GetNumDocuments()));
  for (usize i = 0; i < documents.size(); i++) {
    documents[i] = context->GetDocument(static_cast<i32>(i));
  }

  for (auto* document : documents) {
    const auto url = document->GetSourceURL();
    // the debugger's documents are built in memory
    if (url.empty()) {
      continue;
    }

    const auto document_path = normalized_path(url);
    const auto is_edited = std::ranges::any_of(paths, [&document_path](std::string_view path) {
      return normalized_path(path) == document_path;
    });
    if (!is_edited) {
      if (is_style_changed) {
        document->ReloadStyleSheet();
      }

      continue;
    }

    const auto is_visible = document->IsVisible();
    const auto is_modal = document->IsModal();
    auto* replacement = context->LoadDocument(url);
    if (!replacement) {
      OX_LOG_ERROR("Couldn't reload {}, the document on screen stays as it was", url);
      continue;
    }

    document->Close();
    if (is_visible) {
      replacement->Show(is_modal ? Rml::ModalFlag::Modal : Rml::ModalFlag::None);
    }

    OX_LOG_INFO("Reloaded {}", url);
  }
}

auto RmlUI::init(this RmlUI& self) -> std::expected<void, std::string> {
  ZoneScoped;

  Rml::SetSystemInterface(&self.rml_system);
  // before `Initialise`, which is when RmlUi settles on one
  self.rml_files.vfs = &App::get_vfs();
  self.rml_files.asset_man = &App::mod<AssetManager>();
  Rml::SetFileInterface(&self.rml_files);

  // No `Rml::SetRenderInterface`: every context brings its own.
  if (!Rml::Initialise()) {
    return std::unexpected("Failed to initalize RmlUI!");
  }

  return {};
}

auto RmlUI::deinit(this RmlUI& self) -> std::expected<void, std::string> {
  ZoneScoped;

  Rml::Shutdown();
  self.debugger_host_context = nullptr;

  return {};
}

auto RmlUI::update(this RmlUI& self, const Timestep& timestep) -> void {
  ZoneScoped;

  for (auto* view : self.views) {
    view->frames_unplaced++;
  }

  if (!self.wants_gamepad()) {
    self.stop_navigation();
    self.scroll_remainder = {};
    return;
  }

  auto* context = self.keyboard_view()->context();
  const auto seconds = static_cast<f32>(timestep.get_seconds());
  self.scroll_focus(context, seconds);

  if (self.nav_direction == RmlNavDirection::None) {
    return;
  }

  self.nav_delay -= seconds;
  if (self.nav_delay <= 0.0f) {
    self.nav_delay = NAV_REPEAT_INTERVAL;
    navigate(*context, self.nav_direction);
  }
}

auto RmlUI::register_view(this RmlUI& self, RmlView* view) -> void {
  ZoneScoped;

  OX_CHECK_NULL(view);

  self.views.push_back(view);
}

auto RmlUI::unregister_view(this RmlUI& self, RmlView* view) -> void {
  ZoneScoped;

  std::erase(self.views, view);

  if (self.hovered_view == view) {
    self.hovered_view = nullptr;
    // nothing is left to hover out of it
    self.rml_system.SetMouseCursor({});
  }

  if (self.capture_view == view) {
    self.capture_view = nullptr;
    self.held_buttons = 0;
  }

  // The debugger draws through its host context, so it cannot outlive it.
  if (view->context() && self.debugger_host_context == view->context()) {
    self.hide_debugger();
  }
}

auto RmlUI::keyboard_view(this RmlUI& self) -> RmlView* {
  for (auto* view : self.views) {
    if (view->keyboard_focused && is_placed(*view)) {
      return view;
    }
  }

  return nullptr;
}

auto RmlUI::update_hover(this RmlUI& self, glm::vec2 position) -> RmlView* {
  auto* target = self.capture_view ? self.capture_view : hit_test(self.views, position);
  // a press on the UI is followed to its release, even when what it pressed locked the mouse
  if (self.is_mouse_locked && self.ui_buttons == 0) {
    target = nullptr;
  }

  if (self.hovered_view && self.hovered_view != target) {
    // Without this the last hovered element stays lit once the cursor moves to another view.
    self.hovered_view->context()->ProcessMouseLeave();
  }
  self.hovered_view = target;

  if (target) {
    const auto coords = to_surface_coords(*target, position);
    target->context()->ProcessMouseMove(coords.x, coords.y, key_modifiers());
  }

  return target;
}

auto RmlUI::process_key(this RmlUI& self, u32 key_code, u16 mods, bool down, bool repeat) -> bool {
  ZoneScoped;

  auto* view = self.keyboard_view();
  if (!view) {
    return false;
  }

  auto* context = view->context();
  const auto key = RmlSystem::convert_key(key_code);
#ifndef OX_DISTRIBUTION
  if (key == Rml::Input::KI_F8) {
    const auto is_pressed = down && !repeat;
    if (is_pressed && self.is_debugger_visible(context)) {
      self.hide_debugger();
    } else if (is_pressed) {
      self.show_debugger(context);
    }

    return true;
  }
#endif

  const auto modifiers = RmlSystem::convert_mod(mods);
  const auto is_propagated = down ? context->ProcessKeyDown(key, modifiers) : context->ProcessKeyUp(key, modifiers);

  return !is_propagated || rml_wants_keyboard(context);
}

auto RmlUI::process_text(this RmlUI& self, std::string_view text) -> void {
  ZoneScoped;

  if (auto* view = self.keyboard_view()) {
    view->context()->ProcessTextInput(Rml::String(text));
  }
}

auto RmlUI::process_mouse_move(this RmlUI& self, glm::vec2 position) -> void {
  ZoneScoped;

  self.last_mouse_position = position;
  self.update_hover(position);
}

auto RmlUI::process_mouse_button(this RmlUI& self, u8 button, bool down) -> bool {
  ZoneScoped;

  i32 rml_button = 0;
  switch (static_cast<MouseCode>(button)) {
    case MouseCode::Left  : rml_button = 0; break;
    case MouseCode::Middle: rml_button = 2; break;
    case MouseCode::Right : rml_button = 1; break;
    default               : return false;
  }

  const auto button_bit = static_cast<u8>(1u << rml_button);

  // Re-resolve rather than trust the last motion event: the layout may have shifted under a
  // stationary cursor, and RmlUi applies the click wherever its own last move landed.
  auto* target = self.update_hover(self.last_mouse_position);

  if (down) {
    if (!target) {
      return false;
    }

    self.capture_view = target;
    self.held_buttons |= button_bit;

    auto* context = target->context();
    context->ProcessMouseButtonDown(rml_button, key_modifiers());
    const auto is_ui_press = context->IsMouseInteracting();
    if (is_ui_press) {
      self.ui_buttons |= button_bit;
    } else if (rml_wants_keyboard(context)) {
      // clicking into the game hands the keyboard back, RmlUi only moves focus to what was clicked
      context->GetFocusElement()->Blur();
    }

    return is_ui_press;
  }

  const auto is_ui_release = (self.ui_buttons & button_bit) != 0;
  self.ui_buttons &= static_cast<u8>(~button_bit);

  // a press in the game is followed too, and released though the game has locked the mouse since
  auto* released = self.capture_view ? self.capture_view : target;
  if (!released) {
    return is_ui_release;
  }

  released->context()->ProcessMouseButtonUp(rml_button, key_modifiers());

  self.held_buttons &= static_cast<u8>(~button_bit);
  if (self.held_buttons == 0) {
    self.capture_view = nullptr;
  }

  return is_ui_release;
}

auto RmlUI::process_mouse_scroll(this RmlUI& self, glm::vec2 offset) -> bool {
  ZoneScoped;

  auto* target = self.update_hover(self.last_mouse_position);
  if (!target) {
    return false;
  }

  const auto is_propagated = target->context()->ProcessMouseWheel(Rml::Vector2f(offset.x, -offset.y), key_modifiers());
  return !is_propagated || target->context()->IsMouseInteracting();
}

auto RmlUI::wants_gamepad(this RmlUI& self) -> bool {
  auto* view = self.keyboard_view();
  return view && rml_wants_gamepad(view->context());
}

auto RmlUI::gamepad(this RmlUI& self, u32 instance_id) -> RmlGamepad& {
  const auto it = std::ranges::find(self.gamepads, instance_id, &RmlGamepad::instance_id);
  return it == self.gamepads.end() ? self.gamepads.emplace_back(RmlGamepad{.instance_id = instance_id}) : *it;
}

auto RmlUI::start_navigation(this RmlUI& self, RmlNavDirection direction, bool is_from_stick) -> void {
  self.nav_direction = direction;
  self.is_nav_from_stick = is_from_stick;
  self.nav_delay = NAV_REPEAT_DELAY;
  navigate(*self.keyboard_view()->context(), direction);
}

auto RmlUI::stop_navigation(this RmlUI& self) -> void { self.nav_direction = RmlNavDirection::None; }

auto RmlUI::scroll_focus(this RmlUI& self, Rml::Context* context, f32 seconds) -> void {
  auto stick = glm::vec2(0.0f);
  for (const auto& gamepad : self.gamepads) {
    for (const auto axis : {0, 1}) {
      const auto tilt = std::abs(gamepad.right_stick[axis]);
      if (tilt > SCROLL_DEAD_ZONE) {
        stick[axis] += std::copysign((tilt - SCROLL_DEAD_ZONE) / (1.0f - SCROLL_DEAD_ZONE), gamepad.right_stick[axis]);
      }
    }
  }

  auto* scrolled = stick == glm::vec2(0.0f) ? nullptr : context->GetFocusElement()->GetClosestScrollableContainer();
  if (!scrolled) {
    self.scroll_remainder = {};
    return;
  }

  self.scroll_remainder += stick * SCROLL_SPEED * context->GetDensityIndependentPixelRatio() * seconds;
  const auto pixels = glm::trunc(self.scroll_remainder);
  self.scroll_remainder -= pixels;
  if (pixels.x != 0.0f) {
    scrolled->SetScrollLeft(scrolled->GetScrollLeft() + pixels.x);
  }

  if (pixels.y != 0.0f) {
    scrolled->SetScrollTop(scrolled->GetScrollTop() + pixels.y);
  }
}

auto RmlUI::process_gamepad_button(this RmlUI& self, u32 instance_id, GamepadButtonCode button, bool down) -> bool {
  ZoneScoped;

  const auto direction = dpad_direction(button);
  auto key = nav_key(direction);
  if (button == GamepadButtonCode::South) {
    key = Rml::Input::KI_RETURN;
  } else if (button == GamepadButtonCode::East) {
    key = Rml::Input::KI_ESCAPE;
  }

  if (key == Rml::Input::KI_UNKNOWN) {
    return false;
  }

  auto& pad = self.gamepad(instance_id);
  const auto button_bit = 1u << static_cast<u32>(button);
  auto* view = self.keyboard_view();
  if (!down) {
    if (!self.is_nav_from_stick && self.nav_direction == direction) {
      self.stop_navigation();
    }

    const auto is_ui_release = (pad.ui_buttons & button_bit) != 0;
    pad.ui_buttons &= ~button_bit;
    if (is_ui_release && view) {
      view->context()->ProcessKeyUp(key, 0);
    }

    return is_ui_release;
  }

  if (!view || !rml_wants_gamepad(view->context())) {
    return false;
  }

  // the press may close the menu it lands on, its release is still the UI's
  pad.ui_buttons |= button_bit;
  if (direction != RmlNavDirection::None) {
    self.start_navigation(direction, false);
    return true;
  }

  const auto is_propagated = view->context()->ProcessKeyDown(key, 0);
  if (is_propagated && button == GamepadButtonCode::East) {
    // nothing in the UI goes back on it, the game may
    pad.ui_buttons &= ~button_bit;
    return false;
  }

  return true;
}

auto RmlUI::process_gamepad_axis(this RmlUI& self, u32 instance_id, GamepadAxisCode axis, i16 value) -> bool {
  ZoneScoped;

  const auto tilt = std::max(static_cast<f32>(value) / 32767.0f, -1.0f);
  auto& pad = self.gamepad(instance_id);
  switch (axis) {
    case GamepadAxisCode::AxisLeftX : pad.left_stick.x = tilt; break;
    case GamepadAxisCode::AxisLeftY : pad.left_stick.y = tilt; break;
    case GamepadAxisCode::AxisRightX: pad.right_stick.x = tilt; break;
    case GamepadAxisCode::AxisRightY: pad.right_stick.y = tilt; break;
    default                         : return false;
  }

  // kept while the game has the stick, one already tilted when a menu opens moves nothing until it is let go
  const auto previous = std::exchange(pad.stick_direction, stick_direction(pad.left_stick, pad.stick_direction));
  const auto is_changed = previous != pad.stick_direction;
  if (is_changed && self.is_nav_from_stick) {
    self.stop_navigation();
  }

  if (!self.wants_gamepad()) {
    return false;
  }

  if (is_changed && pad.stick_direction != RmlNavDirection::None) {
    self.start_navigation(pad.stick_direction, true);
  }

  return true;
}

auto RmlUI::reload_ui_files(this RmlUI& self, std::span<const std::string> paths) -> void {
  ZoneScoped;

  for (auto* view : self.views) {
    reload_rml_documents(view->context(), paths);
  }
}

auto RmlUI::show_debugger(this RmlUI& self, Rml::Context* context) -> bool {
  ZoneScoped;

  if (!context) {
    return false;
  }

  if (self.is_debugger_visible(context)) {
    return true;
  }

  // its documents live in the context they inspect, so inspecting another context moves them there
  self.hide_debugger();
  if (!Rml::Debugger::Initialise(context)) {
    return false;
  }

  self.debugger_host_context = context;
  Rml::Debugger::SetVisible(true);
  return true;
}

auto RmlUI::hide_debugger(this RmlUI& self) -> void {
  ZoneScoped;

  // `SetVisible` only hides the menu, the panels opened from it would stay on screen
  if (self.debugger_host_context) {
    Rml::Debugger::Shutdown();
    self.debugger_host_context = nullptr;
  }
}

auto RmlUI::is_debugger_visible(this const RmlUI& self, Rml::Context* context) -> bool {
  if (!context || self.debugger_host_context != context) {
    return false;
  }

  // unloading every document takes the debugger's with it, and `IsVisible` doesn't check
  return context->GetDocument("rmlui-debug-menu") && Rml::Debugger::IsVisible();
}
} // namespace ox
