#include "Core/Enum.hpp"
#include "Render/Graph/RenderGraph.hpp"
#include "Render/Graph/Types.hpp"
#include "Render/RenderContext.hpp"
#include "Render/RendererInstance.hpp"
#include "Render/Utils/PushConstants.hpp"
#include "Render/Utils/Samplers.hpp"
#include "Scene/Scene.hpp"

namespace ox {
constexpr static auto TRANSIENT_BUFFER_USAGE = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

using SkyViewPush = PushConstants<glm::vec3, f32, glm::vec3>;
using SkyCubemapPush = PushConstants<glm::vec3, f32, f32, u32>;
using SkyAerialPush = PushConstants<glm::vec3, f32>;
using VBGTAOPrefilterPush = PushConstants<vir::Extent3D, glm::vec2>;
using VBGTAODenoisePush = PushConstants<glm::ivec2, GPU::VBGTAOSettings>;
using RTAOPush = PushConstants<u32, f32, f32, u32>;
using PBRSunPush = PushConstants<glm::vec3, f32, glm::vec3>;
using PBRLightsPush = PushConstants<u32, glm::vec3, u32>;

auto RendererInstance::bind_atmosphere_variables(
  this const RendererInstance& self, vir::Program& program, const PBRVariables& variables
) -> void {
  ZoneScoped;
  if (!program.has_variable(variables.is_luts_dirty)) {
    return;
  }

  const auto num_frames = static_cast<u32>(self.renderer.render_context->num_frames);
  program.set(variables.is_luts_dirty, self.atmosphere_luts_dirty);
  program.set_bytes(
    variables.sky_view_push,
    SkyViewPush(
      self.directional_light.direction,
      self.directional_light.intensity,
      glm::vec3(self.camera_data.position)
    )
  );
  program.set_bytes(
    variables.sky_cubemap_push,
    SkyCubemapPush(self.directional_light.direction, self.directional_light.intensity, 0.15f, num_frames)
  );
  program.set_bytes(
    variables.aerial_push,
    SkyAerialPush(self.directional_light.direction, self.directional_light.intensity)
  );
}

auto RendererInstance::draw_atmosphere(
  this RendererInstance& self, vir::Module& module, FrameRecording& recording, AtmosphereContext& context
) -> void {
  ZoneScoped;
  auto& prepared = recording.prepared;
  auto& values = recording.values;

  auto& render_context = *self.renderer.render_context;
  auto& variables = prepared.pbr;
  variables.is_luts_dirty = module.declare_bool_var("atmosphere luts dirty", false);
  variables.sky_view_push = module.declare_bytes_var("sky view push", sizeof(SkyViewPush));
  variables.sky_cubemap_push = module.declare_bytes_var("sky cubemap push", sizeof(SkyCubemapPush));
  variables.aerial_push = module.declare_bytes_var("sky aerial perspective push", sizeof(SkyAerialPush));

  const auto linear_clamped = render_context.cached_sampler_handle(vir::LinearSamplerClamped);

  // the transmittance and multiscattering luts only follow the atmosphere settings, so they are
  // rebuilt on the frames that change them
  std::tie(
    context.sky_transmittance_lut_attachment,
    context.sky_multiscatter_lut_attachment,
    values.atmosphere,
    context.sky_cubemap_attachment
  ) = std::tuple_cat(module
                       .set_condition(
                         variables.is_luts_dirty,
                         [&](vir::Module& arm) {
                           arm
                             .begin_compute(
                               {{context.sky_transmittance_lut_attachment, vir::Access::ComputeRW},
                                {values.atmosphere, vir::Access::ComputeUniformRead}}
                             )
                             .with_name("sky transmittance")
                             .bind_compute_pipeline(render_context.pipeline("sky_transmittance"))
                             .bind_image(0, 0, context.sky_transmittance_lut_attachment)
                             .bind_buffer(0, 1, values.atmosphere)
                             .dispatch_invocations_per_pixel(context.sky_transmittance_lut_attachment);
                           const auto [transmittance, atmosphere] = arm.end_compute<2>();

                           arm
                             .begin_compute(
                               {{transmittance, vir::Access::ComputeSampled},
                                {context.sky_multiscatter_lut_attachment, vir::Access::ComputeRW},
                                {atmosphere, vir::Access::ComputeUniformRead}}
                             )
                             .with_name("sky multiscattering")
                             .bind_compute_pipeline(render_context.pipeline("sky_multiscatter"))
                             .bind_sampler(0, 0, linear_clamped)
                             .bind_image(0, 1, transmittance)
                             .bind_image(0, 2, context.sky_multiscatter_lut_attachment)
                             .bind_buffer(0, 3, atmosphere)
                             .dispatch_invocations_per_pixel(context.sky_multiscatter_lut_attachment);
                           const auto [transmittance_out, multiscatter, atmosphere_out] = arm.end_compute<3>();

                           const auto cubemap = arm.clear(
                             context.sky_cubemap_attachment,
                             vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 1.0f)
                           );
                           return std::array{transmittance_out, multiscatter, atmosphere_out, cubemap};
                         },
                         [&](vir::Module&) {
                           return std::array{
                             context.sky_transmittance_lut_attachment,
                             context.sky_multiscatter_lut_attachment,
                             values.atmosphere,
                             context.sky_cubemap_attachment,
                           };
                         }
                       ));

