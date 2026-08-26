#include "EditorGrid.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <tuple>
#include <vuk/vsl/Core.hpp>

#include "Core/App.hpp"
#include "Render/Renderer.hpp"
#include "Render/RendererInstance.hpp"
#include "Render/Utils/VukCommon.hpp"

namespace ox {
auto add_editor_grid_stage(RendererInstance& renderer_instance, const f32 grid_distance) -> void {
  ZoneScoped;

  renderer_instance.add_stage_after(
    RenderStage::PostProcessing,
    "grid_stage",
    [grid_distance](RenderStageContext& ctx) {
      auto result_attachment = ctx.get_image_resource("result_attachment");
      auto depth_attachment = ctx.get_image_resource("depth_attachment");
      auto camera_buffer = ctx.get_buffer_resource("camera_buffer");

      auto grid_pass = vuk::make_pass(
        "grid_pass",
        [grid_distance](
          vuk::CommandBuffer& cmd_list,
          VUK_IA(vuk::eColorWrite) out,
          VUK_IA(vuk::eDepthStencilRead) depth,
          VUK_BA(vuk::eAttributeRead) vertex_buffer,
          VUK_BA(vuk::eIndexRead) index_buffer,
          VUK_BA(vuk::eVertexRead) camera
        ) {
          const auto vertex_pack = vuk::Packed{
            vuk::Format::eR32G32B32Sfloat,
            vuk::Format::eR32G32Sfloat,
          };

          const auto position = glm::vec3(0.0f);
          const auto rotation = glm::vec3(glm::radians(90.0f), 0.0f, 0.0f);
          const auto scale = glm::floor(glm::vec3(grid_distance / 2.0f)) * 2.0f;
          const auto grid_transform = glm::translate(glm::mat4(1.0f), position) *
                                      glm::toMat4(glm::quat(rotation)) * glm::scale(glm::mat4(1.0f), scale);

          cmd_list.bind_graphics_pipeline("grid")
            .set_dynamic_state(vuk::DynamicStateFlagBits::eScissor | vuk::DynamicStateFlagBits::eViewport)
            .set_viewport(0, vuk::Rect2D::framebuffer())
            .set_scissor(0, vuk::Rect2D::framebuffer())
            .set_depth_stencil(
              {.depthTestEnable = true, .depthWriteEnable = false, .depthCompareOp = vuk::CompareOp::eGreaterOrEqual}
            )
            .broadcast_color_blend(vuk::BlendPreset::eAlphaBlend)
            .set_rasterization({.cullMode = vuk::CullModeFlagBits::eNone})
            .bind_buffer(0, 0, camera)
            .bind_vertex_buffer(0, vertex_buffer, 0, vertex_pack)
            .bind_index_buffer(index_buffer, vuk::IndexType::eUint32)
            .push_constants(
              vuk::ShaderStageFlagBits::eVertex | vuk::ShaderStageFlagBits::eFragment,
              0,
              PushConstants(grid_transform, scale.x)
            )
            .draw_indexed(6, 1, 0, 0, 0);

          return std::make_tuple(out, depth, camera);
        }
      );

      auto grid_attachment = vuk::declare_ia(
        "grid_attachment",
        {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
         .sample_count = vuk::Samples::e1}
      );
      grid_attachment.same_format_as(result_attachment);
      grid_attachment.same_shape_as(result_attachment);
      grid_attachment = vuk::clear_image(grid_attachment, vuk::Black<f32>);

      auto& renderer = App::mod<Renderer>();
      auto grid_vertex_buffer = vuk::acquire_buf("grid vertex buffer", *renderer.quad_vertex_buffer, vuk::eMemoryRead);
      auto grid_index_buffer = vuk::acquire_buf("grid index buffer", *renderer.quad_index_buffer, vuk::eMemoryRead);

      std::tie(
        grid_attachment,
        depth_attachment,
        camera_buffer
      ) = grid_pass(grid_attachment, depth_attachment, grid_vertex_buffer, grid_index_buffer, camera_buffer);

      auto apply_grid_pass = vuk::make_pass(
        "apply_grid_pass",
        [](
          vuk::CommandBuffer& cmd_list,
          VUK_IA(vuk::eColorWrite) out,
          VUK_IA(vuk::eFragmentSampled) source,
          VUK_IA(vuk::eFragmentSampled) grid
        ) {
          cmd_list.bind_graphics_pipeline("apply_grid")
            .set_rasterization({})
            .broadcast_color_blend({})
            .set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
            .set_viewport(0, vuk::Rect2D::framebuffer())
            .set_scissor(0, vuk::Rect2D::framebuffer())
            .bind_image(0, 0, grid)
            .bind_image(0, 1, source)
            .bind_sampler(0, 2, vuk::LinearSamplerClamped)
            .draw(3, 1, 0, 0);

          return std::make_tuple(out, source, grid);
        }
      );

      auto grid_applied_attachment = vuk::declare_ia(
        "grid_applied_attachment",
        {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
         .sample_count = vuk::Samples::e1}
      );
      grid_applied_attachment.same_format_as(result_attachment);
      grid_applied_attachment.same_shape_as(result_attachment);
      grid_applied_attachment = vuk::clear_image(grid_applied_attachment, vuk::Black<f32>);

      std::tie(grid_applied_attachment, result_attachment, grid_attachment) = apply_grid_pass(
        grid_applied_attachment,
        result_attachment,
        grid_attachment
      );

      ctx.set_image_resource("result_attachment", std::move(grid_applied_attachment))
        .set_image_resource("depth_attachment", std::move(depth_attachment))
        .set_buffer_resource("camera_buffer", std::move(camera_buffer));
    }
  );
}
} // namespace ox
