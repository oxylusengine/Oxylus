#include <vuk/runtime/CommandBuffer.hpp>

#include "Core/App.hpp"
#include "Render/DebugRenderer.hpp"
#include "Render/RendererInstance.hpp"
#include "Render/Utils/VukCommon.hpp"

namespace ox {
auto RendererInstance::apply_debug_view(this RendererInstance& self, DebugContext& context, vuk::Extent3D extent)
  -> vuk::Value<vuk::ImageAttachment> {
  ZoneScoped;

  auto debug_attachment = vuk::declare_ia(
    "debug",
    {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
     .extent = extent,
     .format = vuk::Format::eR16G16B16A16Sfloat,
     .sample_count = vuk::Samples::e1,
     .level_count = 1,
     .layer_count = 1}
  );
  debug_attachment = vuk::clear_image(std::move(debug_attachment), vuk::Black<f32>);

  if (self.prepared_frame.mesh_instance_count == 0) {
    // Prevent reading invalid geometry buffers
    switch (context.debug_view) {
      case GPU::DebugView::Triangles:
      case GPU::DebugView::Meshlets:
      case GPU::DebugView::Overdraw:
      case GPU::DebugView::Materials:
      case GPU::DebugView::MeshInstances:
      case GPU::DebugView::MeshLods     : {
        return debug_attachment;
      }
      default:;
    }
  }

  auto debug_view_pass = vuk::make_pass(
    "debug view pass",
    [debug_view = context.debug_view, overdraw_heatmap_scale = context.overdraw_heatmap_scale](
      vuk::CommandBuffer& cmd_list,
      VUK_IA(vuk::eColorWrite) dst,
      VUK_IA(vuk::eFragmentSampled) visbuffer,
      VUK_IA(vuk::eFragmentSampled) depth,
      VUK_IA(vuk::eFragmentSampled) overdraw,
      VUK_IA(vuk::eFragmentSampled) albedo,
      VUK_IA(vuk::eFragmentSampled) normal,
      VUK_IA(vuk::eFragmentSampled) emissive,
      VUK_IA(vuk::eFragmentSampled) metallic_roughness_occlusion,
      VUK_IA(vuk::eFragmentSampled) gtao,
      VUK_BA(vuk::eFragmentRead) visible_meshlet_instances_indices,
      VUK_BA(vuk::eFragmentRead) meshlet_instances,
      VUK_BA(vuk::eFragmentRead) mesh_instances,
      VUK_BA(vuk::eFragmentRead) meshes
    ) {
      cmd_list //
        .bind_graphics_pipeline("debug_view")
        .set_rasterization({})
        .set_color_blend(dst, vuk::BlendPreset::eOff)
        .set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
        .set_viewport(0, vuk::Rect2D::framebuffer())
        .set_scissor(0, vuk::Rect2D::framebuffer())
        .bind_sampler(0, 0, vuk::LinearSamplerRepeated)
        .bind_image(0, 1, visbuffer)
        .bind_image(0, 2, depth)
        .bind_image(0, 3, overdraw)
        .bind_image(0, 4, albedo)
        .bind_image(0, 5, normal)
        .bind_image(0, 6, emissive)
        .bind_image(0, 7, metallic_roughness_occlusion)
        .bind_image(0, 8, gtao)
        .bind_buffer(0, 9, visible_meshlet_instances_indices)
        .bind_buffer(0, 10, meshlet_instances)
        .bind_buffer(0, 11, mesh_instances)
        .bind_buffer(0, 12, meshes)
        .push_constants(
          vuk::ShaderStageFlagBits::eFragment,
          0,
          PushConstants(std::to_underlying(debug_view), overdraw_heatmap_scale)
        )
        .draw(3, 1, 0, 0);

      return dst;
    }
  );

  return debug_view_pass(
    std::move(debug_attachment),
    std::move(context.visbuffer_attachment),
    std::move(context.depth_attachment),
    std::move(context.overdraw_attachment),
    std::move(context.albedo_attachment),
    std::move(context.normal_attachment),
    std::move(context.emissive_attachment),
    std::move(context.metallic_roughness_occlusion_attachment),
    std::move(context.ambient_occlusion_attachment),
    std::move(self.prepared_frame.visible_meshlet_instances_indices_buffer),
    std::move(self.prepared_frame.meshlet_instances_buffer),
    std::move(self.prepared_frame.mesh_instances_buffer),
    std::move(self.prepared_frame.meshes_buffer)
  );
}

auto RendererInstance::draw_for_debug(
  this RendererInstance& self, DebugContext& context, vuk::Value<vuk::ImageAttachment>&& dst_attachment
) -> vuk::Value<vuk::ImageAttachment> {
  ZoneScoped;

  if (self.prepared_frame.line_index_count == 0 && self.prepared_frame.triangle_index_count == 0) {
    return std::move(dst_attachment);
  }

  auto debug_mesh_pass = vuk::make_pass(
    "debug mesh",
    [line_index_count = self.prepared_frame.line_index_count](
      vuk::CommandBuffer& cmd_list,
      VUK_IA(vuk::eColorWrite) dst,
      VUK_IA(vuk::eFragmentSampled) depth_img,
      VUK_BA(vuk::eMemoryRead) dbg_vtx,
      VUK_BA(vuk::eFragmentRead) camera
    ) {
      auto& dbg_index_buffer = *App::mod<DebugRenderer>().get_global_index_buffer();

      cmd_list.bind_graphics_pipeline("debug_mesh")
        .set_depth_stencil(
          vuk::PipelineDepthStencilStateCreateInfo{
            .depthTestEnable = false,
            .depthWriteEnable = false,
            .depthCompareOp = vuk::CompareOp::eGreaterOrEqual,
          }
        )
        .set_dynamic_state(vuk::DynamicStateFlagBits::eScissor | vuk::DynamicStateFlagBits::eViewport)
        .broadcast_color_blend(vuk::BlendPreset::eAlphaBlend)
        .set_rasterization(
          {.polygonMode = vuk::PolygonMode::eLine, .cullMode = vuk::CullModeFlagBits::eNone, .lineWidth = 3.f}
        )
        .set_primitive_topology(vuk::PrimitiveTopology::eLineList)
        .set_viewport(0, vuk::Rect2D::framebuffer())
        .set_scissor(0, vuk::Rect2D::framebuffer())
        .bind_vertex_buffer(0, dbg_vtx, 0, DebugRenderer::vertex_pack)
        .bind_index_buffer(dbg_index_buffer, vuk::IndexType::eUint32)
        .bind_buffer(0, 0, camera)
        .bind_image(0, 1, depth_img)
        .draw_indexed(line_index_count, 1, 0, 0, 0);

      return std::make_tuple(dst, camera, depth_img);
    }
  );

  std::tie(dst_attachment, self.prepared_frame.camera_buffer, context.depth_attachment) = debug_mesh_pass(
    dst_attachment,
    context.depth_attachment,
    self.prepared_frame.debug_renderer_verticies_buffer,
    self.prepared_frame.camera_buffer
  );

  return dst_attachment;
}
} // namespace ox
