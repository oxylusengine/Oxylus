#include <vir/RenderGraph.hpp>

#include "Render/DebugRenderer.hpp"
#include "Render/RenderContext.hpp"
#include "Render/RendererInstance.hpp"
#include "Render/Utils/PushConstants.hpp"
#include "Render/Utils/Samplers.hpp"

namespace ox {
// mirrored in debug_mesh.slang
static constexpr u32 DEBUG_FLAG_DEPTH_TESTED = 1_u32 << 0_u32;
static constexpr u32 DEBUG_FLAG_SHADED = 1_u32 << 1_u32;
static constexpr f32 DEBUG_HEATMAP_SCALE = 5.0f;

using PointSpotDebugPush = PushConstants<u32, glm::vec3>;

auto RendererInstance::bind_debug_variables(
  this const RendererInstance& self, vir::Program& program, const DebugVariables& variables
) -> void {
  ZoneScoped;

  program.set_bytes(variables.pointspot_push, PointSpotDebugPush(0_u32, self.light_grid_origin));

  auto vsm_context = self.frame_params.vsm;
  vsm_context.curr_clipmap_index = 0;
  vsm_context.depth_extent = {};
  vsm_context.z_length = 1.0f;
  program.set_bytes(variables.vsm_context, vsm_context);

  const auto& ranges = self.frame_data.debug_draw_ranges;
  for (auto index = 0_sz; index < variables.shape_counts.size(); index++) {
    const auto is_lines = index >= 2;
    const auto depth_tested = index % 2;
    const auto& range = (is_lines ? ranges.lines : ranges.triangles)[depth_tested];
    program.set(variables.shape_counts[index], range.count);
    program.set(variables.shape_offsets[index], range.offset);
  }
}

auto RendererInstance::apply_debug_view(
  this RendererInstance& self, vir::Module& module, FrameRecording& recording, DebugContext& context, vir::ValueID dst
) -> vir::ValueID {
  ZoneScoped;
  auto& prepared = recording.prepared;
  auto& values = recording.values;

  auto& render_context = *self.renderer.render_context;
  auto& variables = prepared.debug;
  variables.pointspot_push = module.declare_bytes_var("rmvsm pointspot debug push", sizeof(PointSpotDebugPush));
  variables.vsm_context = module.declare_bytes_var("rmvsm debug context", sizeof(GPU::VSMContext));

  const auto& shape = recording.shape;
  const auto debug_view = shape.debug_view;
  dst = module.clear(dst, vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 1.0f));

  if (!shape.has_meshes) {
    // the geometry buffers hold nothing worth reading
    switch (debug_view) {
      case GPU::DebugView::Triangles    :
      case GPU::DebugView::Meshlets     :
      case GPU::DebugView::Overdraw     :
      case GPU::DebugView::Materials    :
      case GPU::DebugView::MeshInstances:
      case GPU::DebugView::MeshLods     : {
        return dst;
      }
      default:;
    }
  }

  if (debug_view == GPU::DebugView::RMVSMPointSpot) {
    auto pointspot_pass = //
      module
        .begin_rendering(
          {{dst, vir::Access::ColorWrite},
           {context.depth_attachment, vir::Access::FragmentSampled},
           {context.vsm_pointspot_page_table_attachment, vir::Access::FragmentSampled},
           {values.camera, vir::Access::FragmentUniformRead},
           {context.pointspot_views_buffer, vir::Access::FragmentRead},
           {context.light_grid_buffer, vir::Access::FragmentRead}}
        )
        .with_name("rmvsm pointspot debug pass")
        .bind_graphics_pipeline(render_context.pipeline("rmvsm_debug_pointspot"));
    std::tie(
      dst,
      context.depth_attachment,
      context.vsm_pointspot_page_table_attachment,
      values.camera,
      context.pointspot_views_buffer,
      context.light_grid_buffer
    ) = std::tuple_cat( //
      bind_vsm_pointspot_spec_constants(pointspot_pass)
        .set_viewport(0, vir::Rect2D::framebuffer())
        .set_scissor(0, vir::Rect2D::framebuffer())
        .bind_buffer(0, 0, values.camera)
        .bind_buffer(0, 1, context.pointspot_views_buffer)
        .bind_buffer(0, 2, context.light_grid_buffer)
        .bind_image(0, 3, context.depth_attachment)
        .bind_image(0, 4, context.vsm_pointspot_page_table_attachment)
        .push_constants_from(prepared.debug.pointspot_push)
        .draw(3, 1)
        .end_rendering()
    );
    return dst;
  }

  if (debug_view == GPU::DebugView::RMVSM) {
    std::tie(
      dst,
      context.depth_attachment,
      context.normal_attachment,
      context.vsm_page_table_attachment,
      values.camera,
      context.vsm_clipmaps_buffer
    ) = std::tuple_cat( //
      module
        .begin_rendering(
          {{dst, vir::Access::ColorWrite},
           {context.depth_attachment, vir::Access::FragmentSampled},
           {context.normal_attachment, vir::Access::FragmentSampled},
           {context.vsm_page_table_attachment, vir::Access::FragmentSampled},
           {values.camera, vir::Access::FragmentUniformRead},
           {context.vsm_clipmaps_buffer, vir::Access::FragmentRead}}
        )
        .with_name("rmvsm debug pass")
        .bind_graphics_pipeline(render_context.pipeline("rmvsm_debug"))
        .set_viewport(0, vir::Rect2D::framebuffer())
        .set_scissor(0, vir::Rect2D::framebuffer())
        .bind_buffer(0, 0, values.camera)
        .bind_buffer(0, 1, context.vsm_clipmaps_buffer)
        .bind_image(0, 2, context.depth_attachment)
        .bind_image(0, 3, context.normal_attachment)
        .bind_image(0, 4, context.vsm_page_table_attachment)
        .push_constants_from(prepared.debug.vsm_context)
        .draw(3, 1)
        .end_rendering()
    );
    return dst;
  }

