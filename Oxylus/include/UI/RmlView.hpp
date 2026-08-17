#pragma once

#include <RmlUi/Core/Types.h>
#include <glm/vec2.hpp>
#include <memory>
#include <string_view>
#include <vuk/ImageAttachment.hpp>
#include <vuk/Value.hpp>

#include "Core/Types.hpp"

namespace ox {
class RenderContext;
class RmlRenderer;

// One RmlUi context plus the renderer that draws it, owned by whatever owns the UI (one per Scene).
// Contexts cannot share a renderer: two of them drawing in the same frame would acquire the same
// attachments twice. Registers itself with the RmlUI module so input can be routed to it.
class RmlView {
public:
  explicit RmlView(std::string_view name);
  ~RmlView();

  RmlView(const RmlView&) = delete;
  auto operator=(const RmlView&) -> RmlView& = delete;

  // Collects this frame's geometry. Must run before RenderContext::new_frame, because RmlUi
  // generates font atlases lazily in here and those uploads block on a fence.
  auto update(this RmlView& self, glm::ivec2 surface_size_) -> void;
  // Appends the draw pass. Runs during graph building, unlike update.
  auto draw(this RmlView& self, RenderContext& context, vuk::Value<vuk::ImageAttachment> target)
    -> vuk::Value<vuk::ImageAttachment>;

  // Where this view lands on the window, in the same space as the mouse events.
  auto set_viewport(this RmlView& self, glm::ivec2 origin, glm::ivec2 size, bool keyboard_focused_) -> void;
  auto set_dpi_ratio(this const RmlView& self, f32 ratio) -> void;

  auto context(this const RmlView& self) -> Rml::Context* { return self.rml_context; }
  auto name(this const RmlView& self) -> std::string_view;

  // Read by RmlUI's router to hit test the cursor and scale it into context space.
  glm::vec2 viewport_origin = {};
  glm::vec2 viewport_size = {};
  glm::ivec2 surface_size = {};
  bool keyboard_focused = false;

private:
  Rml::Context* rml_context = nullptr;
  std::unique_ptr<RmlRenderer> renderer = {};
};
} // namespace ox
