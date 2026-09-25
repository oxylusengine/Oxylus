#include <vuk/runtime/CommandBuffer.hpp>

#include "Core/App.hpp"
#include "Render/DebugRenderer.hpp"
#include "Render/RendererInstance.hpp"
#include "Render/Utils/VukCommon.hpp"

namespace ox {
static const auto DEBUG_VERTEX_PACK = vuk::Packed{
  vuk::Format::eR32G32B32Sfloat, // position
  vuk::Format::eR32Uint,         // rgba8 color
};

// mirrored in debug_mesh.slang
static constexpr u32 DEBUG_FLAG_DEPTH_TESTED = 1_u32 << 0_u32;
static constexpr u32 DEBUG_FLAG_SHADED = 1_u32 << 1_u32;

auto RendererInstance::apply_debug_view(
  this RendererInstance& self, DebugContext& context, vuk::Value<vuk::ImageAttachment>&& dst_attachment
) -> vuk::Value<vuk::ImageAttachment> {
  ZoneScoped;

  auto vsm_ctx = GPU::VSMContext{
    .page_size = RMVSMContext::PAGE_SIZE,
    .page_table_size = RMVSMContext::DIRECTIONAL_PAGE_TABLE_SIZE,
    .physcial_page_table_size = RMVSMContext::PHYSICAL_PAGE_TABLE_SIZE,
    .clipmap_count = RMVSMContext::MAX_DIRECTIONAL_CLIPMAP_COUNT,
    .first_clipmap_width = self.first_clipmap_width,
    .clipmap_selection_bias = self.clipmap_selection_bias,
    .virtual_extent = RMVSMContext::DIRECTIONAL_IMAGE_RESOLUTION,
    .z_length = 1.0f,
    .directional_light_dir = self.directional_light.direction,
  };

  auto debug_attachment = vuk::clear_image(std::move(dst_attachment), vuk::Black<f32>);

  if (self.prepared_frame.mesh_instance_count == 0) {
    // Prevent reading invalid geometry buffers
    switch (context.debug_view) {
      case GPU::DebugView::Triangles    :
      case GPU::DebugView::Meshlets     :
      case GPU::DebugView::Overdraw     :
      case GPU::DebugView::Materials    :
      case GPU::DebugView::MeshInstances:
      case GPU::DebugView::MeshLods     : {
        return debug_attachment;
      }
      default:;
    }
  }

  if (context.debug_view == GPU::DebugView::RMVSMPointSpot) {
    auto debug_view_pass = vuk::make_pass(
      "rmvsm pointspot debug pass",
      [light_grid_origin = context.light_grid_origin](
        vuk::CommandBuffer& cmd_list,
        VUK_IA(vuk::eColorWrite) dst,
        VUK_IA(vuk::eFragmentSampled) depth,
        VUK_IA(vuk::eFragmentSampled) page_table,
        VUK_BA(vuk::eFragmentUniformRead) camera,
        VUK_BA(vuk::eFragmentRead) views,
        VUK_BA(vuk::eFragmentRead) light_grid
      ) {
        cmd_list.bind_graphics_pipeline("rmvsm_debug_pointspot");
        bind_vsm_pointspot_spec_constants(cmd_list)
          .set_rasterization({})
          .set_color_blend(dst, vuk::BlendPreset::eOff)
          .set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
          .set_viewport(0, vuk::Rect2D::framebuffer())
          .set_scissor(0, vuk::Rect2D::framebuffer())
          .bind_buffer(0, 0, camera)
          .bind_buffer(0, 1, views)
          .bind_buffer(0, 2, light_grid)
          .bind_image(0, 3, depth)
          .bind_image(0, 4, page_table)
          .push_constants(vuk::ShaderStageFlagBits::eFragment, 0, PushConstants(0_u32, light_grid_origin))
          .draw(3, 1, 0, 0);

        return dst;
      }
    );

    return debug_view_pass(
      std::move(debug_attachment),
      std::move(context.depth_attachment),
      std::move(context.vsm_pointspot_page_table_attachment),
      std::move(self.prepared_frame.camera_buffer),
      std::move(context.pointspot_views_buffer),
      std::move(context.light_grid_buffer)
    );
  }

  if (context.debug_view != GPU::DebugView::RMVSM) {
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
          .bind_buffer(0, 9, meshlet_instances)
          .bind_buffer(0, 10, mesh_instances)
          .bind_buffer(0, 11, meshes)
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
      std::move(self.prepared_frame.meshlet_instances_buffer),
      std::move(self.prepared_frame.mesh_instances_buffer),
      std::move(self.prepared_frame.meshes_buffer)
    );
  } else {
    auto debug_view_pass = vuk::make_pass(
      "debug view pass",
      [vsm_ctx](
        vuk::CommandBuffer& cmd_list,
        VUK_IA(vuk::eColorWrite) dst,
        VUK_IA(vuk::eFragmentSampled) depth,
        VUK_IA(vuk::eFragmentSampled) normal,
        VUK_IA(vuk::eFragmentSampled) page_table,
        VUK_BA(vuk::eFragmentUniformRead) camera,
        VUK_BA(vuk::eFragmentRead) clipmaps
      ) {
        cmd_list //
          .bind_graphics_pipeline("rmvsm_debug")
          .set_rasterization({})
          .set_color_blend(dst, vuk::BlendPreset::eOff)
          .set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
          .set_viewport(0, vuk::Rect2D::framebuffer())
          .set_scissor(0, vuk::Rect2D::framebuffer())
          .bind_buffer(0, 0, camera)
          .bind_buffer(0, 1, clipmaps)
          .bind_image(0, 2, depth)
          .bind_image(0, 3, normal)
          .bind_image(0, 4, page_table)
          .push_constants(vuk::ShaderStageFlagBits::eFragment, 0, vsm_ctx)
          .draw(3, 1, 0, 0);

        return dst;
      }
    );

    return debug_view_pass(
      std::move(debug_attachment),
      std::move(context.depth_attachment),
      std::move(context.normal_attachment),
      std::move(context.vsm_page_table_attachment),
      self.prepared_frame.camera_buffer,
      std::move(context.vsm_clipmaps_buffer)
    );
  }
}

