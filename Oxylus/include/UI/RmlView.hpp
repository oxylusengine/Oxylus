#pragma once

#include <RmlUi/Core/Types.h>
#include <glm/vec2.hpp>
#include <memory>
#include <string_view>
#include <vir/Fwd.hpp>

#include "Core/Option.hpp"
#include "Core/Types.hpp"

namespace ox {
class RenderContext;
class RmlRenderer;
struct RmlCompositeVariables;

// One RmlUi context plus the renderer that draws it, owned by whatever owns the UI (one per Scene).
// Contexts cannot share a renderer, each one stages its own geometry every frame. Registers itself
// with the RmlUI module so input can be routed to it.
class RmlView {
public:
  explicit RmlView(std::string_view name);
  ~RmlView();

  RmlView(const RmlView&) = delete;
  auto operator=(const RmlView&) -> RmlView& = delete;

  // the context's one Update per frame, and the frame's geometry once there is a surface. must run before
  // RenderContext::new_frame, because RmlUi generates font atlases lazily in here and those uploads block on a fence
  auto update(this RmlView& self, glm::ivec2 surface_size_) -> void;
  // draws this frame's UI with a program of its own, after RenderContext::new_frame and before the owner's program
  // composites it
  auto render(this RmlView& self, RenderContext& context) -> void;
  // records the composite into the owner's program, which keeps it across frames and binds it before every run
  auto record(
    this RmlView& self,
    vir::Module& module,
    RenderContext& render_context,
    vir::ValueID target,
    vir::Extent3D extent,
    RmlCompositeVariables& variables
  ) -> vir::ValueID;
  auto bind(this const RmlView& self, vir::Program& program, const RmlCompositeVariables& variables) -> void;

  // Where this view lands on the window, in the same space as the mouse events.
  auto set_viewport(this RmlView& self, glm::ivec2 origin, glm::ivec2 size, bool keyboard_focused_) -> void;
  auto set_dpi_ratio(this RmlView& self, f32 ratio) -> void;
  auto clear_dpi_ratio_override(this RmlView& self) -> void;

  auto context(this const RmlView& self) -> Rml::Context* { return self.rml_context; }
  auto name(this const RmlView& self) -> std::string_view;

  // Read by RmlUI's router to hit test the cursor and scale it into context space.
  glm::vec2 viewport_origin = {};
  glm::vec2 viewport_size = {};
  glm::ivec2 surface_size = {};
  bool keyboard_focused = false;
  // off while something drawn over the view has the mouse
  bool accepts_mouse = true;
  // since `set_viewport`, counted by RmlUI. a view nothing shows any more keeps its rectangle
  u32 frames_unplaced = 0;

private:
  Rml::Context* rml_context = nullptr;
  std::unique_ptr<RmlRenderer> renderer = {};
  option<f32> dpi_ratio_override = nullopt;
  f32 applied_dpi_ratio = 0.0f;

  auto sync_dpi_ratio(this RmlView& self) -> void;
};
} // namespace ox