  module
    .begin_compute(
      {{context.sky_transmittance_lut_attachment, vir::Access::ComputeSampled},
       {context.sky_multiscatter_lut_attachment, vir::Access::ComputeSampled},
       {values.atmosphere, vir::Access::ComputeUniformRead},
       {context.sky_view_lut_attachment, vir::Access::ComputeRW}}
    )
    .with_name("sky view")
    .bind_compute_pipeline(render_context.pipeline("sky_view"))
    .bind_sampler(0, 0, linear_clamped)
    .bind_image(0, 1, context.sky_transmittance_lut_attachment)
    .bind_image(0, 2, context.sky_multiscatter_lut_attachment)
    .bind_buffer(0, 3, values.atmosphere)
    .bind_image(0, 4, context.sky_view_lut_attachment)
    .push_constants_from(variables.sky_view_push)
    .dispatch_invocations_per_pixel(context.sky_view_lut_attachment);
  std::tie(
    context.sky_transmittance_lut_attachment,
    context.sky_multiscatter_lut_attachment,
    values.atmosphere,
    context.sky_view_lut_attachment
  ) = std::tuple_cat(module.end_compute<4>());

  constexpr auto cubemap_layers = 6.0f;
  module
    .begin_compute(
      {{context.sky_view_lut_attachment, vir::Access::ComputeSampled},
       {context.sky_transmittance_lut_attachment, vir::Access::ComputeSampled},
       {values.camera, vir::Access::ComputeUniformRead},
       {values.atmosphere, vir::Access::ComputeRead},
       {context.sky_cubemap_attachment, vir::Access::ComputeRW}}
    )
    .with_name("sky cubemap")
    .bind_compute_pipeline(render_context.pipeline("sky_ibl"))
    .bind_sampler(0, 0, linear_clamped)
    .bind_image(0, 1, context.sky_view_lut_attachment)
    .bind_image(0, 2, context.sky_transmittance_lut_attachment)
    .bind_buffer(0, 3, values.camera)
    .bind_image(0, 4, context.sky_cubemap_attachment)
    .push_constant_address(0, values.atmosphere)
    .push_constants_from_at(8, variables.sky_cubemap_push)
    .dispatch_invocations_per_pixel(context.sky_cubemap_attachment, {1.0f, 1.0f, cubemap_layers});
  std::tie(
    context.sky_view_lut_attachment,
    context.sky_transmittance_lut_attachment,
    values.camera,
    values.atmosphere,
    context.sky_cubemap_attachment
  ) = std::tuple_cat(module.end_compute<5>());

