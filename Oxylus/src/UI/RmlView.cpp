#include "UI/RmlView.hpp"

#include <RmlUi/Core.h>

#include "Core/App.hpp"
#include "UI/RmlRenderer.hpp"
#include "UI/RmlUI.hpp"

namespace ox {
RmlView::RmlView(std::string_view name) {
  ZoneScoped;

  this->renderer = std::make_unique<RmlRenderer>();

  this->rml_context = Rml::CreateContext(Rml::String(name), {1, 1}, this->renderer.get());
  if (!this->rml_context) {
    OX_LOG_ERROR("Failed to create RmlUi context '{}'.", name);
    this->renderer.reset();
    return;
  }

  App::mod<RmlUI>().register_view(this);
}

RmlView::~RmlView() {
  ZoneScoped;

  if (!this->rml_context) {
    return;
  }

  App::mod<RmlUI>().unregister_view(this);

  Rml::RemoveContext(this->rml_context->GetName());
  this->rml_context = nullptr;

  // Releases the geometry and textures the render manager still holds, through the interface, which
  // therefore has to outlive this call.
  Rml::ReleaseRenderManagers();
  this->renderer.reset();
}

auto RmlView::update(this RmlView& self, glm::ivec2 surface_size_) -> void {
  ZoneScoped;

  if (!self.rml_context || surface_size_.x <= 0 || surface_size_.y <= 0) {
    return;
  }

  self.surface_size = surface_size_;

  self.renderer->begin_frame();
  self.rml_context->SetDimensions(Rml::Vector2i(surface_size_.x, surface_size_.y));
  self.rml_context->Render();
}

auto RmlView::draw(this RmlView& self, RenderContext& context, vuk::Value<vuk::ImageAttachment> target)
  -> vuk::Value<vuk::ImageAttachment> {
  ZoneScoped;

  if (!self.rml_context) {
    return target;
  }

  return self.renderer->end_frame(context, std::move(target));
}

auto RmlView::set_viewport(this RmlView& self, glm::ivec2 origin, glm::ivec2 size, bool keyboard_focused_) -> void {
  self.viewport_origin = {static_cast<f32>(origin.x), static_cast<f32>(origin.y)};
  self.viewport_size = {static_cast<f32>(size.x), static_cast<f32>(size.y)};
  self.keyboard_focused = keyboard_focused_;
}

auto RmlView::set_dpi_ratio(this const RmlView& self, f32 ratio) -> void {
  ZoneScoped;

  if (self.rml_context) {
    self.rml_context->SetDensityIndependentPixelRatio(ratio);
  }
}

auto RmlView::name(this const RmlView& self) -> std::string_view {
  return self.rml_context ? std::string_view(self.rml_context->GetName()) : std::string_view{};
}
} // namespace ox
