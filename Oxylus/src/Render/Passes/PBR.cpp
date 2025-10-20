#include <vuk/runtime/CommandBuffer.hpp>

#include "Render/RendererInstance.hpp"
#include "Render/Utils/VukCommon.hpp"

namespace ox {
auto RendererInstance::draw_atmosphere(this RendererInstance& self, AtmosphereContext& context) -> void {
  ZoneScoped;
  auto sky_view_pass = vuk::make_pass(
    "sky view",
    [](
      vuk::CommandBuffer& cmd_list, //
      VUK_IA(vuk::eComputeSampled) sky_transmittance_lut,
      VUK_IA(vuk::eComputeSampled) sky_multiscatter_lut,
      VUK_BA(vuk::eComputeUniformRead) atmosphere_,
      VUK_BA(vuk::eComputeUniformRead) directional_light_,
      VUK_IA(vuk::eComputeRW) sky_view_lut
    ) {
      cmd_list //
        .bind_compute_pipeline("sky_view")
        .bind_sampler(0, 0, vuk::LinearSamplerClamped)
        .bind_image(0, 1, sky_transmittance_lut)
        .bind_image(0, 2, sky_multiscatter_lut)
        .bind_buffer(0, 3, atmosphere_)
        .bind_buffer(0, 4, directional_light_)
        .bind_image(0, 5, sky_view_lut)
        .dispatch_invocations_per_pixel(sky_view_lut);
      return std::make_tuple(
        sky_transmittance_lut,
        sky_multiscatter_lut,
        atmosphere_,
        directional_light_,
        sky_view_lut
      );
    }
  );

  std::tie(
    context.sky_transmittance_lut_attachment,
    context.sky_multiscatter_lut_attachment,
    self.prepared_frame.atmosphere_buffer,
    self.prepared_frame.directional_light_buffer,
    context.sky_view_lut_attachment
  ) =
    sky_view_pass(
      std::move(context.sky_transmittance_lut_attachment),
      std::move(context.sky_multiscatter_lut_attachment),
      std::move(self.prepared_frame.atmosphere_buffer),
      std::move(self.prepared_frame.directional_light_buffer),
      std::move(context.sky_view_lut_attachment)
    );

  auto sky_aerial_perspective_pass = vuk::make_pass(
    "sky aerial perspective",
    [](
      vuk::CommandBuffer& cmd_list, //
      VUK_IA(vuk::eComputeSampled) sky_transmittance_lut,
      VUK_IA(vuk::eComputeSampled) sky_multiscatter_lut,
      VUK_BA(vuk::eComputeUniformRead) atmosphere_,
      VUK_BA(vuk::eComputeUniformRead) directional_light_,
      VUK_BA(vuk::eComputeUniformRead) camera,
      VUK_IA(vuk::eComputeRW) sky_aerial_perspective_lut
    ) {
      cmd_list //
        .bind_compute_pipeline("sky_aerial_perspective")
        .bind_sampler(0, 0, vuk::LinearSamplerClamped)
        .bind_image(0, 1, sky_transmittance_lut)
        .bind_image(0, 2, sky_multiscatter_lut)
        .bind_buffer(0, 3, atmosphere_)
        .bind_buffer(0, 4, directional_light_)
        .bind_buffer(0, 5, camera)
        .bind_image(0, 6, sky_aerial_perspective_lut)
        .dispatch_invocations_per_pixel(sky_aerial_perspective_lut);

      return std::make_tuple(
        sky_transmittance_lut,
        sky_multiscatter_lut,
        atmosphere_,
        directional_light_,
        camera,
        sky_aerial_perspective_lut
      );
    }
  );

  std::tie(
    context.sky_transmittance_lut_attachment,
    context.sky_multiscatter_lut_attachment,
    self.prepared_frame.atmosphere_buffer,
    self.prepared_frame.directional_light_buffer,
    self.prepared_frame.camera_buffer,
    context.sky_aerial_perspective_lut_attachment
  ) =
    sky_aerial_perspective_pass(
      std::move(context.sky_transmittance_lut_attachment),
      std::move(context.sky_multiscatter_lut_attachment),
      std::move(self.prepared_frame.atmosphere_buffer),
      std::move(self.prepared_frame.directional_light_buffer),
      std::move(self.prepared_frame.camera_buffer),
      std::move(context.sky_aerial_perspective_lut_attachment)
    );
}

auto RendererInstance::generate_ambient_occlusion(this RendererInstance& self, AmbientOcclusionContext& context)
  -> void {
  ZoneScoped;

  auto vbgtao_prefilter_pass = vuk::make_pass(
    "vbgtao prefilter",
    [](
      vuk::CommandBuffer& command_buffer, //
      VUK_IA(vuk::eComputeSampled) depth_input,
      VUK_IA(vuk::eComputeRW) dst_image
    ) {
      command_buffer //
        .bind_compute_pipeline("vbgtao_prefilter")
        .bind_image(0, 0, depth_input)
        .bind_image(0, 1, dst_image->mip(0))
        .bind_image(0, 2, dst_image->mip(1))
        .bind_image(0, 3, dst_image->mip(2))
        .bind_image(0, 4, dst_image->mip(3))
        .bind_image(0, 5, dst_image->mip(4))
        .bind_sampler(0, 6, vuk::NearestSamplerClamped)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, depth_input->extent)
        .dispatch((depth_input->extent.width + 16 - 1) / 16, (depth_input->extent.height + 16 - 1) / 16);

      return std::make_tuple(depth_input, dst_image);
    }
  );