  module
    .begin_compute(
      {{context.sky_transmittance_lut_attachment, vir::Access::ComputeSampled},
       {context.sky_multiscatter_lut_attachment, vir::Access::ComputeSampled},
       {values.atmosphere, vir::Access::ComputeUniformRead},
       {values.camera, vir::Access::ComputeUniformRead},
       {context.sky_aerial_perspective_lut_attachment, vir::Access::ComputeRW}}
    )
    .with_name("sky aerial perspective")
    .bind_compute_pipeline(render_context.pipeline("sky_aerial_perspective"))
    .bind_sampler(0, 0, linear_clamped)
    .bind_image(0, 1, context.sky_transmittance_lut_attachment)
    .bind_image(0, 2, context.sky_multiscatter_lut_attachment)
    .bind_buffer(0, 3, values.atmosphere)
    .bind_buffer(0, 4, values.camera)
    .bind_image(0, 5, context.sky_aerial_perspective_lut_attachment)
    .push_constants_from(variables.aerial_push)
    .dispatch_invocations_per_pixel(context.sky_aerial_perspective_lut_attachment);
  std::tie(
    context.sky_transmittance_lut_attachment,
    context.sky_multiscatter_lut_attachment,
    values.atmosphere,
    values.camera,
    context.sky_aerial_perspective_lut_attachment
  ) = std::tuple_cat(module.end_compute<5>());
}

auto RendererInstance::update_vbgtao_info(this RendererInstance& self, const RendererCVar& cvar) -> void {
  auto gtao_enabled = cvar.cvar_vbgtao_enable.as_bool();
  if (gtao_enabled && self.viewport_size_.x > 0) {
    self.vbgtao_info.thickness = cvar.cvar_vbgtao_thickness.get();
    self.vbgtao_info.effect_radius = cvar.cvar_vbgtao_radius.get();

    switch (cvar.cvar_vbgtao_quality_level.get()) {
      case 0: { // low
        self.vbgtao_info.slice_count = 1;
        self.vbgtao_info.samples_per_slice_side = 2;
        break;
      }
      case 1: { // medium
        self.vbgtao_info.slice_count = 2;
        self.vbgtao_info.samples_per_slice_side = 2;
        break;
      }
      case 2: { // high
        self.vbgtao_info.slice_count = 3;
        self.vbgtao_info.samples_per_slice_side = 3;
        break;
      }
      case 3: { // ultra
        self.vbgtao_info.slice_count = 9;
        self.vbgtao_info.samples_per_slice_side = 3;
        break;
      }
    }

    // vbgtao_info.noise_index = (RendererCVar::cvar_gtao_denoise_passes.get() > 0) ? (frameCounter % 64) : (0); //
    // TODO: If we have TAA
    self.vbgtao_info.noise_index = 0;
    self.vbgtao_info.final_power = cvar.cvar_vbgtao_final_power.get();
  }
}

auto RendererInstance::bind_ambient_occlusion_variables(
  this const RendererInstance& self, vir::Program& program, const PBRVariables& variables
) -> void {
  ZoneScoped;
  if (!program.has_variable(variables.vbgtao_settings)) {
    return;
  }

  const auto& shape = self.frame_params.shape;
  const auto depth_extent = vir::Extent3D{shape.render_size.x, shape.render_size.y, 1};
  const auto& projection = self.camera_data.projection;
  program.set_bytes(
    variables.vbgtao_prefilter_push,
    VBGTAOPrefilterPush(depth_extent, glm::vec2(projection[3][2], projection[2][2]))
  );
  program.set_bytes(variables.vbgtao_settings, self.vbgtao_info);
  program.set_bytes(
    variables.vbgtao_denoise_push,
    VBGTAODenoisePush(glm::ivec2(depth_extent.width, depth_extent.height), self.vbgtao_info)
  );
}