  std::tie(
    dst,
    context.visbuffer_attachment,
    context.depth_attachment,
    context.overdraw_attachment,
    context.albedo_attachment,
    context.normal_attachment,
    context.emissive_attachment,
    context.metallic_roughness_occlusion_attachment,
    context.ambient_occlusion_attachment,
    values.meshlet_instances,
    values.mesh_instances,
    values.meshes
  ) = std::tuple_cat( //
    module
      .begin_rendering(
        {{dst, vir::Access::ColorWrite},
         {context.visbuffer_attachment, vir::Access::FragmentSampled},
         {context.depth_attachment, vir::Access::FragmentSampled},
         {context.overdraw_attachment, vir::Access::FragmentSampled},
         {context.albedo_attachment, vir::Access::FragmentSampled},
         {context.normal_attachment, vir::Access::FragmentSampled},
         {context.emissive_attachment, vir::Access::FragmentSampled},
         {context.metallic_roughness_occlusion_attachment, vir::Access::FragmentSampled},
         {context.ambient_occlusion_attachment, vir::Access::FragmentSampled},
         {values.meshlet_instances, vir::Access::FragmentRead},
         {values.mesh_instances, vir::Access::FragmentRead},
         {values.meshes, vir::Access::FragmentRead}}
      )
      .with_name("debug view pass")
      .bind_graphics_pipeline(render_context.pipeline("debug_view"))
      .set_viewport(0, vir::Rect2D::framebuffer())
      .set_scissor(0, vir::Rect2D::framebuffer())
      .bind_sampler(0, 0, render_context.cached_sampler_handle(LinearSamplerRepeated))
      .bind_image(0, 1, context.visbuffer_attachment)
      .bind_image(0, 2, context.depth_attachment)
      .bind_image(0, 3, context.overdraw_attachment)
      .bind_image(0, 4, context.albedo_attachment)
      .bind_image(0, 5, context.normal_attachment)
      .bind_image(0, 6, context.emissive_attachment)
      .bind_image(0, 7, context.metallic_roughness_occlusion_attachment)
      .bind_image(0, 8, context.ambient_occlusion_attachment)
      .bind_buffer(0, 9, values.meshlet_instances)
      .bind_buffer(0, 10, values.mesh_instances)
      .bind_buffer(0, 11, values.meshes)
      .push_constants(PushConstants(std::to_underlying(debug_view), DEBUG_HEATMAP_SCALE))
      .draw(3, 1)
      .end_rendering()
  );
  return dst;
}

auto RendererInstance::draw_debug_shapes(
  this RendererInstance& self, vir::Module& module, FrameRecording& recording, vir::ValueID depth, vir::ValueID dst
) -> vir::ValueID {
  ZoneScoped;
  auto& prepared = recording.prepared;
  auto& values = recording.values;

  auto& render_context = *self.renderer.render_context;
  auto& variables = prepared.debug;
  variables.vertices = module.declare_buffer_var("debug vertices", vir::Access::None);

  for (auto index = 0_sz; index < variables.shape_counts.size(); index++) {
    variables.shape_counts[index] = module.declare_u32_var("debug shape count", 0);
    variables.shape_offsets[index] = module.declare_u32_var("debug shape offset", 0);
  }

  auto vertices = prepared.debug.vertices;

  auto pass = //
    module
      .begin_rendering(
        {{dst, vir::Access::ColorRW},
         {depth, vir::Access::FragmentSampled},
         {vertices, vir::Access::AttributeRead},
         {values.camera, vir::Access::VertexUniformRead | vir::Access::FragmentUniformRead}}
      )
      .with_name("debug shapes")
      .broadcast_color_blend(vir::BlendPreset::AlphaBlend)
      .set_viewport(0, vir::Rect2D::framebuffer())
      .set_scissor(0, vir::Rect2D::framebuffer())
      .bind_vertex_buffer(0, vertices)
      .bind_buffer(0, 0, values.camera)
      .bind_image(0, 1, depth);

  // the ranges move every frame, and an empty one draws nothing
  const auto draw_ranges = [&](bool is_lines, u32 flags) {
    for (u32 depth_tested = 0; depth_tested < 2; depth_tested++) {
      const auto count = prepared.debug.shape_counts[(is_lines ? 2 : 0) + depth_tested];
      const auto offset = prepared.debug.shape_offsets[(is_lines ? 2 : 0) + depth_tested];
      module.push_constants(PushConstants(flags | (depth_tested != 0 ? DEBUG_FLAG_DEPTH_TESTED : 0_u32)))
        .draw_range(count, 1, offset, 0);
    }
  };

  module.bind_graphics_pipeline(render_context.pipeline("debug_mesh"))
    .set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
    .set_rasterization({.cull_mode = VK_CULL_MODE_BACK_BIT});
  draw_ranges(false, DEBUG_FLAG_SHADED);

  module.bind_graphics_pipeline(render_context.pipeline("debug_mesh"))
    .set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_LINE_LIST)
    .set_rasterization({.cull_mode = VK_CULL_MODE_NONE});
  draw_ranges(true, 0_u32);

  std::tie(dst, std::ignore, std::ignore, values.camera) = std::tuple_cat(pass.end_rendering());
  return dst;
}
} // namespace ox