  auto vbgtao_depth_attachment = vuk::declare_ia(
    "vbgtao depth",
    {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage,
     .format = vuk::Format::eR32Sfloat,
     .sample_count = vuk::Samples::e1,
     .level_count = 5,
     .layer_count = 1}
  );
  vbgtao_depth_attachment.same_extent_as(context.depth_attachment);
  vbgtao_depth_attachment = vuk::clear_image(std::move(vbgtao_depth_attachment), vuk::Black<f32>);

  std::tie(context.depth_attachment, vbgtao_depth_attachment) = vbgtao_prefilter_pass(
    std::move(context.depth_attachment),
    std::move(vbgtao_depth_attachment)
  );

  auto vbgtao_generate_pass = vuk::make_pass(
    "vbgtao generate",
    [settings = self.vbgtao_info](
      vuk::CommandBuffer& command_buffer,
      VUK_BA(vuk::eComputeUniformRead) camera,
      VUK_IA(vuk::eComputeSampled) prefiltered_depth,
      VUK_IA(vuk::eComputeSampled) normals,
      VUK_IA(vuk::eComputeSampled) hilbert_noise,
      VUK_IA(vuk::eComputeRW) ambient_occlusion,
      VUK_IA(vuk::eComputeRW) depth_differences
    ) {
      command_buffer //
        .bind_compute_pipeline("vbgtao_main")
        .bind_buffer(0, 0, camera)
        .bind_image(0, 1, prefiltered_depth)
        .bind_image(0, 2, normals)
        .bind_image(0, 3, hilbert_noise)
        .bind_image(0, 4, ambient_occlusion)
        .bind_image(0, 5, depth_differences)
        .bind_sampler(0, 6, vuk::NearestSamplerClamped)
        .bind_sampler(0, 7, vuk::LinearSamplerClamped)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, settings)
        .dispatch_invocations_per_pixel(ambient_occlusion);

      return std::make_tuple(camera, normals, hilbert_noise, ambient_occlusion, depth_differences);
    }
  );

  auto vbgtao_noisy_occlusion_attachment = vuk::declare_ia(
    "vbgtao noisy occlusion",
    {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage,
     .format = vuk::Format::eR16Sfloat,
     .sample_count = vuk::Samples::e1}
  );
  vbgtao_noisy_occlusion_attachment.same_shape_as(context.ambient_occlusion_attachment);
  vbgtao_noisy_occlusion_attachment = vuk::clear_image(std::move(vbgtao_noisy_occlusion_attachment), vuk::White<f32>);

  std::tie(
    self.prepared_frame.camera_buffer,
    context.normal_attachment,
    context.noise_attachment,
    vbgtao_noisy_occlusion_attachment,
    context.depth_differences_attachment
  ) =
    vbgtao_generate_pass(
      std::move(self.prepared_frame.camera_buffer),
      std::move(vbgtao_depth_attachment),
      std::move(context.normal_attachment),
      std::move(context.noise_attachment),
      std::move(vbgtao_noisy_occlusion_attachment),
      std::move(context.depth_differences_attachment)
    );

  auto vbgtao_denoise_pass = vuk::make_pass(
    "vbgtao denoise",
    [gtao_settings = self.vbgtao_info](
      vuk::CommandBuffer& command_buffer,
      VUK_IA(vuk::eComputeSampled) noisy_occlusion,
      VUK_IA(vuk::eComputeSampled) depth_differences,
      VUK_IA(vuk::eComputeRW) ambient_occlusion
    ) {
      glm::ivec2 occlusion_noisy_extent = {noisy_occlusion->extent.width, noisy_occlusion->extent.height};
      command_buffer //
        .bind_compute_pipeline("vbgtao_denoise")
        .bind_image(0, 0, noisy_occlusion)
        .bind_image(0, 1, depth_differences)
        .bind_image(0, 2, ambient_occlusion)
        .bind_sampler(0, 3, vuk::NearestSamplerClamped)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(occlusion_noisy_extent, gtao_settings))
        .dispatch_invocations_per_pixel(ambient_occlusion);

      return std::make_tuple(ambient_occlusion, noisy_occlusion);
    }
  );

  std::tie(context.ambient_occlusion_attachment, vbgtao_noisy_occlusion_attachment) = vbgtao_denoise_pass(
    std::move(vbgtao_noisy_occlusion_attachment),
    std::move(context.depth_differences_attachment),
    std::move(context.ambient_occlusion_attachment)
  );
}

