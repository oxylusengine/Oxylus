#include "UI/RmlUI.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Debugger.h>

#include "Core/App.hpp"

namespace ox {
auto RmlUI::init() -> std::expected<void, std::string> {
  ZoneScoped;

  Rml::SetSystemInterface(&this->rml_system);
  Rml::SetRenderInterface(&this->rml_renderer);

  if (!Rml::Initialise()) {
    return std::unexpected("Failed to initalize RmlUI!");
  }

  auto window_size = App::get_window().get_real_size();

  auto main_context = Rml::CreateContext("main", Rml::Vector2i(window_size.x, window_size.y));
  if (!main_context) {
    return std::unexpected("Failed to create the main context of RmlUI!");
  }

  auto& event_system = App::get_event_system();
  auto sub_result = event_system.subscribe<WindowResizeEvent>([this](const WindowResizeEvent&) {
    auto ws = App::get_window().get_real_size();
    for (auto& context : this->get_contexts()) {
      context->SetDimensions({static_cast<i32>(ws.x), static_cast<i32>(ws.y)});
    }
  });

  auto& window = App::get_window();
  const f32 dpi_scale = window.get_dpi_scale();
  main_context->SetDensityIndependentPixelRatio(dpi_scale);

  Rml::Debugger::Initialise(main_context);

  uint32_t white_pixel = 0xFFFFFFFF;
  white_texture = Texture::create({
    .format = vuk::Format::eR8G8B8A8Unorm,
    .extent = vuk::Extent3D{1, 1, 1u},
  });

  // TODO load data

  this->rml_renderer.set_white_texture(&*white_texture);

  return {};
}

auto RmlUI::deinit() -> std::expected<void, std::string> {
  ZoneScoped;

  Rml::Shutdown();

  return {};
}

auto RmlUI::update(const Timestep& timestep) -> void {
  ZoneScoped;

  for (const auto& ctx : this->get_contexts()) {
    ctx->Update();
  }
}

auto RmlUI::render_contexts(this RmlUI& self) -> void {
  ZoneScoped;

  for (const auto& ctx : self.get_contexts()) {
    ctx->Render();
  }
}

auto RmlUI::get_renderer(this RmlUI& self) -> RmlRenderer& {
  ZoneScoped;

  return self.rml_renderer;
}

auto RmlUI::get_contexts(this RmlUI& self) -> std::vector<Rml::Context*> {
  ZoneScoped;

  std::vector<Rml::Context*> contexts = {};

  auto num_context = Rml::GetNumContexts();
  contexts.reserve(num_context);
  for (i32 i = 0; i < num_context; i++) {
    contexts.emplace_back(Rml::GetContext(i));
  }

  return contexts;
}

auto RmlUI::get_main_context(this const RmlUI& self) -> Rml::Context* {
  ZoneScoped;

  return Rml::GetContext("main");
}
} // namespace ox
