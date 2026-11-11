#include <vir/RenderGraph.hpp>
#include <vir/Types.hpp>

#include "Render/RenderContext.hpp"
#include "Render/Renderer.hpp"
#include "Render/RendererInstance.hpp"
#include "Render/Utils/PushConstants.hpp"
#include "Render/Utils/Samplers.hpp"
#include "Scene/Scene.hpp"
#include "Utils/Log.hpp"

namespace ox {
using RTGIGBufferPush = PushConstants<glm::uvec2>;
using RTGITracePush = PushConstants<
  u32,
  u32,
  u32,
  u32,
  u32,
  u32,
  u32,
  u32,
  f32,
  f32,
  f32,
  f32,
  f32,
  f32,
  f32,
  glm::vec3,
  glm::vec3,
  glm::vec3>;

constexpr static auto TRANSIENT_BUFFER_USAGE = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

constexpr static auto RTGI_TRACE_GROUP_WIDTH = 128_u32;
constexpr static auto RTGI_POST_BLUR_ALONG = 16_u32;
constexpr static auto RTGI_POST_BLUR_ACROSS = 4_u32;

static auto rtgi_half_extent(const glm::uvec2 render_size) -> vir::Extent3D {
  return {(render_size.x + 1_u32) / 2_u32, (render_size.y + 1_u32) / 2_u32, 1};
}

static auto rtgi_ray_list_limit(const glm::uvec2 half_size, const RendererCVar& cvar) -> u32 {
  const auto ray_percentage = std::clamp(
    cvar.cvar_rtgi_ray_percentage.get(),
    0.05f,
    static_cast<f32>(GPU::RTGI_RAY_LIST_CAPACITY_MUL)
  );
  const auto min_budget = std::clamp(cvar.cvar_rtgi_min_ray_budget.get(), 0.0f, 1.0f);
  const auto budget = static_cast<u32>(
    static_cast<f32>(half_size.x * half_size.y) * std::max(ray_percentage, min_budget)
  );
  return std::min(budget, half_size.x * half_size.y * GPU::RTGI_RAY_LIST_CAPACITY_MUL);
}

auto RendererInstance::bind_rtgi_variables(
  this const RendererInstance& self, vir::Program& program, const RTGIVariables& variables
) -> void {
  ZoneScoped;

  if (!program.has_variable(variables.is_reset)) {
    return;
  }

  const auto& params = self.frame_params;
  const auto& cvar = *params.cvar;
  program.set(variables.is_reset, params.rtgi_reset);

  const auto half_extent = rtgi_half_extent(params.render_size);
  const auto ray_list_limit = rtgi_ray_list_limit({half_extent.width, half_extent.height}, cvar);
  program.set(variables.trace_groups, (ray_list_limit + RTGI_TRACE_GROUP_WIDTH - 1_u32) / RTGI_TRACE_GROUP_WIDTH);
  program.set_bytes(
    variables.trace_push,
    RTGITracePush(
      static_cast<u32>(self.probe_volumes.size()),
      static_cast<u32>(self.scene.lights.size()),
      static_cast<u32>(params.ddgi_history_valid && cvar.cvar_rtgi_ddgi_bounce.as_bool()),
      GPU::ddgi_radiance_atlas_y_offset(params.ddgi_total_probe_count),
      static_cast<u32>(params.ddgi_distance_culling_enabled),
      params.frame_index & 0xFFF_u32,
      ray_list_limit,
      static_cast<u32>(params.shape.has_rtgi_pioneer),
      cvar.cvar_rtgi_max_distance.get(),
      cvar.cvar_ddgi_shadow_ray_offset.get(),
      cvar.cvar_ddgi_normal_bias.get(),
      cvar.cvar_ddgi_view_bias.get(),
      self.directional_light.intensity,
      std::clamp(cvar.cvar_rtgi_bounce_range.get(), 0.0f, 16.0f),
      std::clamp(cvar.cvar_rtgi_guide_concentration.get(), 0.0f, 0.999f),
      self.directional_light.direction,
      self.sky_data.ambient_color,
      self.light_grid_origin
    )
  );

  program.set_bytes(
    variables.settings,
    GPU::RTGISettings{
      .half_size = {half_extent.width, half_extent.height},
      .max_temporal_samples = static_cast<f32>(std::clamp(cvar.cvar_rtgi_max_temporal_samples.get(), 1, 1024)),
      .fast_convergence_samples = std::max(cvar.cvar_rtgi_fast_convergence_samples.get(), 0.0f),
      .temporal_fast_history_frames = static_cast<u32>(
        std::clamp(cvar.cvar_rtgi_temporal_fast_history_frames.get(), 1, 15)
      ),
      .post_blur_max_width = static_cast<u32>(std::clamp(cvar.cvar_rtgi_post_blur_max_width.get(), 1, 16)),
      .is_eye_adapted = static_cast<u32>(params.shape.has_eye_adaptation),
      .fixed_exposure = cvar.cvar_exposure.get(),
      .ray_percentage = std::clamp(cvar.cvar_rtgi_ray_percentage.get(), 0.05f, 4.0f),
      .min_ray_budget = std::clamp(cvar.cvar_rtgi_min_ray_budget.get(), 0.0f, 1.0f),
      .max_rays_per_pixel = static_cast<u32>(std::clamp(cvar.cvar_rtgi_max_rays_per_pixel.get(), 1, 12)),
      .frame_index = params.frame_index & 0xFFF_u32,
      .firefly_filter_enabled = static_cast<u32>(cvar.cvar_rtgi_firefly_filter.as_bool()),
      .firefly_perceptual_tolerance = std::clamp(cvar.cvar_rtgi_firefly_tolerance.get(), 0.0f, 16.0f),
      .firefly_clamp_mode = 0,
      .pre_blur_iterations = std::max(params.shape.rtgi_pre_blur_iterations, 1_u32),
      .pre_blur_base_width = std::clamp(cvar.cvar_rtgi_pre_blur_width.get(), 1.0f, 64.0f),
    }
  );

  const auto current_index = params.rtgi_history_ping ? 1 : 0;
  const auto previous_index = 1 - current_index;
  program.set(variables.half_res_depth, self.rtgi_half_res_depth[current_index].attachment_info());
  program.set(variables.previous_half_res_depth, self.rtgi_half_res_depth[previous_index].attachment_info());
  program.set(variables.half_res_face_normal, self.rtgi_half_res_face_normal[current_index].attachment_info());
  program.set(
    variables.previous_half_res_face_normal,
    self.rtgi_half_res_face_normal[previous_index].attachment_info()
  );
  program.set(
    variables.half_res_normal_roughness,
    self.rtgi_half_res_normal_roughness[current_index].attachment_info()
  );
  program.set(
    variables.previous_half_res_normal_roughness,
    self.rtgi_half_res_normal_roughness[previous_index].attachment_info()
  );

  program.set(variables.half_res_sample_count, self.rtgi_half_res_sample_count[current_index].attachment_info());
  program.set(
    variables.previous_half_res_sample_count,
    self.rtgi_half_res_sample_count[previous_index].attachment_info()
  );
  program.set(variables.half_res_diffuse, self.rtgi_half_res_diffuse[current_index].attachment_info());
  program.set(variables.previous_half_res_diffuse, self.rtgi_half_res_diffuse[previous_index].attachment_info());
  program.set(variables.half_res_diffuse2, self.rtgi_half_res_diffuse2[current_index].attachment_info());
  program.set(variables.previous_half_res_diffuse2, self.rtgi_half_res_diffuse2[previous_index].attachment_info());
  program.set(variables.fast_temporal_history, self.rtgi_fast_temporal_history[current_index].attachment_info());
  program.set(
    variables.previous_fast_temporal_history,
    self.rtgi_fast_temporal_history[previous_index].attachment_info()
  );
  program.set(variables.half_res_ao_guide, self.rtgi_half_res_ao_guide[current_index].attachment_info());
  program.set(variables.previous_half_res_ao_guide, self.rtgi_half_res_ao_guide[previous_index].attachment_info());
  program.set(
    variables.temporal_perceptual_radiance,
    self.rtgi_temporal_perceptual_radiance[current_index].attachment_info()
  );
  program.set(
    variables.previous_temporal_perceptual_radiance,
    self.rtgi_temporal_perceptual_radiance[previous_index].attachment_info()
  );
}

auto RendererInstance::allocate_rtgi_resources(this RendererInstance& self, const glm::uvec2 render_size) -> void {
  ZoneScoped;

  if (self.rtgi_render_size == render_size) {
    return;
  }

  constexpr auto history_usage = vir::ImageUsageFlags::Storage | vir::ImageUsageFlags::Sampled |
                                 vir::ImageUsageFlags::TransferDst;
  const auto half_extent = rtgi_half_extent(render_size);
  for (auto i = 0_u32; i < 2; i++) {
    self.rtgi_half_res_depth[i] = Texture::create(
      {.format = vir::Format::R32Sfloat, .extent = half_extent, .usage = history_usage}
    );
    self.rtgi_half_res_face_normal[i] = Texture::create(
      {.format = vir::Format::R32Uint, .extent = half_extent, .usage = history_usage}
    );
    self.rtgi_half_res_normal_roughness[i] = Texture::create(
      {.format = vir::Format::R32Uint, .extent = half_extent, .usage = history_usage}
    );
    self.rtgi_half_res_sample_count[i] = Texture::create(
      {.format = vir::Format::R16Uint, .extent = half_extent, .usage = history_usage}
    );
    self.rtgi_half_res_diffuse[i] = Texture::create(
      {.format = vir::Format::RGBA16Sfloat, .extent = half_extent, .usage = history_usage}
    );
    self.rtgi_half_res_diffuse2[i] = Texture::create(
      {.format = vir::Format::RG16Sfloat, .extent = half_extent, .usage = history_usage}
    );
    self.rtgi_fast_temporal_history[i] = Texture::create(
      {.format = vir::Format::RG16Sfloat, .extent = half_extent, .usage = history_usage}
    );
    self.rtgi_half_res_ao_guide[i] = Texture::create(
      {.format = vir::Format::R16Sfloat, .extent = half_extent, .usage = history_usage}
    );
    self.rtgi_temporal_perceptual_radiance[i] = Texture::create(
      {.format = vir::Format::R16Sfloat, .extent = half_extent, .usage = history_usage}
    );
  }

  auto module = vir::Module{};
  const auto black = vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 0.0f);
  const auto zero = vir::ClearValue::rgba_u32(0, 0, 0, 0);
  for (auto i = 0_u32; i < 2; i++) {
    for (const auto* texture :
         {&self.rtgi_half_res_depth[i],
          &self.rtgi_half_res_diffuse[i],
          &self.rtgi_half_res_diffuse2[i],
          &self.rtgi_fast_temporal_history[i],
          &self.rtgi_half_res_ao_guide[i],
          &self.rtgi_temporal_perceptual_radiance[i]}) {
      const auto cleared = module.clear(texture->import_discarded(module, vir::Access::None), black);
      module.export_to(cleared, RTGIContext::HISTORY_REST);
    }

    for (const auto* texture :
         {&self.rtgi_half_res_face_normal[i],
          &self.rtgi_half_res_normal_roughness[i],
          &self.rtgi_half_res_sample_count[i]}) {
      const auto cleared = module.clear(texture->import_discarded(module, vir::Access::None), zero);
      module.export_to(cleared, RTGIContext::HISTORY_REST);
    }
  }