auto RendererInstance::generate_ambient_occlusion(
  this RendererInstance& self, vir::Module& module, FrameRecording& recording, AmbientOcclusionContext& context
) -> void {
  ZoneScoped;
  auto& prepared = recording.prepared;
  auto& values = recording.values;

  auto& render_context = *self.renderer.render_context;
  auto& variables = prepared.pbr;
  variables.vbgtao_prefilter_push = module.declare_bytes_var("vbgtao prefilter push", sizeof(VBGTAOPrefilterPush));
  variables.vbgtao_settings = module.declare_bytes_var("vbgtao settings", sizeof(GPU::VBGTAOSettings));
  variables.vbgtao_denoise_push = module.declare_bytes_var("vbgtao denoise push", sizeof(VBGTAODenoisePush));

  const auto nearest_clamped = render_context.cached_sampler_handle(vir::NearestSamplerClamped);
  const auto depth_extent = module.image_extent(context.depth_attachment).value_or(vir::Extent3D{1, 1, 1});

  // the prefilter push inverts ndc_z = (m[2][2] * view_z + m[2][3]) / -view_z, giving
  // linear_depth = mul / (ndc_z + add). glm is column major, so m[2][3] lives at projection[3][2]
  auto prefiltered_depth = module.transient_image({
    .extent = {depth_extent.width, depth_extent.height, 1},
    .format = vir::Format::R32Sfloat,
    .mip_levels = 5,
    .name = "vbgtao depth",
  });
  prefiltered_depth = module.clear(prefiltered_depth, vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 0.0f));

  auto prefilter_accesses = std::array<vir::ResourceAccess, 6>{};
  prefilter_accesses[0] = {context.depth_attachment, vir::Access::ComputeSampled};
  for (auto i = 0_u32; i < 5; i++) {
    prefilter_accesses[i + 1] = {module.mip(prefiltered_depth, i), vir::Access::ComputeWrite};
  }

  module.begin_compute(prefilter_accesses)
    .with_name("vbgtao prefilter")
    .bind_compute_pipeline(render_context.pipeline("vbgtao_prefilter"))
    .bind_image(0, 0, context.depth_attachment);
  for (auto i = 0_u32; i < 5; i++) {
    module.bind_image(0, 1 + i, prefilter_accesses[i + 1].resource);
  }

  module.bind_sampler(0, 6, nearest_clamped)
    .push_constants_from(variables.vbgtao_prefilter_push)
    .dispatch((depth_extent.width + 16 - 1) / 16, (depth_extent.height + 16 - 1) / 16, 1);
  const auto prefiltered = module.end_compute<6>();
  context.depth_attachment = prefiltered[0];
  prefiltered_depth = module.whole(prefiltered[1]);

  const auto occlusion_extent = module.image_extent(context.ambient_occlusion_attachment).value_or(depth_extent);
  auto noisy_occlusion = module.transient_image({
    .extent = occlusion_extent,
    .format = vir::Format::R8Unorm,
    .name = "vbgtao noisy occlusion",
  });
  noisy_occlusion = module.clear(noisy_occlusion, vir::ClearValue::rgba_f32(1.0f, 1.0f, 1.0f, 1.0f));

  module
    .begin_compute(
      {{values.camera, vir::Access::ComputeUniformRead},
       {prefiltered_depth, vir::Access::ComputeSampled},
       {context.normal_attachment, vir::Access::ComputeSampled},
       {context.noise_attachment, vir::Access::ComputeSampled},
       {noisy_occlusion, vir::Access::ComputeRW},
       {context.depth_differences_attachment, vir::Access::ComputeRW}}
    )
    .with_name("vbgtao generate")
    .bind_compute_pipeline(render_context.pipeline("vbgtao_main"))
    .bind_buffer(0, 0, values.camera)
    .bind_image(0, 1, prefiltered_depth)
    .bind_image(0, 2, context.normal_attachment)
    .bind_image(0, 3, context.noise_attachment)
    .bind_image(0, 4, noisy_occlusion)
    .bind_image(0, 5, context.depth_differences_attachment)
    .bind_sampler(0, 6, nearest_clamped)
    .bind_sampler(0, 7, render_context.cached_sampler_handle(vir::LinearSamplerClamped))
    .push_constants_from(variables.vbgtao_settings)
    .dispatch_invocations_per_pixel(noisy_occlusion);
  std::tie(
    values.camera,
    std::ignore,
    context.normal_attachment,
    context.noise_attachment,
    noisy_occlusion,
    context.depth_differences_attachment
  ) = std::tuple_cat(module.end_compute<6>());

  module
    .begin_compute(
      {{noisy_occlusion, vir::Access::ComputeSampled},
       {context.depth_differences_attachment, vir::Access::ComputeSampled},
       {context.ambient_occlusion_attachment, vir::Access::ComputeRW}}
    )
    .with_name("vbgtao denoise")
    .bind_compute_pipeline(render_context.pipeline("vbgtao_denoise"))
    .bind_image(0, 0, noisy_occlusion)
    .bind_image(0, 1, context.depth_differences_attachment)
    .bind_image(0, 2, context.ambient_occlusion_attachment)
    .bind_sampler(0, 3, nearest_clamped)
    .push_constants_from(variables.vbgtao_denoise_push)
    .dispatch_invocations_per_pixel(context.ambient_occlusion_attachment);
  std::tie(std::ignore, context.depth_differences_attachment, context.ambient_occlusion_attachment) = std::tuple_cat(
    module.end_compute<3>()
  );
}

