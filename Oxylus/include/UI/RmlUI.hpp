#pragma once

#include <RmlUi/Core/Types.h>
#include <expected>
#include <string>
#include <string_view>

#include "UI/RmlRenderer.hpp"
#include "UI/RmlSystem.hpp"

namespace ox {
class Renderer;
class Input;

class RmlUI {
public:
  constexpr static auto MODULE_NAME = "RmlUI";
  using module_dependencies = std::tuple<Input, Renderer>;

  auto init(this RmlUI& self) -> std::expected<void, std::string>;
  auto deinit(this RmlUI& self) -> std::expected<void, std::string>;

  auto begin_frame(this RmlUI& self) -> void;

  auto create_context(this RmlUI& self, std::string_view name) -> Rml::Context*;
  auto remove_context(this RmlUI& self, Rml::Context* context) -> void;
  auto render_context(this RmlUI& self, Rml::Context& context, Rml::Vector2i dimensions) -> void;

  auto set_input_context(
    this RmlUI& self,
    Rml::Context* context,
    Rml::Vector2f viewport_origin,
    Rml::Vector2f viewport_size,
    Rml::Vector2i surface_size
  ) -> void;
  auto clear_input_context(this RmlUI& self) -> void;
  auto process_key(this RmlUI& self, u32 key_code, u16 mods, bool down) -> void;
  auto process_text(this RmlUI& self, std::string_view text) -> void;
  auto process_mouse_move(this RmlUI& self, Rml::Vector2f position) -> void;
  auto process_mouse_button(this RmlUI& self, u8 button, bool down) -> void;
  auto process_mouse_scroll(this RmlUI& self, f32 offset) -> void;

  auto get_renderer(this RmlUI& self) -> RmlRenderer&;

private:
  RmlRenderer rml_renderer = {};
  RmlSystem rml_system = {};
  Texture white_texture = {};
  Rml::Context* input_context = nullptr;
  Rml::Vector2f input_viewport_origin = {};
  Rml::Vector2f input_viewport_size = {};
  Rml::Vector2i input_surface_size = {};
  bool input_mouse_inside = false;
  bool debugger_initialized = false;
};
} // namespace ox
