#include <vuk/runtime/CommandBuffer.hpp>

#include "Memory/Stack.hpp"
#include "Render/RendererInstance.hpp"

namespace ox {
auto RendererInstance::draw_for_visbuffer(this RendererInstance& self, MainGeometryContext& context) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  auto encode_pass = vuk::make_pass(
    stack.format("vis encode {}", context.late ? "late" : "early"),
    [&descriptor_set = *context.bindless_set](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eIndirectRead) triangle_indirect,
      VUK_BA(vuk::eIndexRead) index_buffer,
      VUK_BA(vuk::eVertexRead) camera,
      VUK_BA(vuk::eVertexRead) meshes,
      VUK_BA(vuk::eVertexRead) mesh_instances,
      VUK_BA(vuk::eVertexRead) meshlet_instances,
      VUK_BA(vuk::eVertexRead) transforms,
      VUK_BA(vuk::eFragmentRead) materials,
      VUK_IA(vuk::eColorRW) visbuffer,
      VUK_IA(vuk::eDepthStencilRW) depth,
      VUK_IA(vuk::eFragmentRW) overdraw
    ) {
      cmd_list //
        .bind_graphics_pipeline("visbuffer_encode")
        .set_rasterization({.cullMode = vuk::CullModeFlagBits::eBack})
        .set_depth_stencil(
          {.depthTestEnable = true, .depthWriteEnable = true, .depthCompareOp = vuk::CompareOp::eGreaterOrEqual}
        )
        .set_color_blend(visbuffer, vuk::BlendPreset::eOff)
        .set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
        .set_viewport(0, vuk::Rect2D::framebuffer())
        .set_scissor(0, vuk::Rect2D::framebuffer())
        .bind_buffer(0, 0, camera)
        .bind_buffer(0, 1, meshes)
        .bind_buffer(0, 2, mesh_instances)
        .bind_buffer(0, 3, meshlet_instances)
        .bind_buffer(0, 4, transforms)
        .bind_buffer(0, 5, materials)
        .bind_image(0, 6, overdraw)
        .bind_index_buffer(index_buffer, vuk::IndexType::eUint32)
        .bind_persistent(1, descriptor_set)
        .draw_indexed_indirect(1, triangle_indirect);

      return std::make_tuple(
        index_buffer, //
        camera,
        meshes,
        mesh_instances,
        meshlet_instances,
        transforms,
        materials,
        visbuffer,
        depth,
        overdraw
      );
    }
  );

  std::tie(
    self.prepared_frame.reordered_indices_buffer,
    self.prepared_frame.camera_buffer,
    self.prepared_frame.meshes_buffer,
    self.prepared_frame.mesh_instances_buffer,
    self.prepared_frame.meshlet_instances_buffer,
    self.prepared_frame.transforms_buffer,
    self.prepared_frame.materials_buffer,
    context.visbuffer_attachment,
    context.depth_attachment,
    context.overdraw_attachment
  ) =
    encode_pass(
      std::move(context.draw_geometry_cmd_buffer),
      std::move(self.prepared_frame.reordered_indices_buffer),
      std::move(self.prepared_frame.camera_buffer),
      std::move(self.prepared_frame.meshes_buffer),
      std::move(self.prepared_frame.mesh_instances_buffer),
      std::move(self.prepared_frame.meshlet_instances_buffer),
      std::move(self.prepared_frame.transforms_buffer),
      std::move(self.prepared_frame.materials_buffer),
      std::move(context.visbuffer_attachment),
      std::move(context.depth_attachment),
      std::move(context.overdraw_attachment)
    );
}