auto RendererInstance::apply_pbr(
  this RendererInstance& self, PBRContext& context, vuk::Value<vuk::ImageAttachment>&& dst_attachment
) -> vuk::Value<vuk::ImageAttachment> {
  ZoneScoped;

  auto pbr_apply_pass = vuk::make_pass(
    "pbr apply",
    [scene_flags = self.gpu_scene_flags](
      vuk::CommandBuffer& cmd_list,
      VUK_IA(vuk::eColorWrite) dst,
      VUK_IA(vuk::eFragmentSampled) sky_transmittance_lut,
      VUK_IA(vuk::eFragmentSampled) sky_aerial_perspective_lut_attachment,
      VUK_IA(vuk::eFragmentSampled) sky_view_lut,
      VUK_IA(vuk::eFragmentSampled) depth,
      VUK_IA(vuk::eFragmentSampled) albedo,
      VUK_IA(vuk::eFragmentSampled) normal,
      VUK_IA(vuk::eFragmentSampled) emissive,
      VUK_IA(vuk::eFragmentSampled) metallic_roughness_occlusion,
      VUK_IA(vuk::eFragmentSampled) gtao,
      VUK_IA(vuk::eFragmentSampled) contact_shadows,
      VUK_IA(vuk::eFragmentSampled) shadows,
      VUK_BA(vuk::eFragmentUniformRead) lights,
      VUK_BA(vuk::eFragmentUniformRead) camera
    ) {
      cmd_list //
        .bind_graphics_pipeline("pbr_apply")
        .set_rasterization({})
        .set_color_blend(dst, vuk::BlendPreset::eOff)
        .set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
        .set_viewport(0, vuk::Rect2D::framebuffer())
        .set_scissor(0, vuk::Rect2D::framebuffer())
        .bind_sampler(0, 0, vuk::LinearSamplerClamped)
        .bind_sampler(0, 1, vuk::LinearSamplerRepeated)
        .bind_image(0, 2, sky_transmittance_lut)
        .bind_image(0, 3, sky_aerial_perspective_lut_attachment)
        .bind_image(0, 4, sky_view_lut)
        .bind_image(0, 5, depth)
        .bind_image(0, 6, albedo)
        .bind_image(0, 7, normal)
        .bind_image(0, 8, emissive)
        .bind_image(0, 9, metallic_roughness_occlusion)
        .bind_image(0, 10, gtao)
        .bind_image(0, 11, contact_shadows)
        .bind_image(0, 12, shadows)
        .bind_buffer(0, 13, lights)
        .bind_buffer(0, 14, camera)
        .push_constants(vuk::ShaderStageFlagBits::eFragment, 0, scene_flags)
        .draw(3, 1, 0, 0);

      return std::make_tuple(
        dst,
        sky_transmittance_lut,
        sky_aerial_perspective_lut_attachment,
        sky_view_lut,
        depth,
        albedo,
        normal,
        emissive,
        metallic_roughness_occlusion,
        gtao,
        contact_shadows,
        shadows,
        lights,
        camera
      );
    }
  );

  std::tie(
    dst_attachment,
    context.sky_transmittance_lut_attachment,
    context.sky_aerial_perspective_lut_attachment,
    context.sky_view_lut_attachment,
    context.depth_attachment,
    context.albedo_attachment,
    context.normal_attachment,
    context.emissive_attachment,
    context.metallic_roughness_occlusion_attachment,
    context.ambient_occlusion_attachment,
    context.contact_shadows_attachment,
    context.directional_shadowmap_attachment,
    self.prepared_frame.lights_buffer,
    self.prepared_frame.camera_buffer
  ) =
    pbr_apply_pass(
      std::move(dst_attachment),
      std::move(context.sky_transmittance_lut_attachment),
      std::move(context.sky_aerial_perspective_lut_attachment),
      std::move(context.sky_view_lut_attachment),
      std::move(context.depth_attachment),
      std::move(context.albedo_attachment),
      std::move(context.normal_attachment),
      std::move(context.emissive_attachment),
      std::move(context.metallic_roughness_occlusion_attachment),
      std::move(context.ambient_occlusion_attachment),
      std::move(context.contact_shadows_attachment),
      std::move(context.directional_shadowmap_attachment),
      std::move(self.prepared_frame.lights_buffer),
      std::move(self.prepared_frame.camera_buffer)
    );

  return dst_attachment;
}
} // namespace ox
