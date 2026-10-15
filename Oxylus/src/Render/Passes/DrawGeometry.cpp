#include "Core/Enum.hpp"
#include "Memory/Stack.hpp"
#include "Render/Graph/RenderGraph.hpp"
#include "Render/Graph/Types.hpp"
#include "Render/RenderContext.hpp"
#include "Render/RendererInstance.hpp"

namespace ox {
static constexpr auto VISBUFFER_RASTERIZATION = vir::RasterizationState{.cull_mode = VK_CULL_MODE_BACK_BIT};
static constexpr auto VISBUFFER_DEPTH = vir::DepthState{
  .is_test_enabled = true,
  .is_write_enabled = true,
  .compare_op = VK_COMPARE_OP_GREATER_OR_EQUAL,
};

auto RendererInstance::draw_for_visbuffer_ms(
  this RendererInstance& self, vir::Module& module, FrameRecording& recording, MainGeometryContext& context
) -> void {
  ZoneScoped;
  auto& prepared = recording.prepared;
  auto& values = recording.values;
  memory::ScopedStack stack;

  if (prepared.cull.camera == vir::ValueID::Invalid) {
    prepared.cull.camera = module.declare_bytes_var("cull camera", sizeof(GPU::CullCamera));
  }

  auto& render_context = *self.renderer.render_context;
  const auto is_late = static_cast<bool>(context.cull_flags & GPU::CullFlag::LatePass);
  const auto name = stack.format("vis encode ms {}", is_late ? "late" : "early");
  static constexpr auto scene_access = vir::Access::TaskRead | vir::Access::MeshRead;

  module
    .begin_rendering(
      {{context.draw_geometry_cmd_buffer, vir::Access::IndirectRead},
       {context.hiz_attachment, vir::Access::TaskSampled},
       {values.meshes, scene_access},
       {values.mesh_instances, scene_access},
       {values.meshlet_instances, scene_access},
       {values.transforms_world, scene_access},
       {context.visibility_buffer, vir::Access::TaskRead},
       {values.meshlet_instance_visibility_mask, vir::Access::TaskRW},
       {values.materials, vir::Access::FragmentRead},
       {context.visbuffer_attachment, vir::Access::ColorRW},
       {context.depth_attachment, vir::Access::DepthStencilRW},
       {context.overdraw_attachment, vir::Access::FragmentRW}}
    )
    .with_name(name)
    .bind_graphics_pipeline(render_context.pipeline("visbuffer_encode_ms"))
    .set_rasterization(VISBUFFER_RASTERIZATION)
    .set_depth(VISBUFFER_DEPTH)
    .set_viewport(0, vir::Rect2D::framebuffer())
    .set_scissor(0, vir::Rect2D::framebuffer())
    .bind_image(0, 0, context.hiz_attachment)
    .bind_buffer(0, 1, values.meshes)
    .bind_buffer(0, 2, values.mesh_instances)
    .bind_buffer(0, 3, values.meshlet_instances)
    .bind_buffer(0, 4, values.transforms_world)
    .bind_buffer(0, 5, context.visibility_buffer)
    .bind_buffer(0, 6, values.meshlet_instance_visibility_mask)
    .bind_buffer(0, 7, values.materials)
    .bind_image(0, 8, context.overdraw_attachment)
    .specialize_constant(0, std::to_underlying(context.cull_flags))
    .specialize_constant(1, context.draw_overdraw)
    .push_constants_from(prepared.cull.camera)
    .draw_mesh_tasks_indirect(context.draw_geometry_cmd_buffer, 1);
  std::tie(
    std::ignore,
    context.hiz_attachment,
    values.meshes,
    values.mesh_instances,
    values.meshlet_instances,
    values.transforms_world,
    context.visibility_buffer,
    values.meshlet_instance_visibility_mask,
    values.materials,
    context.visbuffer_attachment,
    context.depth_attachment,
    context.overdraw_attachment
  ) = std::tuple_cat(module.end_rendering<12>());
}

auto RendererInstance::draw_for_visbuffer(
  this RendererInstance& self, vir::Module& module, FrameRecording& recording, MainGeometryContext& context
) -> void {
  ZoneScoped;
  auto& prepared = recording.prepared;
  auto& values = recording.values;

  if (recording.shape.use_mesh_shaders) {
    self.draw_for_visbuffer_ms(module, recording, context);
    return;
  }

  auto& render_context = *self.renderer.render_context;
  module
    .begin_rendering(
      {{context.draw_geometry_cmd_buffer, vir::Access::IndirectRead},
       {values.reordered_indices, vir::Access::IndexRead},
       {values.camera, vir::Access::VertexUniformRead | vir::Access::FragmentUniformRead},
       {values.meshes, vir::Access::VertexRead},
       {values.mesh_instances, vir::Access::VertexRead},
       {values.meshlet_instances, vir::Access::VertexRead},
       {values.transforms_world, vir::Access::VertexRead},
       {values.materials, vir::Access::FragmentRead},
       {context.visbuffer_attachment, vir::Access::ColorRW},
       {context.depth_attachment, vir::Access::DepthStencilRW},
       {context.overdraw_attachment, vir::Access::FragmentRW}}
    )
    .with_name("vis encode")
    .bind_graphics_pipeline(render_context.pipeline("visbuffer_encode"))
    .set_rasterization(VISBUFFER_RASTERIZATION)
    .set_depth(VISBUFFER_DEPTH)
    .set_viewport(0, vir::Rect2D::framebuffer())
    .set_scissor(0, vir::Rect2D::framebuffer())
    .bind_buffer(0, 0, values.camera)
    .bind_buffer(0, 1, values.meshes)
    .bind_buffer(0, 2, values.mesh_instances)
    .bind_buffer(0, 3, values.meshlet_instances)
    .bind_buffer(0, 4, values.transforms_world)
    .bind_buffer(0, 5, values.materials)
    .bind_image(0, 6, context.overdraw_attachment)
    .specialize_constant(0, context.draw_overdraw)
    .bind_index_buffer(values.reordered_indices, VK_INDEX_TYPE_UINT32)
    .draw_indexed_indirect(context.draw_geometry_cmd_buffer, 1);
  std::tie(
    std::ignore,
    values.reordered_indices,
    values.camera,
    values.meshes,
    values.mesh_instances,
    values.meshlet_instances,
    values.transforms_world,
    values.materials,
    context.visbuffer_attachment,
    context.depth_attachment,
    context.overdraw_attachment
  ) = std::tuple_cat(module.end_rendering<11>());
}

auto RendererInstance::decode_visbuffer(
  this RendererInstance& self, vir::Module& module, FrameRecording& recording, MainGeometryContext& context
) -> void {
  ZoneScoped;
  auto& values = recording.values;

  auto& render_context = *self.renderer.render_context;
  module
    .begin_rendering(
      {{values.camera, vir::Access::FragmentUniformRead},
       {values.meshlet_instances, vir::Access::FragmentRead},
       {values.mesh_instances, vir::Access::FragmentRead},
       {values.meshes, vir::Access::FragmentRead},
       {values.transforms_world, vir::Access::FragmentRead},
       {values.transforms_previous, vir::Access::FragmentRead},
       {values.materials, vir::Access::FragmentRead},
       {context.visbuffer_attachment, vir::Access::FragmentSampled},
       {context.albedo_attachment, vir::Access::ColorRW},
       {context.normal_attachment, vir::Access::ColorRW},
       {context.emissive_attachment, vir::Access::ColorRW},
       {context.metallic_roughness_occlusion_attachment, vir::Access::ColorRW},
       {context.velocity_attachment, vir::Access::ColorRW}}
    )
    .with_name("vis decode")
    .bind_graphics_pipeline(render_context.pipeline("visbuffer_decode"))
    .set_viewport(0, vir::Rect2D::framebuffer())
    .set_scissor(0, vir::Rect2D::framebuffer())
    .bind_buffer(0, 0, values.camera)
    .bind_buffer(0, 1, values.meshlet_instances)
    .bind_buffer(0, 2, values.mesh_instances)
    .bind_buffer(0, 3, values.meshes)
    .bind_buffer(0, 4, values.transforms_world)
    .bind_buffer(0, 5, values.transforms_previous)
    .bind_buffer(0, 6, values.materials)
    .bind_image(0, 7, context.visbuffer_attachment)
    .draw_range(3, 1, 0, 1);
  std::tie(
    values.camera,
    values.meshlet_instances,
    values.mesh_instances,
    values.meshes,
    values.transforms_world,
    values.transforms_previous,
    values.materials,
    context.visbuffer_attachment,
    context.albedo_attachment,
    context.normal_attachment,
    context.emissive_attachment,
    context.metallic_roughness_occlusion_attachment,
    context.velocity_attachment
  ) = std::tuple_cat(module.end_rendering<13>());
}
} // namespace ox
