#include "UI/RmlUI.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Debugger.h>

#include "Core/App.hpp"
#include "Core/Input.hpp"
#include "Core/Keycodes.hpp"
#include "UI/RmlRenderer.hpp"

namespace ox {
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

auto RmlUI::create_context(this RmlUI& self, std::string_view name, RmlRenderer* renderer) -> Rml::Context* {
  ZoneScoped;

  OX_CHECK_NULL(renderer);

  auto* context = Rml::CreateContext(Rml::String(name), {1, 1}, renderer);
  if (!context) {
    return nullptr;
  }

  context->SetDensityIndependentPixelRatio(App::get_window().get_dpi_scale());
  if (!self.debugger_host_context && Rml::Debugger::Initialise(context)) {
    self.debugger_host_context = context;
  }

  return context;
}

auto RmlUI::remove_context(this RmlUI& self, Rml::Context* context) -> void {
  ZoneScoped;

  if (!context) {
    return;
  }

  if (self.input_context == context) {
    self.clear_input_context();
  }

  // The debugger draws through its host context, so it cannot outlive it.
  if (self.debugger_host_context == context) {
    Rml::Debugger::Shutdown();
    self.debugger_host_context = nullptr;
  }

  Rml::RemoveContext(context->GetName());

  // Frees the render manager holding this interface, so the caller can destroy it before `Rml::Shutdown`.
  Rml::ReleaseRenderManagers();
}

auto RmlUI::render_context(this RmlUI& self, Rml::Context& context, Rml::Vector2i dimensions) -> void {
  ZoneScoped;

  context.SetDimensions(dimensions);
  context.Render();
}

auto RmlUI::set_input_context(
  this RmlUI& self,
  Rml::Context* context,
  Rml::Vector2f viewport_origin,
  Rml::Vector2f viewport_size,
  Rml::Vector2i surface_size
) -> void {
  self.input_context = context;
  self.input_viewport_origin = viewport_origin;
  self.input_viewport_size = viewport_size;
  self.input_surface_size = surface_size;
  auto mouse_position = App::mod<Input>().get_mouse_position();
  self.process_mouse_move({mouse_position.x, mouse_position.y});
}

auto RmlUI::clear_input_context(this RmlUI& self) -> void {
  self.input_context = nullptr;
  self.input_viewport_origin = {};
  self.input_viewport_size = {};
  self.input_surface_size = {};
  self.input_mouse_inside = false;
}

auto RmlUI::process_key(this RmlUI& self, u32 key_code, u16 mods, bool down) -> void {
  if (!self.input_context) {
    return;
  }

  if (down) {
    self.input_context->ProcessKeyDown(RmlSystem::convert_key(key_code), RmlSystem::convert_mod(mods));
  } else {
    self.input_context->ProcessKeyUp(RmlSystem::convert_key(key_code), RmlSystem::convert_mod(mods));
  }
}

auto RmlUI::process_text(this RmlUI& self, std::string_view text) -> void {
  if (self.input_context) {
    self.input_context->ProcessTextInput(Rml::String(text));
  }
}

auto RmlUI::process_mouse_move(this RmlUI& self, Rml::Vector2f position) -> void {
  if (!self.input_context || self.input_viewport_size.x <= 0.0f || self.input_viewport_size.y <= 0.0f) {
    return;
  }

  auto local_position = position - self.input_viewport_origin;
  self.input_mouse_inside = local_position.x >= 0.0f && local_position.y >= 0.0f &&
                            local_position.x < self.input_viewport_size.x &&
                            local_position.y < self.input_viewport_size.y;
  if (!self.input_mouse_inside) {
    return;
  }

  const f32 x = local_position.x * static_cast<f32>(self.input_surface_size.x) / self.input_viewport_size.x;
  const f32 y = local_position.y * static_cast<f32>(self.input_surface_size.y) / self.input_viewport_size.y;
  self.input_context->ProcessMouseMove(static_cast<i32>(x), static_cast<i32>(y), 0);
}

auto RmlUI::process_mouse_button(this RmlUI& self, u8 button, bool down) -> void {
  if (!self.input_context || !self.input_mouse_inside) {
    return;
  }

  i32 rml_button = 0;
  switch (static_cast<MouseCode>(button)) {
    case MouseCode::Left  : rml_button = 0; break;
    case MouseCode::Middle: rml_button = 2; break;
    case MouseCode::Right : rml_button = 1; break;
    default               : return;
  }

  if (down) {
    self.input_context->ProcessMouseButtonDown(rml_button, 0);
  } else {
    self.input_context->ProcessMouseButtonUp(rml_button, 0);
  }
}

auto RmlUI::process_mouse_scroll(this RmlUI& self, f32 offset) -> void {
  if (self.input_context && self.input_mouse_inside) {
    self.input_context->ProcessMouseWheel(-offset, 0);
  }
}

} // namespace ox
