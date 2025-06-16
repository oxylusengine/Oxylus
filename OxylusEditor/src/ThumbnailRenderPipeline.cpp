#include "ThumbnailRenderPipeline.hpp"

#include "Core/App.hpp"
#include "Render/Camera.hpp"
#include "Render/Slang/Slang.hpp"
#include "Render/Utils/VukCommon.hpp"
#include "Scene/ECSModule/Core.hpp"
#include "Scene/SceneGPU.hpp"

namespace ox {
auto ThumbnailRenderPipeline::init(VkContext& vk_context) -> void {
  auto& runtime = *vk_context.runtime;

  // --- Shaders ---
  auto* vfs = App::get_system<VFS>(EngineSystems::VFS);
  auto shaders_dir = vfs->resolve_physical_dir(VFS::APP_DIR, "Shaders");

  Slang slang = {};
  slang.create_session({.root_directory = shaders_dir, .definitions = {}});

  slang.create_pipeline(runtime,
                        "simple_forward_pipeline",
                        {},
                        {.path = shaders_dir + "/editor/simple_forward.slang", .entry_points = {"vs_main", "fs_main"}});
}

auto ThumbnailRenderPipeline::deinit() -> void {}

auto ThumbnailRenderPipeline::reset() -> void {}

auto ThumbnailRenderPipeline::on_render(VkContext& vk_context, const RenderInfo& render_info)
    -> vuk::Value<vuk::ImageAttachment> {

  if (_final_image == nullptr) {
    _final_image = std::make_unique<Texture>();
    _final_image->create({}, {.preset = Preset::eRTT2DUnmipped, .extent = render_info.extent});
    _final_image->set_name(thumbnail_name);
  }

  auto final_attachment = vuk::acquire_ia(
      _final_image->get_name().c_str(), _final_image->attachment(), vuk::Access::eNone);

  final_attachment = vuk::clear_image(final_attachment, vuk::Black<f32>);

  if (mesh == nullptr)
    return final_attachment;

  CameraComponent cam{.position = {0, 0, 3}};
  Camera::update(cam, {render_info.extent.width, render_info.extent.height});

  const auto camera_data = GPU::CameraData{
      .position = glm::vec4(cam.position, 0.0f),
      .projection = cam.get_projection_matrix(),
      .inv_projection = cam.get_inv_projection_matrix(),
      .view = cam.get_view_matrix(),
      .inv_view = cam.get_inv_view_matrix(),
      .projection_view = cam.get_projection_matrix() * cam.get_view_matrix(),
      .inv_projection_view = cam.get_inverse_projection_view(),
      .previous_projection = cam.get_projection_matrix(),
      .previous_inv_projection = cam.get_inv_projection_matrix(),
      .previous_view = cam.get_view_matrix(),
      .previous_inv_view = cam.get_inv_view_matrix(),
      .previous_projection_view = cam.get_projection_matrix() * cam.get_view_matrix(),
      .previous_inv_projection_view = cam.get_inverse_projection_view(),
      .temporalaa_jitter = cam.jitter,
      .temporalaa_jitter_prev = cam.jitter_prev,
      .near_clip = cam.near_clip,
      .far_clip = cam.far_clip,
      .fov = cam.fov,
      .output_index = 0,
  };

  auto camera_buffer = vk_context.scratch_buffer(camera_data);

  auto vertex_positions = *mesh->vertex_positions;
  auto indices = *mesh->indices;
  auto indices_count = mesh->indices_count;

  auto thumbnail_pass = vuk::make_pass( //
      "thumbnail_pass",
      [indices_count, indices, vertex_positions](vuk::CommandBuffer& command_buffer,
                                                 VUK_IA(vuk::eColorWrite) output,
                                                 VUK_BA(vuk::eVertexRead) camera_buffer_) {
        const auto vertex_pack = vuk::Packed{
            vuk::Format::eR32G32B32Sfloat, // vec3
        };

        command_buffer.bind_graphics_pipeline("simple_forward_pipeline")
            .set_dynamic_state(vuk::DynamicStateFlagBits::eScissor | vuk::DynamicStateFlagBits::eViewport)
            .set_viewport(0, vuk::Rect2D::framebuffer())
            .set_scissor(0, vuk::Rect2D::framebuffer())
            .set_rasterization({})
            .broadcast_color_blend({})
            .push_constants(vuk::ShaderStageFlagBits::eVertex, 0, PushConstants(camera_buffer_->device_address))
            .bind_index_buffer(indices, vuk::IndexType::eUint32)
            .bind_vertex_buffer(0, vertex_positions, 0, vertex_pack)
            .draw_indexed(indices_count, 1, 0, 0, 0);

        return output;
      });

  return thumbnail_pass(final_attachment, camera_buffer);
}

auto ThumbnailRenderPipeline::on_update(Scene* scene) -> void {}

auto ThumbnailRenderPipeline::set_mesh(this ThumbnailRenderPipeline& self, Mesh* mesh) -> void {
  ZoneScoped;

  self.mesh = mesh;
}

auto ThumbnailRenderPipeline::set_name(this ThumbnailRenderPipeline& self, const std::string& name) -> void {
  ZoneScoped;

  self.thumbnail_name = name;
}
} // namespace ox
