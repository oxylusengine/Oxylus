#include <utility>
#include <vir/RenderGraph.hpp>
#include <vir/Types.hpp>

#include "Render/RenderContext.hpp"
#include "Render/RendererInstance.hpp"

namespace ox {
// the pass writes the fog's inscatter and what it lets through, so this is `inscatter + scene * transmittance`
constexpr static auto HEIGHT_FOG_BLEND = vir::ColorBlendAttachmentState{
  .is_blend_enabled = true,
  .src_color_blend_factor = VK_BLEND_FACTOR_ONE,
  .dst_color_blend_factor = VK_BLEND_FACTOR_SRC_ALPHA,
  .color_blend_op = VK_BLEND_OP_ADD,
  .src_alpha_blend_factor = VK_BLEND_FACTOR_ZERO,
  .dst_alpha_blend_factor = VK_BLEND_FACTOR_ONE,
  .alpha_blend_op = VK_BLEND_OP_ADD,
};

auto RendererInstance::apply_height_fog(
  this RendererInstance& self, vir::Module& module, FrameRecording& recording, HeightFogContext& context
) -> void {
  ZoneScoped;
  auto& values = recording.values;
  auto& render_context = *self.renderer.render_context;

  std::tie(values.camera, context.depth_attachment, context.final_attachment) = std::tuple_cat(
    module
      .begin_rendering(
        {{values.camera, vir::Access::FragmentUniformRead},
         {context.depth_attachment, vir::Access::FragmentSampled},
         {context.final_attachment, vir::Access::ColorRW}}
      )
      .with_name("height fog")
      .bind_graphics_pipeline(render_context.pipeline("height_fog"))
      .set_color_blend(0, HEIGHT_FOG_BLEND)
      .set_viewport(0, vir::Rect2D::framebuffer())
      .set_scissor(0, vir::Rect2D::framebuffer())
      .bind_buffer(0, 0, values.camera)
      .bind_image(0, 1, context.depth_attachment)
      .specialize_constant(0, std::to_underlying(recording.shape.scene_flags))
      .draw(3, 1)
      .end_rendering()
  );
}
} // namespace ox
