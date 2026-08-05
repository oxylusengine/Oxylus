#include "UI/RmlUI.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Debugger.h>
#include <algorithm>
#include <ranges>

#include "Core/Keycodes.hpp"
#include "UI/RmlView.hpp"

namespace ox {
// Reverse order so the view registered last wins where two overlap.
static auto hit_test(std::span<RmlView* const> views, glm::vec2 position) -> RmlView* {
  for (auto* view : std::ranges::reverse_view(views)) {
    if (view->viewport_size.x <= 0.f || view->viewport_size.y <= 0.f) {
      continue;
    }

    const auto local = position - view->viewport_origin;
    if (local.x >= 0.f && local.y >= 0.f && local.x < view->viewport_size.x && local.y < view->viewport_size.y) {
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

auto RmlUI::init(this RmlUI& self) -> std::expected<void, std::string> {
  ZoneScoped;

  Rml::SetSystemInterface(&self.rml_system);

  // No `Rml::SetRenderInterface`: every context brings its own.
  if (!Rml::Initialise()) {
    return std::unexpected("Failed to initalize RmlUI!");
  }

  return {};
}

auto RmlUI::deinit(this RmlUI& self) -> std::expected<void, std::string> {
  ZoneScoped;

  Rml::Shutdown();

  return {};
}

auto RmlUI::register_view(this RmlUI& self, RmlView* view) -> void {
  ZoneScoped;

  OX_CHECK_NULL(view);

  self.views.push_back(view);

  if (!self.debugger_host_context && Rml::Debugger::Initialise(view->context())) {
    self.debugger_host_context = view->context();
  }
}

auto RmlUI::unregister_view(this RmlUI& self, RmlView* view) -> void {
  ZoneScoped;

  std::erase(self.views, view);

  if (self.hovered_view == view) {
    self.hovered_view = nullptr;
  }
  if (self.capture_view == view) {
    self.capture_view = nullptr;
    self.held_buttons = 0;
  }

  // The debugger draws through its host context, so it cannot outlive it.
  if (view->context() && self.debugger_host_context == view->context()) {
    Rml::Debugger::Shutdown();
    self.debugger_host_context = nullptr;
  }
}

auto RmlUI::keyboard_view(this RmlUI& self) -> RmlView* {
  for (auto* view : self.views) {
    if (view->keyboard_focused) {
      return view;
    }
  }

  return nullptr;
}

auto RmlUI::update_hover(this RmlUI& self, glm::vec2 position) -> RmlView* {
  auto* target = self.capture_view ? self.capture_view : hit_test(self.views, position);

  if (self.hovered_view && self.hovered_view != target) {
    // Without this the last hovered element stays lit once the cursor moves to another view.
    self.hovered_view->context()->ProcessMouseLeave();
  }
  self.hovered_view = target;

  if (target) {
    const auto coords = to_surface_coords(*target, position);
    target->context()->ProcessMouseMove(coords.x, coords.y, 0);
  }

  return target;
}

auto RmlUI::process_key(this RmlUI& self, u32 key_code, u16 mods, bool down) -> void {
  ZoneScoped;

  auto* view = self.keyboard_view();
  if (!view) {
    return;
  }

  if (down) {
    view->context()->ProcessKeyDown(RmlSystem::convert_key(key_code), RmlSystem::convert_mod(mods));
  } else {
    view->context()->ProcessKeyUp(RmlSystem::convert_key(key_code), RmlSystem::convert_mod(mods));
  }
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

auto RmlUI::process_mouse_button(this RmlUI& self, u8 button, bool down) -> void {
  ZoneScoped;

  i32 rml_button = 0;
  switch (static_cast<MouseCode>(button)) {
    case MouseCode::Left  : rml_button = 0; break;
    case MouseCode::Middle: rml_button = 2; break;
    case MouseCode::Right : rml_button = 1; break;
    default               : return;
  }

  const auto button_bit = static_cast<u8>(1u << rml_button);

  // Re-resolve rather than trust the last motion event: the layout may have shifted under a
  // stationary cursor, and RmlUi applies the click wherever its own last move landed.
  auto* target = self.update_hover(self.last_mouse_position);

  if (down) {
    if (!target) {
      return;
    }

    self.capture_view = target;
    self.held_buttons |= button_bit;
    target->context()->ProcessMouseButtonDown(rml_button, 0);

    return;
  }

  if (!target) {
    return;
  }

  target->context()->ProcessMouseButtonUp(rml_button, 0);

  self.held_buttons &= static_cast<u8>(~button_bit);
  if (self.held_buttons == 0) {
    self.capture_view = nullptr;
  }
}

auto RmlUI::process_mouse_scroll(this RmlUI& self, f32 offset) -> void {
  ZoneScoped;

  if (auto* target = self.update_hover(self.last_mouse_position)) {
    target->context()->ProcessMouseWheel(-offset, 0);
  }
}
} // namespace ox