auto RendererInstance::draw_debug_shapes(
  this RendererInstance& self,
  vuk::Value<vuk::ImageAttachment>&& depth_attachment,
  vuk::Value<vuk::ImageAttachment>&& dst_attachment
) -> vuk::Value<vuk::ImageAttachment> {
  ZoneScoped;

  const auto ranges = self.prepared_frame.debug_draw_ranges;
  if (ranges.empty()) {
    return std::move(dst_attachment);
  }

  auto debug_shapes_pass = vuk::make_pass(
    "debug shapes",
    [ranges](
      vuk::CommandBuffer& cmd_list,
      VUK_IA(vuk::eColorWrite) dst,
      VUK_IA(vuk::eFragmentSampled) depth_img,
      VUK_BA(vuk::eAttributeRead) dbg_vtx,
      VUK_BA(vuk::eVertexUniformRead | vuk::eFragmentUniformRead) camera
    ) {
      cmd_list.set_dynamic_state(vuk::DynamicStateFlagBits::eScissor | vuk::DynamicStateFlagBits::eViewport)
        .broadcast_color_blend(vuk::BlendPreset::eAlphaBlend)
        .set_viewport(0, vuk::Rect2D::framebuffer())
        .set_scissor(0, vuk::Rect2D::framebuffer())
        .bind_vertex_buffer(0, dbg_vtx, 0, DEBUG_VERTEX_PACK)
        .bind_buffer(0, 0, camera)
        .bind_image(0, 1, depth_img);

      const auto draw_ranges = [&](std::span<const DebugRenderer::VertexRange> draws, const u32 flags) {
        for (u32 depth_tested = 0; depth_tested < draws.size(); depth_tested++) {
          const auto& range = draws[depth_tested];
          if (range.count == 0)
            continue;

          const auto depth_flag = depth_tested != 0 ? DEBUG_FLAG_DEPTH_TESTED : 0_u32;
          cmd_list.push_constants(vuk::ShaderStageFlagBits::eFragment, 0, PushConstants(flags | depth_flag))
            .draw(range.count, 1, range.offset, 0);
        }
      };

      cmd_list.bind_graphics_pipeline("debug_mesh")
        .set_primitive_topology(vuk::PrimitiveTopology::eTriangleList)
        .set_rasterization({.cullMode = vuk::CullModeFlagBits::eBack});
      draw_ranges(ranges.triangles, DEBUG_FLAG_SHADED);

      cmd_list.bind_graphics_pipeline("debug_mesh")
        .set_primitive_topology(vuk::PrimitiveTopology::eLineList)
        .set_rasterization({.cullMode = vuk::CullModeFlagBits::eNone});
      draw_ranges(ranges.lines, 0_u32);

      return std::make_tuple(dst, camera, depth_img);
    }
  );

  std::tie(dst_attachment, self.prepared_frame.camera_buffer, depth_attachment) = debug_shapes_pass(
    std::move(dst_attachment),
    std::move(depth_attachment),
    std::move(self.prepared_frame.debug_renderer_vertices_buffer),
    std::move(self.prepared_frame.camera_buffer)
  );

  return dst_attachment;
}
} // namespace ox
