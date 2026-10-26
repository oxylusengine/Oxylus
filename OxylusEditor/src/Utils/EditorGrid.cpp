#include "EditorGrid.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <tuple>
#include <vir/RenderGraph.hpp>
#include <vir/Types.hpp>

#include "Core/App.hpp"
#include "Render/RenderContext.hpp"
#include "Render/Renderer.hpp"
#include "Render/RendererInstance.hpp"
#include "Render/Utils/PushConstants.hpp"
#include "Render/Utils/Samplers.hpp"

namespace ox {
using GridPush = PushConstants<glm::mat4, f32>;

auto add_editor_grid_stage(RendererInstance& renderer_instance, std::function<auto()->f32> distance_provider) -> void {
  ZoneScoped;

  renderer_instance.add_stage_after(
    RenderStage::PostProcessing,
    "grid_stage",
    [](RenderStageContext& ctx) -> void {
      auto& module = ctx.module;
      auto& render_context = ctx.render_context;
      auto result_attachment = ctx.get_image_resource("result_attachment");
      auto depth_attachment = ctx.get_image_resource("depth_attachment");
      auto camera_buffer = ctx.get_buffer_resource("camera_buffer");

      const auto quad_rest = vir::Access::VertexRead | vir::Access::IndexRead;
      auto vertex_buffer = module.declare_buffer_var("grid vertices", quad_rest);
      auto index_buffer = module.declare_buffer_var("grid indices", quad_rest);
      const auto push = module.declare_bytes_var("grid push", sizeof(GridPush));
      ctx.variables["grid vertices"] = vertex_buffer;
      ctx.variables["grid indices"] = index_buffer;
      ctx.variables["grid push"] = push;

      const auto extent = module.image_extent(result_attachment).value_or(vir::Extent3D{1, 1, 1});
      const auto format = module.image_format(result_attachment)
                            .value_or(std::pair{vir::Format::RGBA8Unorm, vir::Samples::e1})
                            .first;
      const auto black = vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 1.0f);
      auto grid_attachment = module.clear(
        module.transient_image({.extent = extent, .format = format, .name = "grid"}),
        black
      );

      std::tie(grid_attachment, depth_attachment, std::ignore, std::ignore, camera_buffer) = std::tuple_cat(
        module
          .begin_rendering(
            {{grid_attachment, vir::Access::ColorRW},
             {depth_attachment, vir::Access::DepthStencilRead},
             {vertex_buffer, vir::Access::AttributeRead},
             {index_buffer, vir::Access::IndexRead},
             {camera_buffer, vir::Access::VertexUniformRead | vir::Access::FragmentUniformRead}}
          )
          .with_name("grid")
          .bind_graphics_pipeline(render_context.pipeline("grid"))
          .set_depth({.is_test_enabled = true, .is_write_enabled = false, .compare_op = VK_COMPARE_OP_GREATER_OR_EQUAL})
          .broadcast_color_blend(vir::BlendPreset::AlphaBlend)
          .set_viewport(0, vir::Rect2D::framebuffer())
          .set_scissor(0, vir::Rect2D::framebuffer())
          .bind_buffer(0, 0, camera_buffer)
          .bind_vertex_buffer(0, vertex_buffer)
          .bind_index_buffer(index_buffer, VK_INDEX_TYPE_UINT32)
          .push_constants_from(push)
          .draw_indexed(6, 1)
          .end_rendering()
      );

      auto grid_applied = module.clear(
        module.transient_image({.extent = extent, .format = format, .name = "grid applied"}),
        black
      );
      grid_applied = //
        module
          .begin_rendering(
            {{grid_applied, vir::Access::ColorWrite},
             {result_attachment, vir::Access::FragmentSampled},
             {grid_attachment, vir::Access::FragmentSampled}}
          )
          .with_name("apply grid")
          .bind_graphics_pipeline(render_context.pipeline("apply_grid"))
          .set_viewport(0, vir::Rect2D::framebuffer())
          .set_scissor(0, vir::Rect2D::framebuffer())
          .bind_image(0, 0, grid_attachment)
          .bind_image(0, 1, result_attachment)
          .bind_sampler(0, 2, render_context.cached_sampler_handle(LinearSamplerClamped))
          .draw(3, 1)
          .end_rendering()[0];

      ctx.set_image_resource("result_attachment", grid_applied)
        .set_image_resource("depth_attachment", depth_attachment)
        .set_buffer_resource("camera_buffer", camera_buffer);
    },
    [grid_distance = std::move(distance_provider)](vir::Program& program, const StageVariables& variables) -> void {
      auto& render_context = App::get_rendercontext();
      auto& renderer = App::mod<Renderer>();
      program.set(variables.at("grid vertices"), render_context.buffer(renderer.quad_vertex_buffer));
      program.set(variables.at("grid indices"), render_context.buffer(renderer.quad_index_buffer));

      const auto scale = glm::floor(glm::vec3(grid_distance() / 2.0f)) * 2.0f;
      const auto rotation = glm::vec3(glm::radians(90.0f), 0.0f, 0.0f);
      const auto grid_transform = glm::toMat4(glm::quat(rotation)) * glm::scale(glm::mat4(1.0f), scale);
      program.set_bytes(variables.at("grid push"), GridPush(grid_transform, scale.x));
    }
  );
}
} // namespace ox