auto RendererInstance::bind_rtao_variables(
  this const RendererInstance& self, vir::Program& program, const PBRVariables& variables
) -> void {
  ZoneScoped;
  if (!program.has_variable(variables.rtao_push)) {
    return;
  }

  const auto num_frames = static_cast<u32>(self.renderer.render_context->num_frames);
  const auto& cvar = *self.frame_params.cvar;
  program.set_bytes(
    variables.rtao_push,
    RTAOPush(
      static_cast<u32>(std::max(cvar.cvar_rtao_ray_count.get(), 1)),
      cvar.cvar_rtao_radius.get(),
      cvar.cvar_rtao_power.get(),
      num_frames
    )
  );
}

auto RendererInstance::generate_rtao(
  this RendererInstance& self, vir::Module& module, FrameRecording& recording, RTAOContext& context
) -> void {
  ZoneScoped;
  auto& prepared = recording.prepared;
  auto& values = recording.values;

  auto& render_context = *self.renderer.render_context;
  auto& variables = prepared.pbr;
  variables.rtao_push = module.declare_bytes_var("rtao push", sizeof(RTAOPush));

  module
    .begin_compute(
      {{context.tlas_buffer, vir::Access::ComputeAccelerationStructureRead},
       {values.camera, vir::Access::ComputeUniformRead},
       {context.depth_attachment, vir::Access::ComputeSampled},
       {context.normal_attachment, vir::Access::ComputeSampled},
       {context.ambient_occlusion_attachment, vir::Access::ComputeRW}}
    )
    .with_name("rtao")
    .bind_compute_pipeline(render_context.pipeline("rtao"))
    .bind_acceleration_structure(0, 0, context.tlas_buffer, self.tlas_handle())
    .bind_buffer(0, 1, values.camera)
    .bind_image(0, 2, context.depth_attachment)
    .bind_image(0, 3, context.normal_attachment)
    .bind_image(0, 4, context.ambient_occlusion_attachment)
    .bind_sampler(0, 5, render_context.cached_sampler_handle(vir::NearestSamplerClamped))
    .push_constants_from(prepared.pbr.rtao_push)
    .dispatch_invocations_per_pixel(context.ambient_occlusion_attachment);
  std::tie(
    context.tlas_buffer,
    values.camera,
    context.depth_attachment,
    context.normal_attachment,
    context.ambient_occlusion_attachment
  ) = std::tuple_cat(module.end_compute<5>());
}

auto RendererInstance::bind_pbr_variables(
  this const RendererInstance& self, vir::Program& program, const PBRVariables& variables
) -> void {
  ZoneScoped;
  program.set_bytes(
    variables.sun_push,
    PBRSunPush(self.directional_light.direction, self.directional_light.intensity, glm::vec3(0.02f, 0.03f, 0.04f))
  );
  program.set_bytes(
    variables.lights_push,
    PBRLightsPush(static_cast<u32>(self.scene.lights.size()), self.light_grid_origin, 0_u32)
  );
  program.set_bytes(variables.sky, self.sky_data);
}

