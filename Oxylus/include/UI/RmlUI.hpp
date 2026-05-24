#pragma once

#include <RmlUi/Core/Types.h>
#include <expected>
#include <span>
#include <string>

#include "Core/Option.hpp"
#include "Core/Types.hpp"
#include "UI/RmlRenderer.hpp"
#include "UI/RmlSystem.hpp"

namespace ox {
class Renderer;
class Input;

class RmlUI {
public:
  constexpr static auto MODULE_NAME = "RmlUI";
  using module_dependencies = std::tuple<Input, Renderer>;

  auto init() -> std::expected<void, std::string>;
  auto deinit() -> std::expected<void, std::string>;

  auto update() -> void;

  auto render_contexts(this RmlUI& self) -> void;

  auto add_context(this RmlUI& self, u32 width, u32 height) -> option<Rml::Context*>;
  auto get_contexts(this RmlUI& self) -> std::span<Rml::Context*>;
  auto get_main_context(this const RmlUI& self) -> Rml::Context*;

private:
  std::vector<Rml::Context*> contexts = {};
  RmlRenderer rml_renderer = {};
  RmlSystem rml_system = {};
};
} // namespace ox
