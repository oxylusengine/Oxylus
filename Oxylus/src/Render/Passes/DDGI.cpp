#include <vuk/runtime/CommandBuffer.hpp>

#include "Render/RendererInstance.hpp"
#include "Render/Utils/VukCommon.hpp"

namespace ox {
auto RendererInstance::draw_ddgi_probes(
  this RendererInstance& self, DDGIDebugContext& context, vuk::Value<vuk::ImageAttachment>&& dst_attachment
) -> vuk::Value<vuk::ImageAttachment> {
  ZoneScoped;

  auto probe_counts = ankerl::svector<u32, 4>{};
  for (const auto& volume : self.probe_volumes) {
    probe_counts.emplace_back(volume.probe_count);
  }

  auto probe_pass = vuk::make_pass(
    "ddgi debug probes",
    [probe_counts, probe_radius = context.probe_radius](
      vuk::CommandBuffer& cmd_list,
      VUK_IA(vuk::eColorWrite) dst,
      VUK_IA(vuk::eDepthStencilRW) depth,
      VUK_BA(vuk::eVertexRead) camera,
      VUK_BA(vuk::eVertexRead) probe_volumes
    ) {
      cmd_list //
        .bind_graphics_pipeline("ddgi_debug_probes")
        .set_rasterization({.cullMode = vuk::CullModeFlagBits::eNone})
        .set_depth_stencil(
          {.depthTestEnable = true, .depthWriteEnable = true, .depthCompareOp = vuk::CompareOp::eGreaterOrEqual}
        )
        .set_color_blend(dst, vuk::BlendPreset::eOff)
        .set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
        .set_viewport(0, vuk::Rect2D::framebuffer())
        .set_scissor(0, vuk::Rect2D::framebuffer())
        .bind_buffer(0, 0, camera)
        .bind_buffer(0, 1, probe_volumes);

      for (u32 volume_index = 0; volume_index < static_cast<u32>(probe_counts.size()); volume_index++) {
        cmd_list //
          .push_constants(vuk::ShaderStageFlagBits::eVertex, 0, PushConstants(volume_index, probe_radius))
          .draw(GPU::DDGI_DEBUG_SPHERE_VERTEX_COUNT, probe_counts[volume_index], 0, 0);
      }

      return std::make_tuple(dst, depth, camera, probe_volumes);
    }
  );

  std::tie(dst_attachment, context.depth_attachment, self.prepared_frame.camera_buffer, context.probe_volumes_buffer) =
    probe_pass(
      std::move(dst_attachment),
      std::move(context.depth_attachment),
      std::move(self.prepared_frame.camera_buffer),
      std::move(context.probe_volumes_buffer)
    );

  return dst_attachment;
}
} // namespace ox
