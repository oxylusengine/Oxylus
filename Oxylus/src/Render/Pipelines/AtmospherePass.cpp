#include "Render/RendererPipeline.hpp"
#include "Render/Utils/VukCommon.hpp"

namespace ox {
auto atmosphere_pass(
  vuk::Value<vuk::Buffer>& atmosphere_buffer,
  vuk::Value<vuk::Buffer>& lights_buffer,
  vuk::Value<vuk::Buffer>& camera_buffer,
  vuk::Value<vuk::ImageAttachment>& sky_transmittance_lut_attachment,
  vuk::Value<vuk::ImageAttachment>& sky_multiscatter_lut_attachment,
  vuk::Value<vuk::ImageAttachment>& sky_view_lut_attachment,
  vuk::Value<vuk::ImageAttachment>& sky_aerial_perspective_attachment,
  vuk::Value<vuk::ImageAttachment>& final_attachment,
  vuk::Value<vuk::ImageAttachment>& depth_attachment
) -> void {
  auto sky_view_pass = vuk::make_pass(
    "sky view",
    [](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eComputeRead) atmosphere_,
      VUK_BA(vuk::eComputeRead) lights,
      VUK_BA(vuk::eComputeRead) camera,
      VUK_IA(vuk::eComputeSampled) sky_transmittance_lut,
      VUK_IA(vuk::eComputeSampled) sky_multiscatter_lut,
      VUK_IA(vuk::eComputeRW) sky_view_lut
    ) {
      auto linear_clamp_sampler = vuk::SamplerCreateInfo{
        .magFilter = vuk::Filter::eLinear,
        .minFilter = vuk::Filter::eLinear,
        .addressModeU = vuk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vuk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vuk::SamplerAddressMode::eClampToEdge,
      };

      cmd_list.bind_compute_pipeline("sky_view_pipeline")
        .bind_sampler(0, 0, linear_clamp_sampler)
        .bind_image(0, 1, sky_transmittance_lut)
        .bind_image(0, 2, sky_multiscatter_lut)
        .bind_image(0, 3, sky_view_lut)
        .bind_buffer(0, 4, atmosphere_)
        .bind_buffer(0, 5, lights)
        .bind_buffer(0, 6, camera)
        .dispatch_invocations_per_pixel(sky_view_lut);

      return std::make_tuple(sky_view_lut, sky_transmittance_lut, sky_multiscatter_lut, atmosphere_, lights, camera);
    }
  );

  std::tie(
    sky_view_lut_attachment,
    sky_transmittance_lut_attachment,
    sky_multiscatter_lut_attachment,
    atmosphere_buffer,
    lights_buffer,
    camera_buffer
  ) =
    sky_view_pass(
      std::move(atmosphere_buffer),
      std::move(lights_buffer),
      std::move(camera_buffer),
      std::move(sky_transmittance_lut_attachment),
      std::move(sky_multiscatter_lut_attachment),
      std::move(sky_view_lut_attachment)
    );

