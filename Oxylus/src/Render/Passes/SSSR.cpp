#include <ankerl/svector.h>
#include <vir/RenderGraph.hpp>
#include <vir/Types.hpp>

#include "Memory/Stack.hpp"
#include "Render/RenderContext.hpp"
#include "Render/Renderer.hpp"
#include "Render/RendererInstance.hpp"
#include "Render/Utils/PushConstants.hpp"
#include "Render/Utils/Samplers.hpp"
#include "Utils/Log.hpp"

namespace ox {
using SSSRBlueNoisePush = PushConstants<u32>;

// the intersect pass pushes the atmosphere address ahead of the settings
static_assert(8 + sizeof(GPU::SSSRSettings) <= 128);

constexpr static auto TRANSIENT_BUFFER_USAGE = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

// rounded up to the 64 pixel block one hiz workgroup covers, so every cell at mip k spans an exact block of render
// texels
static auto sssr_hierarchy_extent(const glm::uvec2 render_size) -> vir::Extent3D {
  return {align_up(render_size.x, 64), align_up(render_size.y, 64), 1};
}

auto RendererInstance::generate_sssr_depth_hierarchy(
  vir::Module& module,
  RenderContext& render_context,
  const glm::uvec2 render_size,
  vir::ValueID depth,
  vir::ValueID hierarchy
) -> std::pair<vir::ValueID, vir::ValueID> {
  ZoneScoped;

  const auto extent = sssr_hierarchy_extent(render_size);
  const auto counter_value = 0_u32;
  auto counter = module.transient_buffer({
    .size = sizeof(counter_value),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "sssr depth hierarchy atomic",
  });
  counter = module.update_buffer(
    counter,
    std::span{reinterpret_cast<const u8*>(&counter_value), sizeof(counter_value)}
  );

  const auto groups_x = extent.width / 64;
  const auto groups_y = extent.height / 64;
  auto pass = //
    module
      .begin_compute({
        {counter, vir::Access::ComputeRW},
        {depth, vir::Access::ComputeSampled},
        {hierarchy, vir::Access::ComputeRW},
      })
      .with_name("sssr depth hierarchy")
      .bind_compute_pipeline(render_context.pipeline("hiz"))
      .specialize_constant(0, 1_u32)
      .specialize_constant(1, render_size.x)
      .specialize_constant(2, render_size.y)
      .specialize_constant(3, 1_u32)
      .specialize_constant(4, 1_u32)
      .bind_buffer(0, 0, counter)
      .bind_sampler(0, 1, render_context.cached_sampler_handle(NearestSamplerClamped))
      .bind_image(0, 2, depth);
  // hiz binds 13 mips, the ones past the hierarchy alias the last and are never written
  for (auto mip = 0_u32; mip < 13; mip++) {
    module.bind_image(0, 3 + mip, module.mip(hierarchy, std::min(mip, SSSRContext::PYRAMID_MIP_COUNT - 1)));
  }

  module.push_constants(PushConstants(SSSRContext::PYRAMID_MIP_COUNT, groups_x * groups_y, glm::mat2(1.0f)))
    .dispatch(groups_x, groups_y, 1);
  const auto result = pass.end_compute();
  return {result[1], result[2]};
}

auto RendererInstance::bind_sssr_variables(
  this const RendererInstance& self, vir::Program& program, const SSSRVariables& variables
) -> void {
  ZoneScoped;

  if (!program.has_variable(variables.is_reset)) {
    return;
  }

  const auto& params = self.frame_params;
  const auto& cvar = *params.cvar;
  program.set(variables.blue_noise_tables, self.renderer.render_context->buffer(self.renderer.blue_noise_buffer));
  program.set_bytes(variables.blue_noise_push, SSSRBlueNoisePush(params.frame_index));
  program.set(variables.is_reset, params.sssr_reset);

  const auto samples_per_quad = cvar.cvar_sssr_samples_per_quad.get();
  program.set_bytes(
    variables.settings,
    GPU::SSSRSettings{
      .render_size = params.shape.render_size,
      .roughness_threshold = cvar.cvar_sssr_roughness_threshold.get(),
      .depth_thickness = cvar.cvar_sssr_depth_thickness.get(),
      .max_traversal_intersections = static_cast<u32>(std::max(cvar.cvar_sssr_max_traversal_intersections.get(), 1)),
      .min_traversal_occupancy = static_cast<u32>(std::max(cvar.cvar_sssr_min_traversal_occupancy.get(), 0)),
      .most_detailed_mip = static_cast<u32>(
        std::clamp(cvar.cvar_sssr_most_detailed_mip.get(), 0, static_cast<i32>(SSSRContext::PYRAMID_MIP_COUNT) - 1)
      ),
      .samples_per_quad = samples_per_quad >= 4 ? 4_u32 : (samples_per_quad >= 2 ? 2_u32 : 1_u32),
      .is_temporal_variance_guided = static_cast<u32>(cvar.cvar_sssr_temporal_variance_guided.as_bool()),
      .variance_threshold = cvar.cvar_sssr_variance_threshold.get(),
      .temporal_stability = cvar.cvar_sssr_temporal_stability.get(),
      .frame_index = params.frame_index,
      .ddgi_volume_count = static_cast<u32>(self.probe_volumes.size()),
      .ddgi_normal_bias = cvar.cvar_ddgi_normal_bias.get(),
      .ddgi_view_bias = cvar.cvar_ddgi_view_bias.get(),
      .ddgi_intensity = cvar.cvar_ddgi_intensity.get(),
      .horizon_darkening = glm::smoothstep(-0.1f, 0.3f, self.directional_light.direction.y),
      .sky_ambient_color = self.sky_data.ambient_color,
      .base_ambient_color = BASE_AMBIENT_COLOR,
      .sun_direction = self.directional_light.direction,
      .is_eye_adapted = static_cast<u32>(params.shape.has_eye_adaptation),
      .fixed_exposure = cvar.cvar_exposure.get(),
    }
  );

  const auto current_index = params.sssr_history_ping ? 1 : 0;
  const auto previous_index = 1 - current_index;
  program.set(variables.depth_hierarchy, self.sssr_depth_hierarchy[current_index].attachment_info());
  program.set(variables.previous_depth_hierarchy, self.sssr_depth_hierarchy[previous_index].attachment_info());
  program.set(variables.radiance, self.sssr_radiance[current_index].attachment_info());
  program.set(variables.previous_radiance, self.sssr_radiance[previous_index].attachment_info());
  program.set(variables.variance, self.sssr_variance[current_index].attachment_info());
  program.set(variables.previous_variance, self.sssr_variance[previous_index].attachment_info());
  program.set(variables.sample_count, self.sssr_sample_count[current_index].attachment_info());
  program.set(variables.previous_sample_count, self.sssr_sample_count[previous_index].attachment_info());
  program.set(variables.roughness, self.sssr_roughness[current_index].attachment_info());
  program.set(variables.previous_roughness, self.sssr_roughness[previous_index].attachment_info());
  program.set(variables.normal, self.sssr_normal[current_index].attachment_info());
  program.set(variables.previous_normal, self.sssr_normal[previous_index].attachment_info());
  program.set(variables.average_radiance, self.sssr_average_radiance.attachment_info());
}

auto RendererInstance::allocate_sssr_resources(this RendererInstance& self, const glm::uvec2 render_size) -> void {
  ZoneScoped;

  if (self.sssr_render_size == render_size) {
    return;
  }

  constexpr auto history_usage = vir::ImageUsageFlags::Storage | vir::ImageUsageFlags::Sampled |
                                 vir::ImageUsageFlags::TransferDst;
  const auto render_extent = vir::Extent3D{render_size.x, render_size.y, 1};
  const auto tile_extent = vir::Extent3D{(render_size.x + 7_u32) / 8_u32, (render_size.y + 7_u32) / 8_u32, 1};
  for (auto i = 0_u32; i < 2; i++) {
    self.sssr_depth_hierarchy[i] = Texture::create({
      .format = vir::Format::R32Sfloat,
      .extent = sssr_hierarchy_extent(render_size),
      .level_count = SSSRContext::PYRAMID_MIP_COUNT,
      .usage = history_usage,
    });
    self.sssr_radiance[i] = Texture::create(
      {.format = vir::Format::RGBA16Sfloat, .extent = render_extent, .usage = history_usage}
    );
    self.sssr_variance[i] = Texture::create(
      {.format = vir::Format::R16Sfloat, .extent = render_extent, .usage = history_usage}
    );
    self.sssr_sample_count[i] = Texture::create(
      {.format = vir::Format::R16Sfloat, .extent = render_extent, .usage = history_usage}
    );
    self.sssr_roughness[i] = Texture::create(
      {.format = vir::Format::R8Unorm, .extent = render_extent, .usage = history_usage}
    );
    self.sssr_normal[i] = Texture::create(
      {.format = vir::Format::RGBA16Sfloat, .extent = render_extent, .usage = history_usage}
    );
  }

  self.sssr_average_radiance = Texture::create(
    {.format = vir::Format::RGBA16Sfloat, .extent = tile_extent, .usage = history_usage}
  );

  auto module = vir::Module{};
  const auto black = vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 0.0f);
  for (auto i = 0_u32; i < 2; i++) {
    for (const auto* texture :
         {&self.sssr_depth_hierarchy[i],
          &self.sssr_radiance[i],
          &self.sssr_variance[i],
          &self.sssr_sample_count[i],
          &self.sssr_roughness[i],
          &self.sssr_normal[i]}) {
      const auto cleared = module.clear(texture->import_discarded(module, vir::Access::None), black);
      module.export_to(cleared, SSSRContext::HISTORY_REST);
    }
  }