  if (const auto initialized = self.renderer.render_context->execute_blocking(std::move(module)); !initialized) {
    OX_LOG_FATAL("failed to initialize the RTGI history ({})", std::to_underlying(initialized.error()));
  }

  self.rtgi_render_size = render_size;
  self.rtgi_history_valid = false;
}

auto RendererInstance::record_rtgi(
  this RendererInstance& self,
  vir::Module& module,
  RenderContext& render_context,
  FrameRecording& recording,
  RTGIContext& context
) -> void {
  ZoneScoped;
  auto& prepared = recording.prepared;
  auto& values = recording.values;

  auto& variables = prepared.rtgi;
  variables.is_reset = module.declare_bool_var("rtgi reset", false);
  variables.half_res_depth = module.declare_image_var(
    "rtgi half res depth",
    vir::Format::R32Sfloat,
    RTGIContext::HISTORY_REST
  );
  variables.previous_half_res_depth = module.declare_image_var(
    "rtgi previous half res depth",
    vir::Format::R32Sfloat,
    RTGIContext::HISTORY_REST
  );
  variables.half_res_face_normal = module.declare_image_var(
    "rtgi half res face normal",
    vir::Format::R32Uint,
    RTGIContext::HISTORY_REST
  );
  variables.previous_half_res_face_normal = module.declare_image_var(
    "rtgi previous half res face normal",
    vir::Format::R32Uint,
    RTGIContext::HISTORY_REST
  );
  variables.half_res_normal_roughness = module.declare_image_var(
    "rtgi half res normal roughness",
    vir::Format::R32Uint,
    RTGIContext::HISTORY_REST
  );
  variables.previous_half_res_normal_roughness = module.declare_image_var(
    "rtgi previous half res normal roughness",
    vir::Format::R32Uint,
    RTGIContext::HISTORY_REST
  );
  variables.half_res_sample_count = module.declare_image_var(
    "rtgi half res sample count",
    vir::Format::R16Uint,
    RTGIContext::HISTORY_REST
  );
  variables.previous_half_res_sample_count = module.declare_image_var(
    "rtgi previous half res sample count",
    vir::Format::R16Uint,
    RTGIContext::HISTORY_REST
  );
  variables.half_res_diffuse = module.declare_image_var(
    "rtgi half res diffuse",
    vir::Format::RGBA16Sfloat,
    RTGIContext::HISTORY_REST
  );
  variables.previous_half_res_diffuse = module.declare_image_var(
    "rtgi previous half res diffuse",
    vir::Format::RGBA16Sfloat,
    RTGIContext::HISTORY_REST
  );
  variables.half_res_diffuse2 = module.declare_image_var(
    "rtgi half res diffuse2",
    vir::Format::RG16Sfloat,
    RTGIContext::HISTORY_REST
  );
  variables.previous_half_res_diffuse2 = module.declare_image_var(
    "rtgi previous half res diffuse2",
    vir::Format::RG16Sfloat,
    RTGIContext::HISTORY_REST
  );
  variables.fast_temporal_history = module.declare_image_var(
    "rtgi fast temporal history",
    vir::Format::RG16Sfloat,
    RTGIContext::HISTORY_REST
  );
  variables.previous_fast_temporal_history = module.declare_image_var(
    "rtgi previous fast temporal history",
    vir::Format::RG16Sfloat,
    RTGIContext::HISTORY_REST
  );
  variables.half_res_ao_guide = module.declare_image_var(
    "rtgi half res ao guide",
    vir::Format::R16Sfloat,
    RTGIContext::HISTORY_REST
  );
  variables.previous_half_res_ao_guide = module.declare_image_var(
    "rtgi previous half res ao guide",
    vir::Format::R16Sfloat,
    RTGIContext::HISTORY_REST
  );
  variables.temporal_perceptual_radiance = module.declare_image_var(
    "rtgi temporal perceptual radiance",
    vir::Format::R16Sfloat,
    RTGIContext::HISTORY_REST
  );
  variables.previous_temporal_perceptual_radiance = module.declare_image_var(
    "rtgi previous temporal perceptual radiance",
    vir::Format::R16Sfloat,
    RTGIContext::HISTORY_REST
  );
  variables.trace_push = module.declare_bytes_var("rtgi trace push", sizeof(RTGITracePush));
  variables.trace_groups = module.declare_u32_var("rtgi trace groups", 0);
  variables.settings = module.declare_bytes_var("rtgi settings", sizeof(GPU::RTGISettings));

  context.half_res_depth = variables.half_res_depth;
  context.half_res_face_normal = variables.half_res_face_normal;
  context.half_res_normal_roughness = variables.half_res_normal_roughness;
  context.half_res_sample_count = variables.half_res_sample_count;
  context.half_res_diffuse = variables.half_res_diffuse;
  context.half_res_diffuse2 = variables.half_res_diffuse2;
  context.fast_temporal_history = variables.fast_temporal_history;
  context.half_res_ao_guide = variables.half_res_ao_guide;
  context.temporal_perceptual_radiance = variables.temporal_perceptual_radiance;

  // after a reset a depth of 0 (sky) and a count of 0 make every pixel look disoccluded
  std::tie(
    context.previous_half_res_depth,
    context.previous_half_res_face_normal,
    context.previous_half_res_normal_roughness,
    context.previous_half_res_sample_count,
    context.previous_half_res_diffuse,
    context.previous_half_res_diffuse2,
    context.previous_fast_temporal_history,
    context.previous_half_res_ao_guide,
    context.previous_temporal_perceptual_radiance
  ) = std::tuple_cat( //
    module.set_condition(
      variables.is_reset,
      [&](vir::Module& arm) {
        const auto black = vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 0.0f);
        const auto zero = vir::ClearValue::rgba_u32(0, 0, 0, 0);
        return std::array{
          arm.clear(variables.previous_half_res_depth, black),
          arm.clear(variables.previous_half_res_face_normal, zero),
          arm.clear(variables.previous_half_res_normal_roughness, zero),
          arm.clear(variables.previous_half_res_sample_count, zero),
          arm.clear(variables.previous_half_res_diffuse, black),
          arm.clear(variables.previous_half_res_diffuse2, black),
          arm.clear(variables.previous_fast_temporal_history, black),
          arm.clear(variables.previous_half_res_ao_guide, black),
          arm.clear(variables.previous_temporal_perceptual_radiance, black),
        };
      },
      [&](vir::Module&) {
        return std::array{
          variables.previous_half_res_depth,
          variables.previous_half_res_face_normal,
          variables.previous_half_res_normal_roughness,
          variables.previous_half_res_sample_count,
          variables.previous_half_res_diffuse,
          variables.previous_half_res_diffuse2,
          variables.previous_fast_temporal_history,
          variables.previous_half_res_ao_guide,
          variables.previous_temporal_perceptual_radiance,
        };
      }
    )
  );

  const auto& shape = recording.shape;
  const auto half_extent = rtgi_half_extent(shape.render_size);
  context.half_res_albedo_metalness = module.transient_image(
    {.extent = half_extent, .format = vir::Format::RGBA8Unorm, .name = "rtgi half res albedo metalness"}
  );

  std::tie(
    values.camera,
    context.depth_attachment,
    context.albedo_attachment,
    context.normal_attachment,
    context.metallic_roughness_occlusion_attachment,
    context.half_res_depth,
    context.half_res_face_normal,
    context.half_res_normal_roughness,
    context.half_res_albedo_metalness
  ) = std::tuple_cat( //
    module
      .begin_compute(
        {{values.camera, vir::Access::ComputeUniformRead},
         {context.depth_attachment, vir::Access::ComputeSampled},
         {context.albedo_attachment, vir::Access::ComputeSampled},
         {context.normal_attachment, vir::Access::ComputeSampled},
         {context.metallic_roughness_occlusion_attachment, vir::Access::ComputeSampled},
         {context.half_res_depth, vir::Access::ComputeWrite},
         {context.half_res_face_normal, vir::Access::ComputeWrite},
         {context.half_res_normal_roughness, vir::Access::ComputeWrite},
         {context.half_res_albedo_metalness, vir::Access::ComputeWrite}}
      )
      .with_name("rtgi gbuffer")
      .bind_compute_pipeline(render_context.pipeline("rtgi_gbuffer"))
      .bind_buffer(0, 0, values.camera)
      .bind_image(0, 1, context.depth_attachment)
      .bind_image(0, 2, context.albedo_attachment)
      .bind_image(0, 3, context.normal_attachment)
      .bind_image(0, 4, context.metallic_roughness_occlusion_attachment)
      .bind_image(0, 5, context.half_res_depth)
      .bind_image(0, 6, context.half_res_face_normal)
      .bind_image(0, 7, context.half_res_normal_roughness)
      .bind_image(0, 8, context.half_res_albedo_metalness)
      .push_constants(RTGIGBufferPush(shape.render_size))
      .dispatch_invocations(half_extent.width, half_extent.height, 1)
      .end_compute()
  );

  const auto pixel_count = half_extent.width * half_extent.height;
  const auto ray_capacity = pixel_count * GPU::RTGI_RAY_LIST_CAPACITY_MUL;
  auto ray_count_image = module.transient_image(
    {.extent = half_extent, .format = vir::Format::R8Uint, .name = "rtgi ray count"}
  );
  auto pixel_ray_alloc = module.transient_image(
    {.extent = half_extent, .format = vir::Format::R32Uint, .name = "rtgi pixel ray alloc"}
  );
  auto ray_list = module.transient_buffer({
    .size = ray_capacity * sizeof(GPU::RTGIRayEntry),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "rtgi ray list",
  });
  auto ray_result = module.transient_buffer({
    .size = ray_capacity * sizeof(GPU::RTGIRayResult),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "rtgi ray result",
  });
  const auto zero_counters = GPU::RTGIRayCounters{};
  auto counters = module.transient_buffer({
    .size = sizeof(zero_counters),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "rtgi ray counters",
  });
  counters = module.update_buffer(
    counters,
    std::span{reinterpret_cast<const u8*>(&zero_counters), sizeof(zero_counters)}
  );

  auto ray_impact = module.transient_image(
    {.extent = half_extent, .format = vir::Format::RGBA8Unorm, .name = "rtgi ray impact"}
  );

  auto reproject_corner = module.transient_image(
    {.extent = half_extent, .format = vir::Format::RG16Uint, .name = "rtgi reproject corner"}
  );
  auto reproject_weights = module.transient_image(
    {.extent = half_extent, .format = vir::Format::RGBA8Unorm, .name = "rtgi reproject weights"}
  );
  std::tie(
    values.camera,
    context.half_res_depth,
    context.half_res_face_normal,
    context.previous_half_res_depth,
    context.previous_half_res_face_normal,
    context.previous_half_res_sample_count,
    context.half_res_sample_count,
    reproject_corner,
    reproject_weights,
    ray_impact,
    counters
  ) = std::tuple_cat( //
    module
      .begin_compute(
        {{values.camera, vir::Access::ComputeUniformRead},
         {context.half_res_depth, vir::Access::ComputeSampled},
         {context.half_res_face_normal, vir::Access::ComputeSampled},
         {context.previous_half_res_depth, vir::Access::ComputeSampled},
         {context.previous_half_res_face_normal, vir::Access::ComputeSampled},
         {context.previous_half_res_sample_count, vir::Access::ComputeSampled},
         {context.half_res_sample_count, vir::Access::ComputeWrite},
         {reproject_corner, vir::Access::ComputeWrite},
         {reproject_weights, vir::Access::ComputeWrite},
         {ray_impact, vir::Access::ComputeWrite},
         {counters, vir::Access::ComputeRW}}
      )
      .with_name("rtgi temporal reproject")
      .bind_compute_pipeline(render_context.pipeline("rtgi_temporal_reproject"))
      .bind_buffer(0, 0, values.camera)
      .bind_image(0, 1, context.half_res_depth)
      .bind_image(0, 2, context.half_res_face_normal)
      .bind_image(0, 3, context.previous_half_res_depth)
      .bind_image(0, 4, context.previous_half_res_face_normal)
      .bind_image(0, 5, context.previous_half_res_sample_count)
      .bind_image(0, 6, context.half_res_sample_count)
      .bind_image(0, 7, reproject_corner)
      .bind_image(0, 8, reproject_weights)
      .bind_image(0, 9, ray_impact)
      .bind_buffer(0, 10, counters)
      .push_constants_from(variables.settings)
      .dispatch_invocations(half_extent.width, half_extent.height, 1)
      .end_compute()
  );

  std::tie(
    context.half_res_sample_count,
    ray_impact,
    ray_count_image,
    pixel_ray_alloc,
    ray_list,
    counters
  ) = std::tuple_cat( //
    module
      .begin_compute(
        {{context.half_res_sample_count, vir::Access::ComputeSampled},
         {ray_impact, vir::Access::ComputeSampled},
         {ray_count_image, vir::Access::ComputeWrite},
         {pixel_ray_alloc, vir::Access::ComputeWrite},
         {ray_list, vir::Access::ComputeWrite},
         {counters, vir::Access::ComputeRW}}
      )
      .with_name("rtgi distribute")
      .bind_compute_pipeline(render_context.pipeline("rtgi_distribute"))
      .bind_image(0, 0, context.half_res_sample_count)
      .bind_image(0, 1, ray_impact)
      .bind_image(0, 2, ray_count_image)
      .bind_image(0, 3, pixel_ray_alloc)
      .bind_buffer(0, 4, ray_list)
      .bind_buffer(0, 5, counters)
      .push_constants_from(variables.settings)
      .dispatch_invocations(half_extent.width, half_extent.height, 1)
      .end_compute()
  );

  const auto pioneer_grid = glm::uvec2(
    (half_extent.width + GPU::RTGI_PIONEER_GRID_DIV - 1_u32) / GPU::RTGI_PIONEER_GRID_DIV,
    (half_extent.height + GPU::RTGI_PIONEER_GRID_DIV - 1_u32) / GPU::RTGI_PIONEER_GRID_DIV
  );
  const auto pioneer_extent = vir::Extent3D{pioneer_grid.x, pioneer_grid.y, 1};
  auto pioneer_guide_hit_y = module.transient_image(
    {.extent = pioneer_extent, .format = vir::Format::RGBA32Sfloat, .name = "rtgi pioneer guide hit y"}
  );
  if (shape.has_rtgi_pioneer) {
    auto pioneer_hit_y = module.transient_image(
      {.extent = pioneer_extent, .format = vir::Format::RGBA32Sfloat, .name = "rtgi pioneer hit y"}
    );
    auto pioneer_pass = //
      module
        .begin_compute(
          {{context.tlas_buffer, vir::Access::RayTracingRead},
           {context.probe_volumes_buffer, vir::Access::RayTracingRead},
           {values.mesh_instances, vir::Access::RayTracingRead},
           {values.meshes, vir::Access::RayTracingRead},
           {values.transforms_world, vir::Access::RayTracingRead},
           {values.materials, vir::Access::RayTracingRead},
           {values.lights, vir::Access::RayTracingRead},
           {context.sky_view_lut_attachment, vir::Access::RayTracingSampled},
           {context.sky_transmittance_lut_attachment, vir::Access::RayTracingSampled},
           {context.irradiance_attachment, vir::Access::RayTracingSampled},
           {context.distance_attachment, vir::Access::RayTracingSampled},
           {context.probe_states_buffer, vir::Access::RayTracingRead},
           {context.pointspot_page_table_attachment, vir::Access::RayTracingSampled},
           {context.vsm_physical_pages_attachment, vir::Access::RayTracingSampled},
           {values.camera, vir::Access::RayTracingUniformRead},
           {context.half_res_depth, vir::Access::RayTracingSampled},
           {context.half_res_face_normal, vir::Access::RayTracingSampled},
           {pioneer_hit_y, vir::Access::RayTracingWrite},
           {values.atmosphere, vir::Access::RayTracingRead},
           {context.light_grid_buffer, vir::Access::RayTracingRead},
           {context.pointspot_views_buffer, vir::Access::RayTracingRead},
           {context.probe_requests_buffer, vir::Access::RayTracingRW}}
        )
        .with_name("rtgi pioneer trace")
        .bind_ray_tracing_pipeline(render_context.pipeline("rtgi_pioneer"));
    std::tie(
      context.tlas_buffer,
      context.probe_volumes_buffer,
      values.mesh_instances,
      values.meshes,
      values.transforms_world,
      values.materials,
      values.lights,
      context.sky_view_lut_attachment,
      context.sky_transmittance_lut_attachment,
      context.irradiance_attachment,
      context.distance_attachment,
      context.probe_states_buffer,
      context.pointspot_page_table_attachment,
      context.vsm_physical_pages_attachment,
      values.camera,
      context.half_res_depth,
      context.half_res_face_normal,
      pioneer_hit_y,
      values.atmosphere,
      context.light_grid_buffer,
      context.pointspot_views_buffer,
      context.probe_requests_buffer
    ) = std::tuple_cat( //
      bind_vsm_pointspot_spec_constants(pioneer_pass)
        .bind_acceleration_structure(0, 0, context.tlas_buffer, self.tlas_handle())
        .bind_buffer(0, 1, context.probe_volumes_buffer)
        .bind_buffer(0, 2, values.mesh_instances)
        .bind_buffer(0, 3, values.meshes)
        .bind_buffer(0, 4, values.transforms_world)
        .bind_buffer(0, 5, values.materials)
        .bind_buffer(0, 6, values.lights)
        .bind_image(0, 7, context.sky_view_lut_attachment)
        .bind_image(0, 8, context.sky_transmittance_lut_attachment)
        .bind_sampler(0, 9, render_context.cached_sampler_handle(LinearSamplerClamped))
        .bind_image(0, 10, context.irradiance_attachment)
        .bind_image(0, 11, context.distance_attachment)
        .bind_buffer(0, 12, context.probe_states_buffer)
        .bind_image(0, 13, context.pointspot_page_table_attachment)
        .bind_image(0, 14, context.vsm_physical_pages_attachment)
        .bind_buffer(0, 15, values.camera)
        .bind_image(0, 16, context.half_res_depth)
        .bind_image(0, 17, context.half_res_face_normal)
        .bind_image(0, 18, pioneer_hit_y)
        .bind_buffer(0, 23, context.probe_requests_buffer)
        .specialize_constant(0, std::to_underlying(shape.scene_flags))
        .push_constant_address(0, values.atmosphere)
        .push_constant_address(8, context.light_grid_buffer)
        .push_constant_address(16, context.pointspot_views_buffer)
        .push_constants_from_at(24, variables.trace_push)
        .trace_rays(pioneer_grid.x, pioneer_grid.y, 1)
        .end_compute()
    );

    auto h_resample_hit_y = module.transient_image(
      {.extent = pioneer_extent, .format = vir::Format::RGBA32Sfloat, .name = "rtgi guide resample h hit y"}
    );
    auto h_resample_weight = module.transient_image(
      {.extent = pioneer_extent, .format = vir::Format::RG32Sfloat, .name = "rtgi guide resample h weight"}
    );
    std::tie(
      values.camera,
      context.half_res_depth,
      pioneer_hit_y,
      h_resample_hit_y,
      h_resample_weight
    ) = std::tuple_cat( //
      module
        .begin_compute(
          {{values.camera, vir::Access::ComputeUniformRead},
           {context.half_res_depth, vir::Access::ComputeSampled},
           {pioneer_hit_y, vir::Access::ComputeSampled},
           {h_resample_hit_y, vir::Access::ComputeWrite},
           {h_resample_weight, vir::Access::ComputeWrite}}
        )
        .with_name("rtgi guide resample horizontal")
        .bind_compute_pipeline(render_context.pipeline("rtgi_guide_resample_h"))
        .bind_buffer(0, 0, values.camera)
        .bind_image(0, 1, context.half_res_depth)
        .bind_image(0, 2, pioneer_hit_y)
        .bind_image(0, 3, h_resample_hit_y)
        .bind_image(0, 4, h_resample_weight)
        .push_constants_from(variables.settings)
        .dispatch_invocations(pioneer_grid.x, pioneer_grid.y, 1)
        .end_compute()
    );

    std::tie(
      values.camera,
      context.half_res_depth,
      h_resample_hit_y,
      h_resample_weight,
      pioneer_guide_hit_y
    ) = std::tuple_cat( //
      module
        .begin_compute(
          {{values.camera, vir::Access::ComputeUniformRead},
           {context.half_res_depth, vir::Access::ComputeSampled},
           {h_resample_hit_y, vir::Access::ComputeSampled},
           {h_resample_weight, vir::Access::ComputeSampled},
           {pioneer_guide_hit_y, vir::Access::ComputeWrite}}
        )
        .with_name("rtgi guide resample vertical")
        .bind_compute_pipeline(render_context.pipeline("rtgi_guide_resample_v"))
        .bind_buffer(0, 0, values.camera)
        .bind_image(0, 1, context.half_res_depth)
        .bind_image(0, 2, h_resample_hit_y)
        .bind_image(0, 3, h_resample_weight)
        .bind_image(0, 4, pioneer_guide_hit_y)
        .push_constants_from(variables.settings)
        .dispatch_invocations(pioneer_grid.x, pioneer_grid.y, 1)
        .end_compute()
    );
  } else {
    pioneer_guide_hit_y = module.clear(pioneer_guide_hit_y, vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 0.0f));
  }

  auto trace_pass = //
    module
      .begin_compute(
        {{context.tlas_buffer, vir::Access::RayTracingRead},
         {context.probe_volumes_buffer, vir::Access::RayTracingRead},
         {values.mesh_instances, vir::Access::RayTracingRead},
         {values.meshes, vir::Access::RayTracingRead},
         {values.transforms_world, vir::Access::RayTracingRead},
         {values.materials, vir::Access::RayTracingRead},
         {values.lights, vir::Access::RayTracingRead},
         {context.sky_view_lut_attachment, vir::Access::RayTracingSampled},
         {context.sky_transmittance_lut_attachment, vir::Access::RayTracingSampled},
         {context.irradiance_attachment, vir::Access::RayTracingSampled},
         {context.distance_attachment, vir::Access::RayTracingSampled},
         {context.probe_states_buffer, vir::Access::RayTracingRead},
         {context.pointspot_page_table_attachment, vir::Access::RayTracingSampled},
         {context.vsm_physical_pages_attachment, vir::Access::RayTracingSampled},
         {values.camera, vir::Access::RayTracingUniformRead},
         {context.half_res_depth, vir::Access::RayTracingSampled},
         {context.half_res_face_normal, vir::Access::RayTracingSampled},
         {context.half_res_sample_count, vir::Access::RayTracingSampled},
         {pioneer_guide_hit_y, vir::Access::RayTracingSampled},
         {ray_list, vir::Access::RayTracingRead},
         {counters, vir::Access::RayTracingRead},
         {ray_result, vir::Access::RayTracingWrite},
         {values.atmosphere, vir::Access::RayTracingRead},
         {context.light_grid_buffer, vir::Access::RayTracingRead},
         {context.pointspot_views_buffer, vir::Access::RayTracingRead},
         {context.probe_requests_buffer, vir::Access::RayTracingRW}}
      )
      .with_name("rtgi trace")
      .bind_ray_tracing_pipeline(render_context.pipeline("rtgi_trace"));
  std::tie(
    context.tlas_buffer,
    context.probe_volumes_buffer,
    values.mesh_instances,
    values.meshes,
    values.transforms_world,
    values.materials,
    values.lights,
    context.sky_view_lut_attachment,
    context.sky_transmittance_lut_attachment,
    context.irradiance_attachment,
    context.distance_attachment,
    context.probe_states_buffer,
    context.pointspot_page_table_attachment,
    context.vsm_physical_pages_attachment,
    values.camera,
    context.half_res_depth,
    context.half_res_face_normal,
    context.half_res_sample_count,
    pioneer_guide_hit_y,
    ray_list,
    counters,
    ray_result,
    values.atmosphere,
    context.light_grid_buffer,
    context.pointspot_views_buffer,
    context.probe_requests_buffer
  ) = std::tuple_cat( //
    bind_vsm_pointspot_spec_constants(trace_pass)
      .bind_acceleration_structure(0, 0, context.tlas_buffer, self.tlas_handle())
      .bind_buffer(0, 1, context.probe_volumes_buffer)
      .bind_buffer(0, 2, values.mesh_instances)
      .bind_buffer(0, 3, values.meshes)
      .bind_buffer(0, 4, values.transforms_world)
      .bind_buffer(0, 5, values.materials)
      .bind_buffer(0, 6, values.lights)
      .bind_image(0, 7, context.sky_view_lut_attachment)
      .bind_image(0, 8, context.sky_transmittance_lut_attachment)
      .bind_sampler(0, 9, render_context.cached_sampler_handle(LinearSamplerClamped))
      .bind_image(0, 10, context.irradiance_attachment)
      .bind_image(0, 11, context.distance_attachment)
      .bind_buffer(0, 12, context.probe_states_buffer)
      .bind_image(0, 13, context.pointspot_page_table_attachment)
      .bind_image(0, 14, context.vsm_physical_pages_attachment)
      .bind_buffer(0, 15, values.camera)
      .bind_image(0, 16, context.half_res_depth)
      .bind_image(0, 17, context.half_res_face_normal)
      .bind_buffer(0, 18, ray_list)
      .bind_buffer(0, 19, counters)
      .bind_buffer(0, 20, ray_result)
      .bind_image(0, 21, context.half_res_sample_count)
      .bind_image(0, 22, pioneer_guide_hit_y)
      .bind_buffer(0, 23, context.probe_requests_buffer)
      .specialize_constant(0, std::to_underlying(shape.scene_flags))
      .push_constant_address(0, values.atmosphere)
      .push_constant_address(8, context.light_grid_buffer)
      .push_constant_address(16, context.pointspot_views_buffer)
      .push_constants_from_at(24, variables.trace_push)
      .trace_rays(RTGI_TRACE_GROUP_WIDTH, variables.trace_groups, 1)
      .end_compute()
  );

  context.radiance = module.transient_image(
    {.extent = half_extent, .format = vir::Format::RGBA16Sfloat, .name = "rtgi radiance"}
  );
  auto perceptual_rgb_shortness = module.transient_image(
    {.extent = half_extent, .format = vir::Format::RGBA16Sfloat, .name = "rtgi perceptual rgb shortness"}
  );
  std::tie(
    values.camera,
    context.half_res_depth,
    ray_count_image,
    pixel_ray_alloc,
    ray_result,
    values.exposure,
    perceptual_rgb_shortness,
    context.radiance
  ) = std::tuple_cat( //
    module
      .begin_compute(
        {{values.camera, vir::Access::ComputeUniformRead},
         {context.half_res_depth, vir::Access::ComputeSampled},
         {ray_count_image, vir::Access::ComputeSampled},
         {pixel_ray_alloc, vir::Access::ComputeSampled},
         {ray_result, vir::Access::ComputeRead},
         {values.exposure, vir::Access::ComputeUniformRead},
         {perceptual_rgb_shortness, vir::Access::ComputeWrite},
         {context.radiance, vir::Access::ComputeWrite}}
      )
      .with_name("rtgi blend rays")
      .bind_compute_pipeline(render_context.pipeline("rtgi_blend_rays"))
      .bind_buffer(0, 0, values.camera)
      .bind_image(0, 1, context.half_res_depth)
      .bind_image(0, 2, ray_count_image)
      .bind_image(0, 3, pixel_ray_alloc)
      .bind_buffer(0, 4, ray_result)
      .bind_buffer(0, 5, values.exposure)
      .bind_image(0, 6, perceptual_rgb_shortness)
      .bind_image(0, 7, context.radiance)
      .push_constants_from(variables.settings)
      .dispatch_invocations(half_extent.width, half_extent.height, 1)
      .end_compute()
  );

  auto pre_filtered_diffuse = module.transient_image(
    {.extent = half_extent, .format = vir::Format::RGBA16Sfloat, .name = "rtgi pre filtered diffuse"}
  );
  auto pre_filtered_diffuse2 = module.transient_image(
    {.extent = half_extent, .format = vir::Format::RG16Sfloat, .name = "rtgi pre filtered diffuse2"}
  );
  auto ao_guide = module.transient_image(
    {.extent = half_extent, .format = vir::Format::R8Unorm, .name = "rtgi ao guide"}
  );
  auto perceptual_radiance = module.transient_image(
    {.extent = half_extent, .format = vir::Format::R16Sfloat, .name = "rtgi perceptual radiance"}
  );
  auto firefly_factor = module.transient_image(
    {.extent = half_extent, .format = vir::Format::R16Sfloat, .name = "rtgi firefly factor"}
  );
  std::tie(
    values.camera,
    context.half_res_depth,
    context.half_res_face_normal,
    ray_count_image,
    pixel_ray_alloc,
    ray_result,
    perceptual_rgb_shortness,
    values.exposure,
    pre_filtered_diffuse,
    pre_filtered_diffuse2,
    ao_guide,
    perceptual_radiance,
    firefly_factor
  ) = std::tuple_cat( //
    module
      .begin_compute(
        {{values.camera, vir::Access::ComputeUniformRead},
         {context.half_res_depth, vir::Access::ComputeSampled},
         {context.half_res_face_normal, vir::Access::ComputeSampled},
         {ray_count_image, vir::Access::ComputeSampled},
         {pixel_ray_alloc, vir::Access::ComputeSampled},
         {ray_result, vir::Access::ComputeRead},
         {perceptual_rgb_shortness, vir::Access::ComputeSampled},
         {values.exposure, vir::Access::ComputeUniformRead},
         {pre_filtered_diffuse, vir::Access::ComputeWrite},
         {pre_filtered_diffuse2, vir::Access::ComputeWrite},
         {ao_guide, vir::Access::ComputeWrite},
         {perceptual_radiance, vir::Access::ComputeWrite},
         {firefly_factor, vir::Access::ComputeWrite}}
      )
      .with_name("rtgi pre filter")
      .bind_compute_pipeline(render_context.pipeline("rtgi_pre_filter"))
      .bind_buffer(0, 0, values.camera)
      .bind_image(0, 1, context.half_res_depth)
      .bind_image(0, 2, context.half_res_face_normal)
      .bind_image(0, 3, ray_count_image)
      .bind_image(0, 4, pixel_ray_alloc)
      .bind_buffer(0, 5, ray_result)
      .bind_image(0, 6, perceptual_rgb_shortness)
      .bind_buffer(0, 7, values.exposure)
      .bind_image(0, 8, pre_filtered_diffuse)
      .bind_image(0, 9, pre_filtered_diffuse2)
      .bind_image(0, 10, ao_guide)
      .bind_image(0, 11, perceptual_radiance)
      .bind_image(0, 12, firefly_factor)
      .push_constants_from(variables.settings)
      .dispatch_invocations(half_extent.width, half_extent.height, 1)
      .end_compute()
  );

  // the pre blur ping pongs between two sets, the second only exists from two iterations on
  auto accumulate_diffuse = pre_filtered_diffuse;
  auto accumulate_diffuse2 = pre_filtered_diffuse2;
  if (shape.rtgi_pre_blur_iterations > 0) {
    auto ping_diffuse = module.transient_image(
      {.extent = half_extent, .format = vir::Format::RGBA16Sfloat, .name = "rtgi pre blurred diffuse"}
    );
    auto ping_diffuse2 = module.transient_image(
      {.extent = half_extent, .format = vir::Format::RG16Sfloat, .name = "rtgi pre blurred diffuse2"}
    );
    auto pong_diffuse = vir::ValueID::Invalid;
    auto pong_diffuse2 = vir::ValueID::Invalid;
    if (shape.rtgi_pre_blur_iterations >= 2) {
      pong_diffuse = module.transient_image(
        {.extent = half_extent, .format = vir::Format::RGBA16Sfloat, .name = "rtgi pre blurred diffuse pong"}
      );
      pong_diffuse2 = module.transient_image(
        {.extent = half_extent, .format = vir::Format::RG16Sfloat, .name = "rtgi pre blurred diffuse2 pong"}
      );
    }

    for (auto i = 0_u32; i < shape.rtgi_pre_blur_iterations; i++) {
      auto& source_diffuse = i == 0 ? pre_filtered_diffuse : (i % 2 == 1 ? ping_diffuse : pong_diffuse);
      auto& source_diffuse2 = i == 0 ? pre_filtered_diffuse2 : (i % 2 == 1 ? ping_diffuse2 : pong_diffuse2);
      auto& destination_diffuse = i % 2 == 0 ? ping_diffuse : pong_diffuse;
      auto& destination_diffuse2 = i % 2 == 0 ? ping_diffuse2 : pong_diffuse2;
      std::tie(
        values.camera,
        context.half_res_depth,
        context.half_res_face_normal,
        source_diffuse,
        source_diffuse2,
        firefly_factor,
        perceptual_radiance,
        ao_guide,
        ray_count_image,
        destination_diffuse,
        destination_diffuse2
      ) = std::tuple_cat( //
        module
          .begin_compute(
            {{values.camera, vir::Access::ComputeUniformRead},
             {context.half_res_depth, vir::Access::ComputeSampled},
             {context.half_res_face_normal, vir::Access::ComputeSampled},
             {source_diffuse, vir::Access::ComputeSampled},
             {source_diffuse2, vir::Access::ComputeSampled},
             {firefly_factor, vir::Access::ComputeSampled},
             {perceptual_radiance, vir::Access::ComputeSampled},
             {ao_guide, vir::Access::ComputeSampled},
             {ray_count_image, vir::Access::ComputeSampled},
             {destination_diffuse, vir::Access::ComputeWrite},
             {destination_diffuse2, vir::Access::ComputeWrite}}
          )
          .with_name("rtgi pre blur")
          .bind_compute_pipeline(render_context.pipeline("rtgi_pre_blur"))
          .bind_buffer(0, 0, values.camera)
          .bind_image(0, 1, context.half_res_depth)
          .bind_image(0, 2, context.half_res_face_normal)
          .bind_image(0, 3, source_diffuse)
          .bind_image(0, 4, source_diffuse2)
          .bind_image(0, 5, firefly_factor)
          .bind_image(0, 6, perceptual_radiance)
          .bind_image(0, 7, ao_guide)
          .bind_image(0, 8, ray_count_image)
          .bind_image(0, 9, destination_diffuse)
          .bind_image(0, 10, destination_diffuse2)
          .push_constants_from(variables.settings)
          .push_constants_at(static_cast<u32>(sizeof(GPU::RTGISettings)), i)
          .dispatch_invocations(half_extent.width, half_extent.height, 1)
          .end_compute()
      );
    }

    const auto is_last_in_ping = shape.rtgi_pre_blur_iterations % 2 == 1;
    accumulate_diffuse = is_last_in_ping ? ping_diffuse : pong_diffuse;
    accumulate_diffuse2 = is_last_in_ping ? ping_diffuse2 : pong_diffuse2;
  }

  context.firefly_factor = firefly_factor;

  std::tie(
    context.half_res_sample_count,
    context.previous_half_res_sample_count,
    ray_count_image,
    reproject_corner,
    reproject_weights,
    accumulate_diffuse,
    accumulate_diffuse2,
    ao_guide,
    perceptual_radiance,
    context.previous_half_res_diffuse,
    context.previous_half_res_diffuse2,
    context.previous_fast_temporal_history,
    context.previous_half_res_ao_guide,
    context.previous_temporal_perceptual_radiance,
    context.half_res_diffuse,
    context.half_res_diffuse2,
    context.fast_temporal_history,
    context.half_res_ao_guide,
    context.temporal_perceptual_radiance
  ) = std::tuple_cat( //
    module
      .begin_compute(
        {{context.half_res_sample_count, vir::Access::ComputeRW},
         {context.previous_half_res_sample_count, vir::Access::ComputeSampled},
         {ray_count_image, vir::Access::ComputeSampled},
         {reproject_corner, vir::Access::ComputeSampled},
         {reproject_weights, vir::Access::ComputeSampled},
         {accumulate_diffuse, vir::Access::ComputeSampled},
         {accumulate_diffuse2, vir::Access::ComputeSampled},
         {ao_guide, vir::Access::ComputeSampled},
         {perceptual_radiance, vir::Access::ComputeSampled},
         {context.previous_half_res_diffuse, vir::Access::ComputeSampled},
         {context.previous_half_res_diffuse2, vir::Access::ComputeSampled},
         {context.previous_fast_temporal_history, vir::Access::ComputeSampled},
         {context.previous_half_res_ao_guide, vir::Access::ComputeSampled},
         {context.previous_temporal_perceptual_radiance, vir::Access::ComputeSampled},
         {context.half_res_diffuse, vir::Access::ComputeWrite},
         {context.half_res_diffuse2, vir::Access::ComputeWrite},
         {context.fast_temporal_history, vir::Access::ComputeWrite},
         {context.half_res_ao_guide, vir::Access::ComputeWrite},
         {context.temporal_perceptual_radiance, vir::Access::ComputeWrite}}
      )
      .with_name("rtgi temporal accumulate")
      .bind_compute_pipeline(render_context.pipeline("rtgi_temporal_accumulate"))
      .bind_image(0, 0, context.half_res_sample_count)
      .bind_image(0, 1, context.previous_half_res_sample_count)
      .bind_image(0, 2, ray_count_image)
      .bind_image(0, 3, reproject_corner)
      .bind_image(0, 4, reproject_weights)
      .bind_image(0, 5, accumulate_diffuse)
      .bind_image(0, 6, accumulate_diffuse2)
      .bind_image(0, 7, ao_guide)
      .bind_image(0, 8, perceptual_radiance)
      .bind_image(0, 9, context.previous_half_res_diffuse)
      .bind_image(0, 10, context.previous_half_res_diffuse2)
      .bind_image(0, 11, context.previous_fast_temporal_history)
      .bind_image(0, 12, context.previous_half_res_ao_guide)
      .bind_image(0, 13, context.previous_temporal_perceptual_radiance)
      .bind_image(0, 14, context.half_res_diffuse)
      .bind_image(0, 15, context.half_res_diffuse2)
      .bind_image(0, 16, context.fast_temporal_history)
      .bind_image(0, 17, context.half_res_ao_guide)
      .bind_image(0, 18, context.temporal_perceptual_radiance)
      .push_constants_from(variables.settings)
      .dispatch_invocations(half_extent.width, half_extent.height, 1)
      .end_compute()
  );

  context.ray_count = ray_count_image;

  auto horizontal_diffuse = module.transient_image(
    {.extent = half_extent, .format = vir::Format::RGBA16Sfloat, .name = "rtgi post blur horizontal diffuse"}
  );
  auto horizontal_diffuse2 = module.transient_image(
    {.extent = half_extent, .format = vir::Format::RG16Sfloat, .name = "rtgi post blur horizontal diffuse2"}
  );
  std::tie(
    values.camera,
    context.half_res_depth,
    context.half_res_face_normal,
    context.half_res_sample_count,
    context.half_res_ao_guide,
    context.temporal_perceptual_radiance,
    context.half_res_diffuse,
    context.half_res_diffuse2,
    horizontal_diffuse,
    horizontal_diffuse2
  ) = std::tuple_cat( //
    module
      .begin_compute(
        {{values.camera, vir::Access::ComputeUniformRead},
         {context.half_res_depth, vir::Access::ComputeSampled},
         {context.half_res_face_normal, vir::Access::ComputeSampled},
         {context.half_res_sample_count, vir::Access::ComputeSampled},
         {context.half_res_ao_guide, vir::Access::ComputeSampled},
         {context.temporal_perceptual_radiance, vir::Access::ComputeSampled},
         {context.half_res_diffuse, vir::Access::ComputeSampled},
         {context.half_res_diffuse2, vir::Access::ComputeSampled},
         {horizontal_diffuse, vir::Access::ComputeWrite},
         {horizontal_diffuse2, vir::Access::ComputeWrite}}
      )
      .with_name("rtgi post blur horizontal")
      .bind_compute_pipeline(render_context.pipeline("rtgi_post_blur_horizontal"))
      .bind_buffer(0, 0, values.camera)
      .bind_image(0, 1, context.half_res_depth)
      .bind_image(0, 2, context.half_res_face_normal)
      .bind_image(0, 3, context.half_res_sample_count)
      .bind_image(0, 4, context.half_res_ao_guide)
      .bind_image(0, 5, context.temporal_perceptual_radiance)
      .bind_image(0, 6, context.half_res_diffuse)
      .bind_image(0, 7, context.half_res_diffuse2)
      .bind_image(0, 8, horizontal_diffuse)
      .bind_image(0, 9, horizontal_diffuse2)
      .push_constants_from(variables.settings)
      .dispatch(
        (half_extent.width + RTGI_POST_BLUR_ALONG - 1_u32) / RTGI_POST_BLUR_ALONG,
        (half_extent.height + RTGI_POST_BLUR_ACROSS - 1_u32) / RTGI_POST_BLUR_ACROSS,
        1
      )
      .end_compute()
  );

  context.post_blur_diffuse = module.transient_image(
    {.extent = half_extent, .format = vir::Format::RGBA16Sfloat, .name = "rtgi post blur diffuse"}
  );
  context.post_blur_diffuse2 = module.transient_image(
    {.extent = half_extent, .format = vir::Format::RG16Sfloat, .name = "rtgi post blur diffuse2"}
  );
  std::tie(
    values.camera,
    context.half_res_depth,
    context.half_res_face_normal,
    context.half_res_sample_count,
    context.half_res_ao_guide,
    context.temporal_perceptual_radiance,
    horizontal_diffuse,
    horizontal_diffuse2,
    context.post_blur_diffuse,
    context.post_blur_diffuse2
  ) = std::tuple_cat( //
    module
      .begin_compute(
        {{values.camera, vir::Access::ComputeUniformRead},
         {context.half_res_depth, vir::Access::ComputeSampled},
         {context.half_res_face_normal, vir::Access::ComputeSampled},
         {context.half_res_sample_count, vir::Access::ComputeSampled},
         {context.half_res_ao_guide, vir::Access::ComputeSampled},
         {context.temporal_perceptual_radiance, vir::Access::ComputeSampled},
         {horizontal_diffuse, vir::Access::ComputeSampled},
         {horizontal_diffuse2, vir::Access::ComputeSampled},
         {context.post_blur_diffuse, vir::Access::ComputeWrite},
         {context.post_blur_diffuse2, vir::Access::ComputeWrite}}
      )
      .with_name("rtgi post blur vertical")
      .bind_compute_pipeline(render_context.pipeline("rtgi_post_blur_vertical"))
      .bind_buffer(0, 0, values.camera)
      .bind_image(0, 1, context.half_res_depth)
      .bind_image(0, 2, context.half_res_face_normal)
      .bind_image(0, 3, context.half_res_sample_count)
      .bind_image(0, 4, context.half_res_ao_guide)
      .bind_image(0, 5, context.temporal_perceptual_radiance)
      .bind_image(0, 6, horizontal_diffuse)
      .bind_image(0, 7, horizontal_diffuse2)
      .bind_image(0, 8, context.post_blur_diffuse)
      .bind_image(0, 9, context.post_blur_diffuse2)
      .push_constants_from(variables.settings)
      .dispatch(
        (half_extent.width + RTGI_POST_BLUR_ACROSS - 1_u32) / RTGI_POST_BLUR_ACROSS,
        (half_extent.height + RTGI_POST_BLUR_ALONG - 1_u32) / RTGI_POST_BLUR_ALONG,
        1
      )
      .end_compute()
  );

  context.resolved_diffuse = module.transient_image(
    {.extent = {shape.render_size.x, shape.render_size.y, 1},
     .format = vir::Format::RGBA16Sfloat,
     .name = "rtgi resolved diffuse"}
  );
  std::tie(
    values.camera,
    context.post_blur_diffuse,
    context.post_blur_diffuse2,
    context.half_res_depth,
    context.half_res_face_normal,
    context.depth_attachment,
    context.normal_attachment,
    context.resolved_diffuse
  ) = std::tuple_cat( //
    module
      .begin_compute(
        {{values.camera, vir::Access::ComputeUniformRead},
         {context.post_blur_diffuse, vir::Access::ComputeSampled},
         {context.post_blur_diffuse2, vir::Access::ComputeSampled},
         {context.half_res_depth, vir::Access::ComputeSampled},
         {context.half_res_face_normal, vir::Access::ComputeSampled},
         {context.depth_attachment, vir::Access::ComputeSampled},
         {context.normal_attachment, vir::Access::ComputeSampled},
         {context.resolved_diffuse, vir::Access::ComputeWrite}}
      )
      .with_name("rtgi upscale")
      .bind_compute_pipeline(render_context.pipeline("rtgi_upscale"))
      .bind_buffer(0, 0, values.camera)
      .bind_image(0, 1, context.post_blur_diffuse)
      .bind_image(0, 2, context.post_blur_diffuse2)
      .bind_image(0, 3, context.half_res_depth)
      .bind_image(0, 4, context.half_res_face_normal)
      .bind_image(0, 5, context.depth_attachment)
      .bind_image(0, 6, context.normal_attachment)
      .bind_image(0, 7, context.resolved_diffuse)
      .push_constants(RTGIGBufferPush(shape.render_size))
      .dispatch_invocations(shape.render_size.x, shape.render_size.y, 1)
      .end_compute()
  );
}
} // namespace ox