  auto sky_aerial_perspective_pass = vuk::make_pass(
    "sky aerial perspective",
    [](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eComputeRead) atmosphere_,
      VUK_BA(vuk::eComputeRead) lights,
      VUK_BA(vuk::eComputeRead) camera,
      VUK_IA(vuk::eComputeSampled) sky_transmittance_lut,
      VUK_IA(vuk::eComputeSampled) sky_multiscatter_lut,
      VUK_IA(vuk::eComputeRW) sky_aerial_perspective_lut
    ) {
      auto linear_clamp_sampler = vuk::SamplerCreateInfo{
        .magFilter = vuk::Filter::eLinear,
        .minFilter = vuk::Filter::eLinear,
        .addressModeU = vuk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vuk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vuk::SamplerAddressMode::eClampToEdge,
      };

      cmd_list.bind_compute_pipeline("sky_aerial_perspective_pipeline")
        .bind_sampler(0, 0, linear_clamp_sampler)
        .bind_image(0, 1, sky_transmittance_lut)
        .bind_image(0, 2, sky_multiscatter_lut)
        .bind_image(0, 3, sky_aerial_perspective_lut)
        .bind_buffer(0, 4, atmosphere_)
        .bind_buffer(0, 5, lights)
        .bind_buffer(0, 6, camera)
        .dispatch_invocations_per_pixel(sky_aerial_perspective_lut);

      return std::make_tuple(sky_aerial_perspective_lut, sky_transmittance_lut, atmosphere_, lights, camera);
    }
  );

  std::tie(
    sky_aerial_perspective_attachment,
    sky_transmittance_lut_attachment,
    atmosphere_buffer,
    lights_buffer,
    camera_buffer
  ) =
    sky_aerial_perspective_pass(
      std::move(atmosphere_buffer),
      std::move(lights_buffer),
      std::move(camera_buffer),
      std::move(sky_transmittance_lut_attachment),
      std::move(sky_multiscatter_lut_attachment),
      std::move(sky_aerial_perspective_attachment)
    );

  auto sky_final_pass = vuk::make_pass(
    "sky final",
    [](
      vuk::CommandBuffer& cmd_list,
      VUK_IA(vuk::eColorWrite) dst,
      VUK_BA(vuk::eFragmentRead) atmosphere_,
      VUK_BA(vuk::eFragmentRead) sun_,
      VUK_BA(vuk::eFragmentRead) camera,
      VUK_IA(vuk::eFragmentSampled) sky_transmittance_lut,
      VUK_IA(vuk::eFragmentSampled) sky_aerial_perspective_lut,
      VUK_IA(vuk::eFragmentSampled) sky_view_lut,
      VUK_IA(vuk::eFragmentSampled) depth
    ) {
      vuk::PipelineColorBlendAttachmentState blend_info = {
        .blendEnable = true,
        .srcColorBlendFactor = vuk::BlendFactor::eOne,
        .dstColorBlendFactor = vuk::BlendFactor::eSrcAlpha,
        .colorBlendOp = vuk::BlendOp::eAdd,
        .srcAlphaBlendFactor = vuk::BlendFactor::eZero,
        .dstAlphaBlendFactor = vuk::BlendFactor::eOne,
        .alphaBlendOp = vuk::BlendOp::eAdd,
      };

      auto linear_clamp_sampler = vuk::SamplerCreateInfo{
        .magFilter = vuk::Filter::eLinear,
        .minFilter = vuk::Filter::eLinear,
        .addressModeU = vuk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vuk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vuk::SamplerAddressMode::eClampToEdge,
      };

      cmd_list.bind_graphics_pipeline("sky_final_pipeline")
        .set_rasterization({})
        .set_depth_stencil({})
        .set_color_blend(dst, blend_info)
        .set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
        .set_viewport(0, vuk::Rect2D::framebuffer())
        .set_scissor(0, vuk::Rect2D::framebuffer())
        .bind_sampler(0, 0, linear_clamp_sampler)
        .bind_image(0, 1, sky_transmittance_lut)
        .bind_image(0, 2, sky_aerial_perspective_lut)
        .bind_image(0, 3, sky_view_lut)
        .bind_image(0, 4, depth)
        .bind_buffer(0, 5, atmosphere_)
        .bind_buffer(0, 6, sun_)
        .bind_buffer(0, 7, camera)
        .draw(3, 1, 0, 0);

      return std::make_tuple(dst, depth, camera);
    }
  );

  std::tie(final_attachment, depth_attachment, camera_buffer) = sky_final_pass(
    std::move(final_attachment),
    std::move(atmosphere_buffer),
    std::move(lights_buffer),
    std::move(camera_buffer),
    std::move(sky_transmittance_lut_attachment),
    std::move(sky_aerial_perspective_attachment),
    std::move(sky_view_lut_attachment),
    std::move(depth_attachment)
  );
}
} // namespace ox