  const auto cleared_average = module.clear(
    self.sssr_average_radiance.import_discarded(module, vir::Access::None),
    black
  );
  module.export_to(cleared_average, SSSRContext::HISTORY_REST);

  if (const auto initialized = self.renderer.render_context->execute_blocking(std::move(module)); !initialized) {
    OX_LOG_FATAL("failed to initialize the SSSR history ({})", std::to_underlying(initialized.error()));
  }

  self.sssr_render_size = render_size;
  self.sssr_history_valid = false;
}

auto RendererInstance::record_sssr(
  vir::Module& module, RenderContext& render_context, FrameRecording& recording, SSSRContext& context
) -> void {
  ZoneScoped;
  auto& prepared = recording.prepared;
  auto& values = recording.values;

  auto& variables = prepared.sssr;
  variables.blue_noise_tables = module.declare_buffer_var("sssr blue noise tables", vir::Access::MemoryRead);
  variables.blue_noise_push = module.declare_bytes_var("sssr blue noise push", sizeof(SSSRBlueNoisePush));
  variables.is_reset = module.declare_bool_var("sssr reset", false);
  variables.settings = module.declare_bytes_var("sssr settings", sizeof(GPU::SSSRSettings));
  variables.depth_hierarchy = module.declare_image_var(
    "sssr depth hierarchy",
    vir::Format::R32Sfloat,
    SSSRContext::HISTORY_REST
  );
  variables.previous_depth_hierarchy = module.declare_image_var(
    "sssr previous depth hierarchy",
    vir::Format::R32Sfloat,
    SSSRContext::HISTORY_REST
  );
  variables.radiance = module.declare_image_var("sssr radiance", vir::Format::RGBA16Sfloat, SSSRContext::HISTORY_REST);
  variables.previous_radiance = module.declare_image_var(
    "sssr previous radiance",
    vir::Format::RGBA16Sfloat,
    SSSRContext::HISTORY_REST
  );
  variables.variance = module.declare_image_var("sssr variance", vir::Format::R16Sfloat, SSSRContext::HISTORY_REST);
  variables.previous_variance = module.declare_image_var(
    "sssr previous variance",
    vir::Format::R16Sfloat,
    SSSRContext::HISTORY_REST
  );
  variables.sample_count = module
                             .declare_image_var("sssr sample count", vir::Format::R16Sfloat, SSSRContext::HISTORY_REST);
  variables.previous_sample_count = module.declare_image_var(
    "sssr previous sample count",
    vir::Format::R16Sfloat,
    SSSRContext::HISTORY_REST
  );
  variables.roughness = module.declare_image_var("sssr roughness", vir::Format::R8Unorm, SSSRContext::HISTORY_REST);
  variables.previous_roughness = module.declare_image_var(
    "sssr previous roughness",
    vir::Format::R8Unorm,
    SSSRContext::HISTORY_REST
  );
  variables.normal = module.declare_image_var("sssr normal", vir::Format::RGBA16Sfloat, SSSRContext::HISTORY_REST);
  variables.previous_normal = module.declare_image_var(
    "sssr previous normal",
    vir::Format::RGBA16Sfloat,
    SSSRContext::HISTORY_REST
  );
  variables.average_radiance = module.declare_image_var(
    "sssr average radiance",
    vir::Format::RGBA16Sfloat,
    SSSRContext::HISTORY_REST
  );

  context.depth_hierarchy = variables.depth_hierarchy;
  context.radiance = variables.radiance;
  context.variance = variables.variance;
  context.sample_count = variables.sample_count;
  context.roughness = variables.roughness;
  context.normal = variables.normal;
  context.blue_noise_tables = variables.blue_noise_tables;

  // after a reset a depth of 0 (sky) and a zero sample count make every pixel look disoccluded
  std::tie(
    context.previous_depth_hierarchy,
    context.previous_radiance,
    context.previous_variance,
    context.previous_sample_count,
    context.previous_roughness,
    context.previous_normal,
    context.average_radiance
  ) = std::tuple_cat( //
    module.set_condition(
      variables.is_reset,
      [&](vir::Module& arm) {
        const auto black = vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 0.0f);
        return std::array{
          arm.clear(variables.previous_depth_hierarchy, black),
          arm.clear(variables.previous_radiance, black),
          arm.clear(variables.previous_variance, black),
          arm.clear(variables.previous_sample_count, black),
          arm.clear(variables.previous_roughness, black),
          arm.clear(variables.previous_normal, black),
          arm.clear(variables.average_radiance, black),
        };
      },
      [&](vir::Module&) {
        return std::array{
          variables.previous_depth_hierarchy,
          variables.previous_radiance,
          variables.previous_variance,
          variables.previous_sample_count,
          variables.previous_roughness,
          variables.previous_normal,
          variables.average_radiance,
        };
      }
    )
  );

  std::tie(context.depth_attachment, context.depth_hierarchy) = generate_sssr_depth_hierarchy(
    module,
    render_context,
    recording.shape.render_size,
    context.depth_attachment,
    context.depth_hierarchy
  );

  context.blue_noise = module.transient_image(
    {.extent = {128, 128, 1}, .format = vir::Format::RG8Unorm, .name = "sssr blue noise"}
  );
  std::tie(context.blue_noise_tables, context.blue_noise) = std::tuple_cat( //
    module
      .begin_compute(
        {{context.blue_noise_tables, vir::Access::ComputeRead}, {context.blue_noise, vir::Access::ComputeRW}}
      )
      .with_name("sssr blue noise")
      .bind_compute_pipeline(render_context.pipeline("sssr_blue_noise"))
      .bind_buffer(0, 0, context.blue_noise_tables)
      .bind_image(0, 1, context.blue_noise)
      .push_constants_from(variables.blue_noise_push)
      .dispatch(16, 16, 1)
      .end_compute()
  );

  const auto& shape = recording.shape;
  const auto render_extent = vir::Extent3D{shape.render_size.x, shape.render_size.y, 1};
  const auto tile_extent = (shape.render_size + 7_u32) / 8_u32;
  const auto is_ddgi = shape.has_ddgi;
  const auto is_sky = static_cast<bool>(shape.scene_flags & GPU::SceneFlags::HasAtmosphere);
  const auto is_ambient = !is_ddgi && !is_sky;
  const auto scene_flags = std::to_underlying(shape.scene_flags);
  const auto linear_sampler = render_context.cached_sampler_handle(LinearSamplerClamped);

  memory::ScopedStack stack;

  const auto zero_counters = std::array<u32, 4>{};
  context.ray_counter_buffer = module.transient_buffer({
    .size = sizeof(zero_counters),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "sssr ray counters",
  });
  context.ray_counter_buffer = module.update_buffer(
    context.ray_counter_buffer,
    std::span{reinterpret_cast<const u8*>(zero_counters.data()), sizeof(zero_counters)}
  );
  context.ray_list_buffer = module.transient_buffer({
    .size = shape.render_size.x * shape.render_size.y * sizeof(u32),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "sssr ray list",
  });
  context.tile_list_buffer = module.transient_buffer({
    .size = tile_extent.x * tile_extent.y * sizeof(u32),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "sssr denoiser tile list",
  });
  context.indirect_args_buffer = module.transient_buffer({
    .size = 6 * sizeof(u32),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "sssr indirect args",
  });
  context.intersection_radiance = module.transient_image(
    {.extent = render_extent, .format = vir::Format::RGBA16Sfloat, .name = "sssr intersection radiance"}
  );
  context.confidence = module.transient_image(
    {.extent = render_extent, .format = vir::Format::R8Unorm, .name = "sssr confidence"}
  );
  context.specular_occlusion = module.transient_image(
    {.extent = render_extent, .format = vir::Format::R8Unorm, .name = "sssr specular occlusion"}
  );

  auto probe_volumes_buffer = context.probe_volumes_buffer;
  auto probe_states_buffer = context.probe_states_buffer;
  auto irradiance_attachment = context.irradiance_attachment;
  auto distance_attachment = context.distance_attachment;
  auto atmosphere = values.atmosphere;
  if (!is_ddgi) {
    probe_volumes_buffer = module.transient_buffer({
      .size = sizeof(u32),
      .usage = TRANSIENT_BUFFER_USAGE,
      .location = vir::MemoryLocation::GpuOnly,
      .name = "sssr unused probe volumes",
    });
    probe_states_buffer = module.transient_buffer({
      .size = sizeof(u32),
      .usage = TRANSIENT_BUFFER_USAGE,
      .location = vir::MemoryLocation::GpuOnly,
      .name = "sssr unused probe states",
    });
    irradiance_attachment = module.transient_image(
      {.extent = {1, 1, 1}, .format = vir::Format::RGBA16Sfloat, .name = "sssr unused irradiance"}
    );
    distance_attachment = module.transient_image(
      {.extent = {1, 1, 1}, .format = vir::Format::RGBA16Sfloat, .name = "sssr unused distance"}
    );
  }

  if (!is_sky) {
    atmosphere = module.transient_buffer({
      .size = sizeof(u32),
      .usage = TRANSIENT_BUFFER_USAGE,
      .location = vir::MemoryLocation::GpuOnly,
      .name = "sssr unused atmosphere",
    });
  }

  {
    auto pass = module
                  .begin_compute({
                    {values.camera, vir::Access::ComputeUniformRead},
                    {context.depth_attachment, vir::Access::ComputeSampled},
                    {context.normal_attachment, vir::Access::ComputeSampled},
                    {context.metallic_roughness_occlusion_attachment, vir::Access::ComputeSampled},
                    {context.albedo_attachment, vir::Access::ComputeSampled},
                    {context.ambient_occlusion_attachment, vir::Access::ComputeSampled},
                    {context.ray_counter_buffer, vir::Access::ComputeRW},
                    {context.ray_list_buffer, vir::Access::ComputeRW},
                    {context.tile_list_buffer, vir::Access::ComputeRW},
                    {context.intersection_radiance, vir::Access::ComputeRW},
                    {context.confidence, vir::Access::ComputeRW},
                    {context.roughness, vir::Access::ComputeRW},
                    {context.previous_variance, vir::Access::ComputeSampled},
                    {context.normal, vir::Access::ComputeRW},
                    {context.radiance, vir::Access::ComputeRW},
                    {context.variance, vir::Access::ComputeRW},
                    {context.sample_count, vir::Access::ComputeRW},
                    {context.specular_occlusion, vir::Access::ComputeRW},
                    {values.exposure, vir::Access::ComputeUniformRead},
                    {probe_volumes_buffer, vir::Access::ComputeRead},
                    {probe_states_buffer, vir::Access::ComputeRead},
                    {irradiance_attachment, vir::Access::ComputeSampled},
                    {distance_attachment, vir::Access::ComputeSampled},
                    {context.sky_cubemap_attachment, vir::Access::ComputeSampled},
                  })
                  .with_name("sssr classify");
    const auto* environment = is_ddgi ? "ddgi" : (is_sky ? "sky" : "ambient");
    module.bind_compute_pipeline(render_context.pipeline(stack.format("sssr_classify_{}", environment)));
    if (is_ambient) {
      module.specialize_constant(0, scene_flags);
    }

    module.bind_buffer(0, 0, values.camera)
      .bind_image(0, 1, context.depth_attachment)
      .bind_image(0, 2, context.normal_attachment)
      .bind_image(0, 3, context.metallic_roughness_occlusion_attachment)
      .bind_image(0, 4, context.albedo_attachment)
      .bind_image(0, 5, context.ambient_occlusion_attachment)
      .bind_buffer(0, 6, context.ray_counter_buffer)
      .bind_buffer(0, 7, context.ray_list_buffer)
      .bind_buffer(0, 8, context.tile_list_buffer)
      .bind_image(0, 9, context.intersection_radiance)
      .bind_image(0, 10, context.confidence)
      .bind_image(0, 11, context.roughness)
      .bind_image(0, 12, context.previous_variance)
      .bind_image(0, 13, context.normal)
      .bind_image(0, 14, context.radiance)
      .bind_image(0, 15, context.variance)
      .bind_image(0, 16, context.sample_count)
      .bind_image(0, 17, context.specular_occlusion)
      .bind_buffer(0, 18, values.exposure);
    if (is_ddgi) {
      module.bind_sampler(0, 20, linear_sampler)
        .bind_buffer(0, 21, probe_volumes_buffer)
        .bind_buffer(0, 22, probe_states_buffer)
        .bind_image(0, 23, irradiance_attachment)
        .bind_image(0, 24, distance_attachment);
    } else if (is_sky) {
      module.bind_sampler(0, 20, linear_sampler).bind_image(0, 21, context.sky_cubemap_attachment);
    }

    module.push_constants_from(variables.settings).dispatch(tile_extent.x, tile_extent.y, 1);
    std::tie(
      values.camera,
      context.depth_attachment,
      context.normal_attachment,
      context.metallic_roughness_occlusion_attachment,
      context.albedo_attachment,
      context.ambient_occlusion_attachment,
      context.ray_counter_buffer,
      context.ray_list_buffer,
      context.tile_list_buffer,
      context.intersection_radiance,
      context.confidence,
      context.roughness,
      context.previous_variance,
      context.normal,
      context.radiance,
      context.variance,
      context.sample_count,
      context.specular_occlusion,
      values.exposure,
      probe_volumes_buffer,
      probe_states_buffer,
      irradiance_attachment,
      distance_attachment,
      context.sky_cubemap_attachment
    ) = std::tuple_cat(pass.end_compute());
  }

  std::tie(context.ray_counter_buffer, context.indirect_args_buffer) = std::tuple_cat( //
    module
      .begin_compute(
        {{context.ray_counter_buffer, vir::Access::ComputeRW}, {context.indirect_args_buffer, vir::Access::ComputeRW}}
      )
      .with_name("sssr indirect args")
      .bind_compute_pipeline(render_context.pipeline("sssr_indirect_args"))
      .bind_buffer(0, 0, context.ray_counter_buffer)
      .bind_buffer(0, 1, context.indirect_args_buffer)
      .dispatch(1, 1, 1)
      .end_compute()
  );

  {
    auto pass = //
      module
        .begin_compute({
          {values.camera, vir::Access::ComputeUniformRead},
          {context.final_attachment, vir::Access::ComputeSampled},
          {context.depth_hierarchy, vir::Access::ComputeSampled},
          {context.normal_attachment, vir::Access::ComputeSampled},
          {context.roughness, vir::Access::ComputeSampled},
          {context.blue_noise, vir::Access::ComputeSampled},
          {context.ray_list_buffer, vir::Access::ComputeRead},
          {context.ray_counter_buffer, vir::Access::ComputeRead},
          {context.intersection_radiance, vir::Access::ComputeRW},
          {context.confidence, vir::Access::ComputeRW},
          {context.specular_occlusion, vir::Access::ComputeSampled},
          {context.indirect_args_buffer, vir::Access::IndirectRead},
          {values.exposure, vir::Access::ComputeUniformRead},
          {probe_volumes_buffer, vir::Access::ComputeRead},
          {probe_states_buffer, vir::Access::ComputeRead},
          {irradiance_attachment, vir::Access::ComputeSampled},
          {distance_attachment, vir::Access::ComputeSampled},
          {atmosphere, vir::Access::ComputeRead},
          {context.sky_view_lut_attachment, vir::Access::ComputeSampled},
        })
        .with_name("sssr intersect");
    const auto* environment = is_ddgi ? (is_sky ? "ddgi_sky" : "ddgi") : (is_sky ? "sky" : "ambient");
    module.bind_compute_pipeline(render_context.pipeline(stack.format("sssr_intersect_{}", environment)));
    if (is_ambient) {
      module.specialize_constant(0, scene_flags);
    }

    module //
      .bind_buffer(0, 0, values.camera)
      .bind_image(0, 1, context.final_attachment)
      .bind_image(0, 2, context.depth_hierarchy)
      .bind_image(0, 3, context.normal_attachment)
      .bind_image(0, 4, context.roughness)
      .bind_image(0, 5, context.blue_noise)
      .bind_buffer(0, 6, context.ray_list_buffer)
      .bind_buffer(0, 7, context.ray_counter_buffer)
      .bind_image(0, 8, context.intersection_radiance)
      .bind_image(0, 9, context.confidence)
      .bind_image(0, 10, context.specular_occlusion)
      .bind_buffer(0, 11, values.exposure);
    if (is_ddgi || is_sky) {
      module.bind_sampler(0, 20, linear_sampler);
    }

    if (is_ddgi) {
      module.bind_buffer(0, 21, probe_volumes_buffer)
        .bind_buffer(0, 22, probe_states_buffer)
        .bind_image(0, 23, irradiance_attachment)
        .bind_image(0, 24, distance_attachment);
    }

    if (is_sky) {
      module.bind_image(0, 25, context.sky_view_lut_attachment)
        .push_constant_address(0, atmosphere)
        .push_constants_from_at(8, variables.settings);
    } else {
      module.push_constants_from(variables.settings);
    }

    module.dispatch_indirect(context.indirect_args_buffer);
    std::tie(
      values.camera,
      context.final_attachment,
      context.depth_hierarchy,
      context.normal_attachment,
      context.roughness,
      context.blue_noise,
      context.ray_list_buffer,
      context.ray_counter_buffer,
      context.intersection_radiance,
      context.confidence,
      context.specular_occlusion,
      context.indirect_args_buffer,
      values.exposure,
      probe_volumes_buffer,
      probe_states_buffer,
      irradiance_attachment,
      distance_attachment,
      atmosphere,
      context.sky_view_lut_attachment
    ) = std::tuple_cat(pass.end_compute());
  }

  if (is_ddgi) {
    context.probe_volumes_buffer = probe_volumes_buffer;
    context.probe_states_buffer = probe_states_buffer;
    context.irradiance_attachment = irradiance_attachment;
    context.distance_attachment = distance_attachment;
  }

  if (is_sky) {
    values.atmosphere = atmosphere;
  }

  const auto tile_dispatch_offset = 12_u64;

  auto reprojected_radiance = module.transient_image(
    {.extent = render_extent, .format = vir::Format::RGBA16Sfloat, .name = "sssr reprojected radiance"}
  );
  {
    std::tie(
      values.camera,
      values.exposure,
      context.depth_attachment,
      context.previous_depth_hierarchy,
      context.roughness,
      context.previous_roughness,
      context.normal,
      context.previous_normal,
      context.intersection_radiance,
      context.previous_radiance,
      context.velocity_attachment,
      context.previous_variance,
      context.previous_sample_count,
      context.tile_list_buffer,
      context.indirect_args_buffer,
      reprojected_radiance,
      context.average_radiance,
      context.variance,
      context.sample_count
    ) = std::tuple_cat( //
      module
        .begin_compute(
          {{values.camera, vir::Access::ComputeUniformRead},
           {values.exposure, vir::Access::ComputeUniformRead},
           {context.depth_attachment, vir::Access::ComputeSampled},
           {context.previous_depth_hierarchy, vir::Access::ComputeSampled},
           {context.roughness, vir::Access::ComputeSampled},
           {context.previous_roughness, vir::Access::ComputeSampled},
           {context.normal, vir::Access::ComputeSampled},
           {context.previous_normal, vir::Access::ComputeSampled},
           {context.intersection_radiance, vir::Access::ComputeSampled},
           {context.previous_radiance, vir::Access::ComputeSampled},
           {context.velocity_attachment, vir::Access::ComputeSampled},
           {context.previous_variance, vir::Access::ComputeSampled},
           {context.previous_sample_count, vir::Access::ComputeSampled},
           {context.tile_list_buffer, vir::Access::ComputeRead},
           {context.indirect_args_buffer, vir::Access::IndirectRead},
           {reprojected_radiance, vir::Access::ComputeRW},
           {context.average_radiance, vir::Access::ComputeRW},
           {context.variance, vir::Access::ComputeRW},
           {context.sample_count, vir::Access::ComputeRW}}
        )
        .with_name("sssr reproject")
        .bind_compute_pipeline(render_context.pipeline("sssr_reproject"))
        .bind_buffer(0, 0, values.camera)
        .bind_buffer(0, 1, values.exposure)
        .bind_image(0, 2, context.depth_attachment)
        .bind_image(0, 3, context.previous_depth_hierarchy)
        .bind_image(0, 4, context.roughness)
        .bind_image(0, 5, context.previous_roughness)
        .bind_image(0, 6, context.normal)
        .bind_image(0, 7, context.previous_normal)
        .bind_image(0, 8, context.intersection_radiance)
        .bind_image(0, 9, context.previous_radiance)
        .bind_image(0, 10, context.velocity_attachment)
        .bind_image(0, 11, context.previous_variance)
        .bind_image(0, 12, context.previous_sample_count)
        .bind_sampler(0, 13, linear_sampler)
        .bind_buffer(0, 14, context.tile_list_buffer)
        .bind_image(0, 15, reprojected_radiance)
        .bind_image(0, 16, context.average_radiance)
        .bind_image(0, 17, context.variance)
        .bind_image(0, 18, context.sample_count)
        .push_constants_from(variables.settings)
        .dispatch_indirect(context.indirect_args_buffer, tile_dispatch_offset)
        .end_compute()
    );
  }

  auto prefiltered_radiance = module.transient_image(
    {.extent = render_extent, .format = vir::Format::RGBA16Sfloat, .name = "sssr prefiltered radiance"}
  );
  auto prefiltered_variance = module.transient_image(
    {.extent = render_extent, .format = vir::Format::R16Sfloat, .name = "sssr prefiltered variance"}
  );
  {
    std::tie(
      values.camera,
      context.depth_attachment,
      context.roughness,
      context.normal,
      context.intersection_radiance,
      context.variance,
      context.average_radiance,
      context.tile_list_buffer,
      context.indirect_args_buffer,
      prefiltered_radiance,
      prefiltered_variance
    ) = std::tuple_cat( //
      module
        .begin_compute(
          {{values.camera, vir::Access::ComputeUniformRead},
           {context.depth_attachment, vir::Access::ComputeSampled},
           {context.roughness, vir::Access::ComputeSampled},
           {context.normal, vir::Access::ComputeSampled},
           {context.intersection_radiance, vir::Access::ComputeSampled},
           {context.variance, vir::Access::ComputeSampled},
           {context.average_radiance, vir::Access::ComputeSampled},
           {context.tile_list_buffer, vir::Access::ComputeRead},
           {context.indirect_args_buffer, vir::Access::IndirectRead},
           {prefiltered_radiance, vir::Access::ComputeRW},
           {prefiltered_variance, vir::Access::ComputeRW}}
        )
        .with_name("sssr prefilter")
        .bind_compute_pipeline(render_context.pipeline("sssr_prefilter"))
        .bind_buffer(0, 0, values.camera)
        .bind_image(0, 1, context.depth_attachment)
        .bind_image(0, 2, context.roughness)
        .bind_image(0, 3, context.normal)
        .bind_image(0, 4, context.intersection_radiance)
        .bind_image(0, 5, context.variance)
        .bind_image(0, 6, context.average_radiance)
        .bind_sampler(0, 7, linear_sampler)
        .bind_buffer(0, 8, context.tile_list_buffer)
        .bind_image(0, 9, prefiltered_radiance)
        .bind_image(0, 10, prefiltered_variance)
        .push_constants_from(variables.settings)
        .dispatch_indirect(context.indirect_args_buffer, tile_dispatch_offset)
        .end_compute()
    );
  }

  {
    std::tie(
      values.exposure,
      context.roughness,
      prefiltered_radiance,
      prefiltered_variance,
      context.sample_count,
      reprojected_radiance,
      context.average_radiance,
      context.tile_list_buffer,
      context.indirect_args_buffer,
      context.radiance,
      context.variance
    ) = std::tuple_cat( //
      module
        .begin_compute(
          {{values.exposure, vir::Access::ComputeUniformRead},
           {context.roughness, vir::Access::ComputeSampled},
           {prefiltered_radiance, vir::Access::ComputeSampled},
           {prefiltered_variance, vir::Access::ComputeSampled},
           {context.sample_count, vir::Access::ComputeSampled},
           {reprojected_radiance, vir::Access::ComputeSampled},
           {context.average_radiance, vir::Access::ComputeSampled},
           {context.tile_list_buffer, vir::Access::ComputeRead},
           {context.indirect_args_buffer, vir::Access::IndirectRead},
           {context.radiance, vir::Access::ComputeRW},
           {context.variance, vir::Access::ComputeRW}}
        )
        .with_name("sssr resolve temporal")
        .bind_compute_pipeline(render_context.pipeline("sssr_resolve_temporal"))
        .bind_buffer(0, 0, values.exposure)
        .bind_image(0, 1, context.roughness)
        .bind_image(0, 2, prefiltered_radiance)
        .bind_image(0, 3, prefiltered_variance)
        .bind_image(0, 4, context.sample_count)
        .bind_image(0, 5, reprojected_radiance)
        .bind_image(0, 6, context.average_radiance)
        .bind_sampler(0, 7, linear_sampler)
        .bind_buffer(0, 8, context.tile_list_buffer)
        .bind_image(0, 9, context.radiance)
        .bind_image(0, 10, context.variance)
        .push_constants_from(variables.settings)
        .dispatch_indirect(context.indirect_args_buffer, tile_dispatch_offset)
        .end_compute()
    );
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
  std::tie(
    context.final_attachment,
    values.camera,
    context.depth_attachment,
    context.albedo_attachment,
    context.normal_attachment,
    context.metallic_roughness_occlusion_attachment,
    context.radiance
  ) = std::tuple_cat( //
    module
      .begin_rendering(
        {{context.final_attachment, vir::Access::ColorRW},
         {values.camera, vir::Access::FragmentUniformRead},
         {context.depth_attachment, vir::Access::FragmentSampled},
         {context.albedo_attachment, vir::Access::FragmentSampled},
         {context.normal_attachment, vir::Access::FragmentSampled},
         {context.metallic_roughness_occlusion_attachment, vir::Access::FragmentSampled},
         {context.radiance, vir::Access::FragmentSampled}}
      )
      .with_name("sssr composite")
      .bind_graphics_pipeline(render_context.pipeline("sssr_composite"))
      .set_color_blend(0, additive_blend)
      .set_viewport(0, vir::Rect2D::framebuffer())
      .set_scissor(0, vir::Rect2D::framebuffer())
      .bind_buffer(0, 0, values.camera)
      .bind_image(0, 1, context.depth_attachment)
      .bind_image(0, 2, context.albedo_attachment)
      .bind_image(0, 3, context.normal_attachment)
      .bind_image(0, 4, context.metallic_roughness_occlusion_attachment)
      .bind_image(0, 5, context.radiance)
      .draw(3, 1)
      .end_rendering()
  );
}
} // namespace ox
