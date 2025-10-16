#include "Render/RendererPipeline.hpp"
#include "Render/Utils/VukCommon.hpp"

namespace ox {
auto forward_2d_pass(
  Renderer::RenderQueue2D& rq2d,
  vuk::PersistentDescriptorSet& descriptor_set,
  vuk::Value<vuk::ImageAttachment>& final_attachment,
  vuk::Value<vuk::ImageAttachment>& depth_attachment,
  vuk::Value<vuk::Buffer>& vertex_buffer_2d,
  vuk::Value<vuk::Buffer>& materials_buffer,
  vuk::Value<vuk::Buffer>& camera_buffer,
  vuk::Value<vuk::Buffer>& transforms_buffer
) -> void {
  auto pass = vuk::make_pass(
    "2d_forward_pass",
    [rq2d, &descriptor_set](
      vuk::CommandBuffer& command_buffer,
      VUK_IA(vuk::eColorWrite) target,
      VUK_IA(vuk::eDepthStencilRW) depth,
      VUK_BA(vuk::eAttributeRead) vertex_buffer,
      VUK_BA(vuk::eVertexRead) materials,
      VUK_BA(vuk::eVertexRead) camera,
      VUK_BA(vuk::eVertexRead) transforms_
    ) {
      const auto vertex_pack_2d = vuk::Packed{
        vuk::Format::eR32Uint, // 4 material_id
        vuk::Format::eR32Uint, // 4 flags
        vuk::Format::eR32Uint, // 4 transforms_id
      };

      for (const auto& batch : rq2d.batches) {
        if (batch.count < 1)
          continue;

        command_buffer.bind_graphics_pipeline(batch.pipeline_name)
          .set_depth_stencil(
            vuk::PipelineDepthStencilStateCreateInfo{
              .depthTestEnable = true,
              .depthWriteEnable = true,
              .depthCompareOp = vuk::CompareOp::eGreaterOrEqual,
            }
          )
          .set_dynamic_state(vuk::DynamicStateFlagBits::eScissor | vuk::DynamicStateFlagBits::eViewport)
          .set_viewport(0, vuk::Rect2D::framebuffer())
          .set_scissor(0, vuk::Rect2D::framebuffer())
          .broadcast_color_blend(vuk::BlendPreset::eAlphaBlend)
          .set_rasterization({.cullMode = vuk::CullModeFlagBits::eNone})
          .bind_vertex_buffer(0, vertex_buffer, 0, vertex_pack_2d, vuk::VertexInputRate::eInstance)
          .push_constants(
            vuk::ShaderStageFlagBits::eVertex | vuk::ShaderStageFlagBits::eFragment,
            0,
            PushConstants(materials->device_address, camera->device_address, transforms_->device_address)
          )
          .bind_persistent(1, descriptor_set)
          .draw(6, batch.count, 0, batch.offset);
      }

      return std::make_tuple(target, depth, camera, vertex_buffer, materials, transforms_);
    }
  );

  std::tie(final_attachment, depth_attachment, camera_buffer, vertex_buffer_2d, materials_buffer, transforms_buffer) =
    pass(
      std::move(final_attachment),
      std::move(depth_attachment),
      std::move(vertex_buffer_2d),
      std::move(materials_buffer),
      std::move(camera_buffer),
      std::move(transforms_buffer)
    );
}
} // namespace ox