auto RendererInstance::apply_pbr(
  this RendererInstance& self, vir::Module& module, FrameRecording& recording, PBRContext& context, vir::ValueID dst
) -> vir::ValueID {
  ZoneScoped;
  auto& prepared = recording.prepared;
  auto& values = recording.values;

  auto& render_context = *self.renderer.render_context;
  auto& variables = prepared.pbr;
  variables.sun_push = module.declare_bytes_var("pbr apply push", sizeof(PBRSunPush));
  variables.lights_push = module.declare_bytes_var("pbr apply lights push", sizeof(PBRLightsPush));
  variables.sky = module.declare_bytes_var("sky", sizeof(GPU::Sky));

  const auto scene_flags = std::to_underlying(self.gpu_scene_flags);
  const auto linear_clamped = render_context.cached_sampler_handle(vir::LinearSamplerClamped);
  const auto linear_repeated = render_context.cached_sampler_handle(vir::LinearSamplerRepeated);

  if (self.gpu_scene_flags & GPU::SceneFlags::HasAtmosphere) {
    module
      .begin_rendering(
        {{dst, vir::Access::ColorWrite},
         {context.sky_transmittance_lut_attachment, vir::Access::FragmentSampled},
         {context.sky_aerial_perspective_lut_attachment, vir::Access::FragmentSampled},
         {context.sky_view_lut_attachment, vir::Access::FragmentSampled},
         {context.sky_cubemap_attachment, vir::Access::FragmentSampled},
         {context.depth_attachment, vir::Access::FragmentSampled},
         {context.albedo_attachment, vir::Access::FragmentSampled},
         {context.normal_attachment, vir::Access::FragmentSampled},
         {context.emissive_attachment, vir::Access::FragmentSampled},
         {context.metallic_roughness_occlusion_attachment, vir::Access::FragmentSampled},
         {context.ambient_occlusion_attachment, vir::Access::FragmentSampled},
         {context.shadows_attachment, vir::Access::FragmentSampled},
         {values.camera, vir::Access::FragmentUniformRead},
         {values.atmosphere, vir::Access::FragmentRead}}
      )
      .with_name("pbr apply")
      .bind_graphics_pipeline(render_context.pipeline("pbr_apply"))
      .set_viewport(0, vir::Rect2D::framebuffer())
      .set_scissor(0, vir::Rect2D::framebuffer())
      .bind_sampler(0, 0, linear_clamped)
      .bind_sampler(0, 1, linear_repeated)
      .bind_image(0, 2, context.sky_transmittance_lut_attachment)
      .bind_image(0, 3, context.sky_aerial_perspective_lut_attachment)
      .bind_image(0, 4, context.sky_cubemap_attachment)
      .bind_image(0, 5, context.sky_view_lut_attachment)
      .bind_image(0, 6, context.depth_attachment)
      .bind_image(0, 7, context.albedo_attachment)
      .bind_image(0, 8, context.normal_attachment)
      .bind_image(0, 9, context.emissive_attachment)
      .bind_image(0, 10, context.metallic_roughness_occlusion_attachment)
      .bind_image(0, 11, context.ambient_occlusion_attachment)
      .bind_image(0, 12, context.shadows_attachment)
      .bind_buffer(0, 13, values.camera)
      .push_constant_address(0, values.atmosphere)
      .push_constants_from_at(8, variables.sun_push)
      .specialize_constant(0, scene_flags)
      .draw(3, 1);
    std::tie(
      dst,
      context.sky_transmittance_lut_attachment,
      context.sky_aerial_perspective_lut_attachment,
      context.sky_view_lut_attachment,
      context.sky_cubemap_attachment,
      context.depth_attachment,
      context.albedo_attachment,
      context.normal_attachment,
      context.emissive_attachment,
      context.metallic_roughness_occlusion_attachment,
      context.ambient_occlusion_attachment,
      context.shadows_attachment,
      values.camera,
      values.atmosphere
    ) = std::tuple_cat(module.end_rendering<14>());
  } else {
    auto sky = module.transient_buffer({
      .size = sizeof(GPU::Sky),
      .usage = TRANSIENT_BUFFER_USAGE,
      .location = vir::MemoryLocation::GpuOnly,
      .name = "sky",
    });
    sky = module.update_buffer_from(sky, variables.sky);
    module
      .begin_rendering(
        {{dst, vir::Access::ColorWrite},
         {context.depth_attachment, vir::Access::FragmentSampled},
         {context.albedo_attachment, vir::Access::FragmentSampled},
         {context.normal_attachment, vir::Access::FragmentSampled},
         {context.emissive_attachment, vir::Access::FragmentSampled},
         {context.metallic_roughness_occlusion_attachment, vir::Access::FragmentSampled},
         {context.ambient_occlusion_attachment, vir::Access::FragmentSampled},
         {context.shadows_attachment, vir::Access::FragmentSampled},
         {values.camera, vir::Access::FragmentUniformRead},
         {sky, vir::Access::FragmentRead}}
      )
      .with_name("pbr apply")
      .bind_graphics_pipeline(render_context.pipeline("pbr_apply_no_atmos"))
      .set_viewport(0, vir::Rect2D::framebuffer())
      .set_scissor(0, vir::Rect2D::framebuffer())
      .bind_sampler(0, 0, linear_clamped)
      .bind_sampler(0, 1, linear_repeated)
      .bind_image(0, 2, context.depth_attachment)
      .bind_image(0, 3, context.albedo_attachment)
      .bind_image(0, 4, context.normal_attachment)
      .bind_image(0, 5, context.emissive_attachment)
      .bind_image(0, 6, context.metallic_roughness_occlusion_attachment)
      .bind_image(0, 7, context.ambient_occlusion_attachment)
      .bind_image(0, 8, context.shadows_attachment)
      .bind_buffer(0, 9, values.camera)
      .push_constant_address(0, sky)
      .push_constants_from_at(8, variables.sun_push)
      .specialize_constant(0, scene_flags)
      .draw(3, 1);
    std::tie(
      dst,
      context.depth_attachment,
      context.albedo_attachment,
      context.normal_attachment,
      context.emissive_attachment,
      context.metallic_roughness_occlusion_attachment,
      context.ambient_occlusion_attachment,
      context.shadows_attachment,
      values.camera,
      std::ignore
    ) = std::tuple_cat(module.end_rendering<10>());
  }

  static constexpr auto additive_blend = vir::ColorBlendAttachmentState{
    .is_blend_enabled = true,
    .src_color_blend_factor = VK_BLEND_FACTOR_ONE,
    .dst_color_blend_factor = VK_BLEND_FACTOR_ONE,
    .color_blend_op = VK_BLEND_OP_ADD,
    .src_alpha_blend_factor = VK_BLEND_FACTOR_ZERO,
    .dst_alpha_blend_factor = VK_BLEND_FACTOR_ONE,
    .alpha_blend_op = VK_BLEND_OP_ADD,
  };
  bind_vsm_pointspot_spec_constants(module
                                      .begin_rendering(
                                        {{dst, vir::Access::ColorRW},
                                         {context.depth_attachment, vir::Access::FragmentSampled},
                                         {context.albedo_attachment, vir::Access::FragmentSampled},
                                         {context.normal_attachment, vir::Access::FragmentSampled},
                                         {context.metallic_roughness_occlusion_attachment,
                                          vir::Access::FragmentSampled},
                                         {context.pointspot_page_table_attachment, vir::Access::FragmentSampled},
                                         {context.vsm_physical_pages_attachment, vir::Access::FragmentSampled},
                                         {values.camera, vir::Access::FragmentUniformRead},
                                         {values.lights, vir::Access::FragmentRead},
                                         {context.light_grid_buffer, vir::Access::FragmentRead},
                                         {context.pointspot_views_buffer, vir::Access::FragmentRead}}
                                      )
                                      .with_name("pbr apply lights")
                                      .bind_graphics_pipeline(render_context.pipeline("pbr_apply_lights")))
    .set_color_blend(0, additive_blend)
    .set_viewport(0, vir::Rect2D::framebuffer())
    .set_scissor(0, vir::Rect2D::framebuffer())
    .bind_buffer(0, 0, values.camera)
    .bind_image(0, 1, context.depth_attachment)
    .bind_image(0, 2, context.albedo_attachment)
    .bind_image(0, 3, context.normal_attachment)
    .bind_image(0, 4, context.metallic_roughness_occlusion_attachment)
    .bind_image(0, 5, context.pointspot_page_table_attachment)
    .bind_image(0, 6, context.vsm_physical_pages_attachment)
    .push_constant_address(0, values.lights)
    .push_constant_address(8, context.light_grid_buffer)
    .push_constant_address(16, context.pointspot_views_buffer)
    .push_constants_from_at(24, variables.lights_push)
    .draw(3, 1);
  std::tie(
    dst,
    context.depth_attachment,
    context.albedo_attachment,
    context.normal_attachment,
    context.metallic_roughness_occlusion_attachment,
    context.pointspot_page_table_attachment,
    context.vsm_physical_pages_attachment,
    values.camera,
    values.lights,
    context.light_grid_buffer,
    context.pointspot_views_buffer
  ) = std::tuple_cat(module.end_rendering<11>());

  return dst;
}
} // namespace ox
