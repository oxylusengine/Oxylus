#pragma once

#include <RmlUi/Core/Types.h>
#include <expected>
#include <span>
#include <string>

#include "UI/RmlRenderer.hpp"
#include "UI/RmlSystem.hpp"
#include "Utils/Timestep.hpp"

namespace ox {
class Renderer;
class Input;

class RmlUI {
public:
  constexpr static auto MODULE_NAME = "RmlUI";
  using module_dependencies = std::tuple<Input, Renderer>;

  auto init() -> std::expected<void, std::string>;
  auto deinit() -> std::expected<void, std::string>;

  auto update(const Timestep& timestep) -> void;

  auto render_contexts(this RmlUI& self) -> void;
  auto get_renderer(this RmlUI& self) -> RmlRenderer&;

  auto get_contexts(this RmlUI& self) -> std::vector<Rml::Context*>;
  auto get_main_context(this const RmlUI& self) -> Rml::Context*;

private:
  RmlRenderer rml_renderer = {};
  RmlSystem rml_system = {};
  std::unique_ptr<Texture> white_texture = nullptr;
};
} // namespace ox