auto RendererInstance::draw_for_shadowmap(
  this RendererInstance& self, ShadowGeometryContext& context, glm::mat4& projection_view, u32 cascade_index
) -> void {
  ZoneScoped;

  auto encode_pass = vuk::make_pass(
    "shadowmap draw",
    [projection_view](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eIndirectRead) triangle_indirect,
      VUK_BA(vuk::eIndexRead) index_buffer,
      VUK_BA(vuk::eVertexRead) meshes,
      VUK_BA(vuk::eVertexRead) mesh_instances,
      VUK_BA(vuk::eVertexRead) meshlet_instances,
      VUK_BA(vuk::eVertexRead) transforms,
      VUK_IA(vuk::eDepthStencilRW) shadowmap
    ) {
      cmd_list //
        .bind_graphics_pipeline("shadowmap_draw")
        .set_rasterization({.cullMode = vuk::CullModeFlagBits::eBack})
        .set_depth_stencil(
          {.depthTestEnable = true, .depthWriteEnable = true, .depthCompareOp = vuk::CompareOp::eGreaterOrEqual}
        )
        .set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
        .set_viewport(0, vuk::Rect2D::framebuffer())
        .set_scissor(0, vuk::Rect2D::framebuffer())
        .bind_buffer(0, 0, meshes)
        .bind_buffer(0, 1, mesh_instances)
        .bind_buffer(0, 2, meshlet_instances)
        .bind_buffer(0, 3, transforms)
        .bind_index_buffer(index_buffer, vuk::IndexType::eUint32)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, projection_view)
        .draw_indexed_indirect(1, triangle_indirect);

      return std::make_tuple(
        index_buffer, //
        meshes,
        mesh_instances,
        meshlet_instances,
        transforms
      );
    }
  );

  auto cascade_attachment = context.shadowmap_attachment.layer(cascade_index);
  std::tie(
    self.prepared_frame.reordered_indices_buffer,
    self.prepared_frame.meshes_buffer,
    self.prepared_frame.mesh_instances_buffer,
    self.prepared_frame.meshlet_instances_buffer,
    self.prepared_frame.transforms_buffer
  ) =
    encode_pass(
      std::move(context.draw_geometry_cmd_buffer),
      std::move(self.prepared_frame.reordered_indices_buffer),
      std::move(self.prepared_frame.meshes_buffer),
      std::move(self.prepared_frame.mesh_instances_buffer),
      std::move(self.prepared_frame.meshlet_instances_buffer),
      std::move(self.prepared_frame.transforms_buffer),
      std::move(cascade_attachment)
    );
}

auto RendererInstance::decode_visbuffer(this RendererInstance& self, MainGeometryContext& context) -> void {
  ZoneScoped;

  auto vis_decode_pass = vuk::make_pass(
    "vis decode",
    [&descriptor_set = *context.bindless_set](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eFragmentRead) camera,
      VUK_BA(vuk::eFragmentRead) meshlet_instances,
      VUK_BA(vuk::eFragmentRead) mesh_instances,
      VUK_BA(vuk::eFragmentRead) meshes,
      VUK_BA(vuk::eFragmentRead) transforms,
      VUK_BA(vuk::eFragmentRead) materials,
      VUK_IA(vuk::eFragmentSampled) visbuffer,
      VUK_IA(vuk::eColorRW) albedo,
      VUK_IA(vuk::eColorRW) normal,
      VUK_IA(vuk::eColorRW) emissive,
      VUK_IA(vuk::eColorRW) metallic_roughness_occlusion
    ) {
      cmd_list //
        .bind_graphics_pipeline("visbuffer_decode")
        .set_rasterization({.cullMode = vuk::CullModeFlagBits::eNone})
        .set_depth_stencil({})
        .set_color_blend(albedo, vuk::BlendPreset::eOff)
        .set_color_blend(normal, vuk::BlendPreset::eOff)
        .set_color_blend(emissive, vuk::BlendPreset::eOff)
        .set_color_blend(metallic_roughness_occlusion, vuk::BlendPreset::eOff)
        .set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
        .set_viewport(0, vuk::Rect2D::framebuffer())
        .set_scissor(0, vuk::Rect2D::framebuffer())
        .bind_persistent(1, descriptor_set)
        .bind_buffer(0, 0, camera)
        .bind_buffer(0, 1, meshlet_instances)
        .bind_buffer(0, 2, mesh_instances)
        .bind_buffer(0, 3, meshes)
        .bind_buffer(0, 4, transforms)
        .bind_buffer(0, 5, materials)
        .bind_image(0, 6, visbuffer)
        .draw(3, 1, 0, 1);

      return std::make_tuple(
        camera,
        meshlet_instances,
        mesh_instances,
        meshes,
        transforms,
        materials,
        visbuffer,
        albedo,
        normal,
        emissive,
        metallic_roughness_occlusion
      );
    }
  );

  std::tie(
    self.prepared_frame.camera_buffer,
    self.prepared_frame.meshlet_instances_buffer,
    self.prepared_frame.mesh_instances_buffer,
    self.prepared_frame.meshes_buffer,
    self.prepared_frame.transforms_buffer,
    self.prepared_frame.materials_buffer,
    context.visbuffer_attachment,
    context.albedo_attachment,
    context.normal_attachment,
    context.emissive_attachment,
    context.metallic_roughness_occlusion_attachment
  ) =
    vis_decode_pass(
      std::move(self.prepared_frame.camera_buffer),
      std::move(self.prepared_frame.meshlet_instances_buffer),
      std::move(self.prepared_frame.mesh_instances_buffer),
      std::move(self.prepared_frame.meshes_buffer),
      std::move(self.prepared_frame.transforms_buffer),
      std::move(self.prepared_frame.materials_buffer),
      std::move(context.visbuffer_attachment),
      std::move(context.albedo_attachment),
      std::move(context.normal_attachment),
      std::move(context.emissive_attachment),
      std::move(context.metallic_roughness_occlusion_attachment)
    );
}

} // namespace ox
