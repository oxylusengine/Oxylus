#include <cstring>
#include <vir/RenderGraph.hpp>
#include <vir/Types.hpp>

#include "Memory/Stack.hpp"
#include "Render/RenderContext.hpp"
#include "Render/RendererInstance.hpp"
#include "Render/Utils/PushConstants.hpp"
#include "Render/Utils/Samplers.hpp"
#include "Scene/Scene.hpp"
#include "Utils/Log.hpp"

namespace ox {
using DDGISelectPush = PushConstants<u32, u32, u32, f32, u32, u32, u32, glm::ivec3>;
using DDGITracePush = PushConstants<u32, u32, u32, u32, u32, u32, f32, f32, f32, f32, glm::vec3, f32, glm::vec3, f32>;
using DDGITraceTailPush = PushConstants<u32, u32, glm::vec3>;
using DDGIRelocatePush = PushConstants<u32, u32, f32, u32, u32>;
using DDGIIrradiancePush = PushConstants<u32, u32, u32, f32, f32, f32, f32>;
using DDGIDistancePush = PushConstants<u32, u32, f32>;
using DDGIApplyPush = PushConstants<u32, f32, f32, f32, glm::vec3>;
using DDGIDebugPush = PushConstants<u32, u32, f32>;

constexpr static auto TRANSIENT_BUFFER_USAGE = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

static auto ddgi_layout_key(std::span<const GPU::ProbeVolume> volumes) -> u64 {
  auto key = 0xcbf29ce484222325_u64;
  const auto mix = [&key](const auto& value) {
    auto bytes = std::array<u8, sizeof(value)>{};
    std::memcpy(bytes.data(), &value, sizeof(value));
    for (const auto byte : bytes) {
      key = (key ^ byte) * 0x100000001b3_u64;
    }
  };

  for (const auto& volume : volumes) {
    mix(volume.counts);
    mix(volume.spacing);
    mix(volume.probe_offset);
    mix(volume.probe_count);
    mix(volume.cascade_index);
    mix(volume.cascade_count);
  }

  return key;
}

auto RendererInstance::allocate_ddgi_atlases(this RendererInstance& self, u32 probe_count) -> void {
  ZoneScoped;

  const auto layout_key = ddgi_layout_key(self.probe_volumes);
  if (self.ddgi_atlas_probe_count == probe_count) {
    if (self.ddgi_atlas_layout_key != layout_key) {
      self.ddgi_atlas_layout_key = layout_key;
      self.ddgi_history_valid = false;
    }

    return;
  }

  static constexpr auto atlas_usage = vir::ImageUsageFlags::Storage | vir::ImageUsageFlags::Sampled |
                                      vir::ImageUsageFlags::TransferDst;
  self.ddgi_irradiance = Texture::create({
    .format = vir::Format::RGBA16Sfloat,
    .extent = GPU::ddgi_irradiance_atlas_extent(probe_count),
    .usage = atlas_usage,
  });
  self.ddgi_irradiance.set_name("ddgi irradiance");
  self.ddgi_distance = Texture::create({
    .format = vir::Format::RG16Sfloat,
    .extent = GPU::ddgi_atlas_extent(probe_count, GPU::DDGI_DISTANCE_TEXELS),
    .usage = atlas_usage,
  });
  self.ddgi_distance.set_name("ddgi distance");

  auto& render_context = *self.renderer.render_context;
  auto module = vir::Module{};
  for (const auto* texture : {&self.ddgi_irradiance, &self.ddgi_distance}) {
    const auto cleared = module.clear(
      texture->import_discarded(module, vir::Access::None),
      vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 0.0f)
    );
    module.export_to(cleared, vir::Access::FragmentSampled);
  }
  if (const auto initialized = render_context.execute_blocking(std::move(module)); !initialized) {
    OX_LOG_FATAL("failed to initialize DDGI atlases ({})", std::to_underlying(initialized.error()));
  }
  const auto reallocate = [&](BufferID& id, u64 size, std::string_view name) {
    if (id != BufferID::Invalid) {
      render_context.destroy_buffer(id);
      id = BufferID::Invalid;
    }

    if (const auto resized = render_context.resize_buffer(id, size, name); !resized) {
      OX_LOG_ERROR("failed to allocate {}", name);
    }
  };
  reallocate(self.ddgi_probe_states, sizeof(GPU::ProbeState) * probe_count, "ddgi probe states");
  reallocate(self.ddgi_probe_update_list, sizeof(u32) * probe_count, "ddgi probe update list");

  self.ddgi_atlas_probe_count = probe_count;
  self.ddgi_atlas_layout_key = layout_key;
  self.ddgi_history_valid = false;
}

auto RendererInstance::prepare_ddgi(this RendererInstance& self, const RendererCVar& cvar) -> void {
  ZoneScoped;

  auto& params = self.frame_params;
  const auto& shape = params.shape;
  params.ddgi_scroll_deltas.clear();
  if (self.probe_volumes.empty()) {
    return;
  }

  auto total_probe_count = 0_u32;
  for (const auto& volume : self.probe_volumes) {
    total_probe_count += volume.probe_count;
  }

  self.allocate_ddgi_atlases(total_probe_count);

  const auto is_distance_culling = cvar.cvar_ddgi_distance_culling.as_bool();
  params.ddgi_total_probe_count = total_probe_count;
  params.ddgi_rays_per_probe = static_cast<u32>(std::clamp(cvar.cvar_ddgi_rays_per_probe.get(), 8, 512));
  params.ddgi_history_valid = self.ddgi_history_valid;
  params.ddgi_distance_culling_enabled = is_distance_culling;
  params.ddgi_distance_culling_changed = is_distance_culling != self.ddgi_distance_culling_enabled;

  if (!shape.has_ddgi) {
    // nothing traced, so whatever the atlas held is stale by the time tracing resumes
    self.ddgi_history_valid = false;
    return;
  }

  for (u32 index = 0; index < static_cast<u32>(self.probe_volumes.size()); index++) {
    const auto& volume = self.probe_volumes[index];
    const auto& key = self.probe_volume_keys[index];
    const auto previous = std::ranges::find_if(self.probe_volume_scrolls, [&key](const ProbeVolumeScroll& entry) {
      return entry.key == key;
    });
    params.ddgi_scroll_deltas.emplace_back(
      previous != self.probe_volume_scrolls.end() ? volume.scroll - previous->scroll : glm::ivec3(volume.counts)
    );
  }

  self.probe_volume_scrolls.clear();
  for (u32 index = 0; index < static_cast<u32>(self.probe_volumes.size()); index++) {
    self.probe_volume_scrolls.emplace_back(
      ProbeVolumeScroll{.key = self.probe_volume_keys[index], .scroll = self.probe_volumes[index].scroll}
    );
  }

  self.ddgi_distance_culling_enabled = is_distance_culling;
}

auto RendererInstance::bind_ddgi_variables(
  this const RendererInstance& self, vir::Program& program, const DDGIVariables& variables
) -> void {
  ZoneScoped;

  const auto& params = self.frame_params;
  const auto& cvar = *params.cvar;
  const auto& shape = params.shape;
  const auto num_frames = static_cast<u32>(self.renderer.render_context->num_frames);
  auto& render_context = *self.renderer.render_context;

  if (shape.probe_volume_count > 0) {
    program.set(variables.probe_states, render_context.buffer(self.ddgi_probe_states));
    program.set(variables.irradiance, self.ddgi_irradiance.attachment_info());
    program.set(variables.distance, self.ddgi_distance.attachment_info());
  }

  if (shape.has_ddgi) {
    program.set(variables.probe_update_list, render_context.buffer(self.ddgi_probe_update_list));
  }

  program.set(variables.is_history_invalid, !params.ddgi_history_valid);
  if (program.has_variable(variables.rays)) {
    program.set(variables.rays, params.ddgi_rays_per_probe);
    program.set_bytes(
      variables.relocate_push,
      DDGIRelocatePush(
        params.ddgi_rays_per_probe,
        num_frames,
        cvar.cvar_ddgi_min_frontface_distance.get(),
        static_cast<u32>(cvar.cvar_ddgi_probe_relocation.as_bool()),
        static_cast<u32>(params.ddgi_distance_culling_enabled)
      )
    );
    program.set_bytes(
      variables.irradiance_push,
      DDGIIrradiancePush(
        params.ddgi_rays_per_probe,
        num_frames,
        GPU::ddgi_radiance_atlas_y_offset(params.ddgi_total_probe_count),
        params.ddgi_history_valid ? cvar.cvar_ddgi_hysteresis.get() : 0.0f,
        cvar.cvar_ddgi_max_brightness_step.get(),
        cvar.cvar_ddgi_firefly_ratio.get(),
        cvar.cvar_ddgi_hysteresis_dark_bias.get()
      )
    );
    program.set_bytes(
      variables.distance_push,
      DDGIDistancePush(
        params.ddgi_rays_per_probe,
        num_frames,
        params.ddgi_history_valid ? cvar.cvar_ddgi_hysteresis.get() : 0.0f
      )
    );
  }

  if (program.has_variable(variables.apply_push)) {
    program.set_bytes(
      variables.apply_push,
      DDGIApplyPush(
        static_cast<u32>(self.probe_volumes.size()),
        cvar.cvar_ddgi_normal_bias.get(),
        cvar.cvar_ddgi_view_bias.get(),
        cvar.cvar_ddgi_intensity.get(),
        self.sky_data.ambient_color
      )
    );
  }

  const auto is_atlas_valid = static_cast<u32>(shape.has_ddgi);
  for (auto volume_index = 0_u32; volume_index < variables.volumes.size(); volume_index++) {
    const auto& volume = variables.volumes[volume_index];
    const auto probe_count = volume_index < self.probe_volumes.size() ? self.probe_volumes[volume_index].probe_count
                                                                      : 0_u32;
    if (program.has_variable(volume.select_push)) {
      program.set_bytes(
        volume.select_push,
        DDGISelectPush(
          volume_index,
          num_frames,
          static_cast<u32>(std::clamp(cvar.cvar_ddgi_update_max_interval.get(), 1, 255)),
          cvar.cvar_ddgi_update_full_rate_distance.get(),
          static_cast<u32>(!params.ddgi_history_valid),
          static_cast<u32>(params.ddgi_distance_culling_changed),
          static_cast<u32>(params.ddgi_distance_culling_enabled),
          volume_index < params.ddgi_scroll_deltas.size() ? params.ddgi_scroll_deltas[volume_index] : glm::ivec3(0)
        )
      );
      program.set(volume.select_probe_count, probe_count);
    }

    if (program.has_variable(volume.trace_push)) {
      program.set_bytes(
        volume.trace_push,
        DDGITracePush(
          volume_index,
          static_cast<u32>(self.probe_volumes.size()),
          params.ddgi_rays_per_probe,
          num_frames,
          static_cast<u32>(self.scene.lights.size()),
          static_cast<u32>(params.ddgi_history_valid),
          cvar.cvar_ddgi_max_ray_distance.get(),
          cvar.cvar_ddgi_shadow_ray_offset.get(),
          cvar.cvar_ddgi_normal_bias.get(),
          cvar.cvar_ddgi_view_bias.get(),
          self.directional_light.direction,
          self.directional_light.intensity,
          self.sky_data.ambient_color,
          cvar.cvar_ddgi_max_ray_radiance.get()
        )
      );
      program.set_bytes(
        volume.trace_tail_push,
        DDGITraceTailPush(
          GPU::ddgi_radiance_atlas_y_offset(params.ddgi_total_probe_count),
          static_cast<u32>(params.ddgi_distance_culling_enabled),
          self.light_grid_origin
        )
      );
      program.set(volume.trace_probe_count, probe_count);
    }

    if (program.has_variable(volume.debug_push)) {
      program.set_bytes(
        volume.debug_push,
        DDGIDebugPush(volume_index, is_atlas_valid, cvar.cvar_ddgi_probe_debug_radius.get())
      );
      program.set(volume.debug_probe_count, probe_count);
    }
  }
}

auto RendererInstance::record_ddgi(
  this RendererInstance& self, vir::Module& module, FrameRecording& recording, DDGIContext& context
) -> void {
  ZoneScoped;
  auto& prepared = recording.prepared;
  auto& values = recording.values;

  memory::ScopedStack stack;
  prepared.ddgi.volumes.resize(recording.shape.probe_volume_count);

  auto& render_context = *self.renderer.render_context;
  auto& variables = prepared.ddgi;
  if (variables.is_history_invalid == vir::ValueID::Invalid) {
    variables.is_history_invalid = module.declare_bool_var("ddgi history invalid", false);
  }
  variables.rays = module.declare_u32_var("ddgi rays per probe", 0);
  variables.relocate_push = module.declare_bytes_var("ddgi relocate push", sizeof(DDGIRelocatePush));
  variables.irradiance_push = module.declare_bytes_var("ddgi update irradiance push", sizeof(DDGIIrradiancePush));
  variables.distance_push = module.declare_bytes_var("ddgi update distance push", sizeof(DDGIDistancePush));

  const auto& shape = recording.shape;
  const auto volume_count = shape.probe_volume_count;
  const auto linear_clamped = render_context.cached_sampler_handle(LinearSamplerClamped);

  // a history that cannot be trusted is cleared, and the hysteresis below drops to 0 so the first
  // update writes the traced values straight through
  std::tie(context.irradiance_attachment, context.distance_attachment) = std::tuple_cat(module.set_condition(
    variables.is_history_invalid,
    [&](vir::Module& arm) {
      const auto black = vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 1.0f);
      return std::array{arm.clear(context.irradiance_attachment, black), arm.clear(context.distance_attachment, black)};
    },
    [&](vir::Module&) { return std::array{context.irradiance_attachment, context.distance_attachment}; }
  ));

  context.ray_data_attachment = module.transient_image({
    .extent = GPU::ddgi_ray_data_extent(shape.ddgi_probe_count, shape.ddgi_rays_per_probe),
    .format = vir::Format::RGBA16Sfloat,
    .name = "ddgi ray data",
  });
  // ddgi_select_probes counts into this atomically, so it starts zeroed
  const auto probe_update_args = GPU::ProbeUpdateArgs{};
  context.probe_update_args_buffer = module.transient_buffer({
    .size = sizeof(probe_update_args),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "ddgi probe update args",
  });
  context.probe_update_args_buffer = module.update_buffer(
    context.probe_update_args_buffer,
    std::span{reinterpret_cast<const u8*>(&probe_update_args), sizeof(probe_update_args)}
  );

  // --- select ---
  for (u32 volume_index = 0; volume_index < volume_count; volume_index++) {
    auto& volume = variables.volumes[volume_index];
    volume.select_push = module.declare_bytes_var(
      stack.format("ddgi select push {}", volume_index),
      sizeof(DDGISelectPush)
    );
    volume.select_probe_count = module.declare_u32_var(stack.format("ddgi volume {} probes", volume_index), 0);

    std::tie(
      context.probe_states_buffer,
      context.probe_volumes_buffer,
      values.camera,
      context.probe_update_list_buffer,
      context.probe_update_args_buffer
    ) = std::tuple_cat( //
      module
        .begin_compute(
          {{context.probe_states_buffer, vir::Access::ComputeRW},
           {context.probe_volumes_buffer, vir::Access::ComputeRead},
           {values.camera, vir::Access::ComputeUniformRead},
           {context.probe_update_list_buffer, vir::Access::ComputeRW},
           {context.probe_update_args_buffer, vir::Access::ComputeRW}}
        )
        .with_name("ddgi select probes")
        .bind_compute_pipeline(render_context.pipeline("ddgi_select_probes"))
        .bind_buffer(0, 0, values.camera)
        .bind_buffer(0, 1, context.probe_volumes_buffer)
        .bind_buffer(0, 2, context.probe_states_buffer)
        .bind_buffer(0, 3, context.probe_update_list_buffer)
        .bind_buffer(0, 4, context.probe_update_args_buffer)
        .push_constants_from(volume.select_push)
        .dispatch_invocations(volume.select_probe_count, 1, 1)
        .end_compute()
    );
  }

  context.probe_update_args_buffer = //
    module.begin_compute({{context.probe_update_args_buffer, vir::Access::ComputeRW}})
      .with_name("ddgi build dispatch args")
      .bind_compute_pipeline(render_context.pipeline("ddgi_build_dispatch_args"))
      .bind_buffer(0, 0, context.probe_update_args_buffer)
      .dispatch(1, 1, 1)
      .end_compute()[0];

  // --- trace ---
  for (u32 volume_index = 0; volume_index < volume_count; volume_index++) {
    auto& volume = variables.volumes[volume_index];
    volume.trace_push = module.declare_bytes_var(
      stack.format("ddgi trace push {}", volume_index),
      sizeof(DDGITracePush)
    );
    volume.trace_tail_push = module.declare_bytes_var(
      stack.format("ddgi trace tail push {}", volume_index),
      sizeof(DDGITraceTailPush)
    );
    volume.trace_probe_count = module.declare_u32_var(stack.format("ddgi trace volume {} probes", volume_index), 0);

    static_assert(DDGITracePush::TOTAL_SIZE == 72);

    auto trace_pass = //
      module
        .begin_compute(
          {{context.ray_data_attachment, vir::Access::RayTracingWrite},
           {context.irradiance_attachment, vir::Access::RayTracingSampled},
           {context.distance_attachment, vir::Access::RayTracingSampled},
           {context.tlas_buffer, vir::Access::RayTracingRead},
           {context.probe_volumes_buffer, vir::Access::RayTracingRead},
           {values.mesh_instances, vir::Access::RayTracingRead},
           {values.meshes, vir::Access::RayTracingRead},
           {values.transforms_world, vir::Access::RayTracingRead},
           {values.materials, vir::Access::RayTracingRead},
           {values.lights, vir::Access::RayTracingRead},
           {context.pointspot_page_table_attachment, vir::Access::RayTracingSampled},
           {context.vsm_physical_pages_attachment, vir::Access::RayTracingSampled},
           {values.atmosphere, vir::Access::RayTracingRead},
           {context.sky_view_lut_attachment, vir::Access::RayTracingSampled},
           {context.sky_transmittance_lut_attachment, vir::Access::RayTracingSampled},
           {context.probe_states_buffer, vir::Access::RayTracingRead},
           {context.light_grid_buffer, vir::Access::RayTracingRead},
           {context.pointspot_views_buffer, vir::Access::RayTracingRead}}
        )
        .with_name("ddgi trace")
        .bind_ray_tracing_pipeline(render_context.pipeline("ddgi_trace"));
    std::tie(
      context.ray_data_attachment,
      context.irradiance_attachment,
      context.distance_attachment,
      context.tlas_buffer,
      context.probe_volumes_buffer,
      values.mesh_instances,
      values.meshes,
      values.transforms_world,
      values.materials,
      values.lights,
      context.pointspot_page_table_attachment,
      context.vsm_physical_pages_attachment,
      values.atmosphere,
      context.sky_view_lut_attachment,
      context.sky_transmittance_lut_attachment,
      context.probe_states_buffer,
      context.light_grid_buffer,
      context.pointspot_views_buffer
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
        .bind_sampler(0, 9, linear_clamped)
        .bind_image(0, 10, context.ray_data_attachment)
        .bind_image(0, 11, context.irradiance_attachment)
        .bind_image(0, 12, context.distance_attachment)
        .bind_buffer(0, 13, context.probe_states_buffer)
        .bind_image(0, 14, context.pointspot_page_table_attachment)
        .bind_image(0, 15, context.vsm_physical_pages_attachment)
        .specialize_constant(0, std::to_underlying(recording.shape.scene_flags))
        .push_constant_address(0, values.atmosphere)
        .push_constants_from_at(8, volume.trace_push)
        .push_constant_address(80, context.light_grid_buffer)
        .push_constant_address(88, context.pointspot_views_buffer)
        .push_constants_from_at(96, volume.trace_tail_push)
        .trace_rays(variables.rays, volume.trace_probe_count, 1)
        .end_compute()
    );
  }

  // --- relocate ---
  std::tie(
    context.probe_states_buffer,
    context.ray_data_attachment,
    context.probe_volumes_buffer,
    context.probe_update_list_buffer,
    context.probe_update_args_buffer
  ) = std::tuple_cat( //
    module
      .begin_compute(
        {{context.probe_states_buffer, vir::Access::ComputeRW},
         {context.ray_data_attachment, vir::Access::ComputeSampled},
         {context.probe_volumes_buffer, vir::Access::ComputeRead},
         {context.probe_update_list_buffer, vir::Access::ComputeRead},
         {context.probe_update_args_buffer, vir::Access::IndirectRead | vir::Access::ComputeRead}}
      )
      .with_name("ddgi relocate probes")
      .bind_compute_pipeline(render_context.pipeline("ddgi_relocate_probes"))
      .bind_buffer(0, 0, context.probe_volumes_buffer)
      .bind_image(0, 1, context.ray_data_attachment)
      .bind_buffer(0, 2, context.probe_states_buffer)
      .bind_buffer(0, 3, context.probe_update_list_buffer)
      .bind_buffer(0, 4, context.probe_update_args_buffer)
      .push_constants_from(variables.relocate_push)
      .dispatch_indirect(context.probe_update_args_buffer, offsetof(GPU::ProbeUpdateArgs, relocate))
      .end_compute()
  );

  // --- update ---
  std::tie(
    context.irradiance_attachment,
    context.ray_data_attachment,
    context.probe_volumes_buffer,
    context.probe_states_buffer,
    context.probe_update_list_buffer,
    context.probe_update_args_buffer
  ) = std::tuple_cat( //
    module
      .begin_compute(
        {{context.irradiance_attachment, vir::Access::ComputeRW},
         {context.ray_data_attachment, vir::Access::ComputeSampled},
         {context.probe_volumes_buffer, vir::Access::ComputeRead},
         {context.probe_states_buffer, vir::Access::ComputeRead},
         {context.probe_update_list_buffer, vir::Access::ComputeRead},
         {context.probe_update_args_buffer, vir::Access::IndirectRead | vir::Access::ComputeRead}}
      )
      .with_name("ddgi update irradiance")
      .bind_compute_pipeline(render_context.pipeline("ddgi_update_irradiance"))
      .bind_buffer(0, 0, context.probe_volumes_buffer)
      .bind_image(0, 1, context.ray_data_attachment)
      .bind_image(0, 2, context.irradiance_attachment)
      .bind_buffer(0, 3, context.probe_states_buffer)
      .bind_buffer(0, 4, context.probe_update_list_buffer)
      .bind_buffer(0, 5, context.probe_update_args_buffer)
      .push_constants_from(variables.irradiance_push)
      .dispatch_indirect(context.probe_update_args_buffer, offsetof(GPU::ProbeUpdateArgs, irradiance))
      .end_compute()
  );

  std::tie(
    context.distance_attachment,
    context.ray_data_attachment,
    context.probe_volumes_buffer,
    context.probe_states_buffer,
    context.probe_update_list_buffer,
    context.probe_update_args_buffer
  ) = std::tuple_cat( //
    module
      .begin_compute(
        {{context.distance_attachment, vir::Access::ComputeRW},
         {context.ray_data_attachment, vir::Access::ComputeSampled},
         {context.probe_volumes_buffer, vir::Access::ComputeRead},
         {context.probe_states_buffer, vir::Access::ComputeRead},
         {context.probe_update_list_buffer, vir::Access::ComputeRead},
         {context.probe_update_args_buffer, vir::Access::IndirectRead | vir::Access::ComputeRead}}
      )
      .with_name("ddgi update distance")
      .bind_compute_pipeline(render_context.pipeline("ddgi_update_distance"))
      .bind_buffer(0, 0, context.probe_volumes_buffer)
      .bind_image(0, 1, context.ray_data_attachment)
      .bind_image(0, 2, context.distance_attachment)
      .bind_buffer(0, 3, context.probe_states_buffer)
      .bind_buffer(0, 4, context.probe_update_list_buffer)
      .bind_buffer(0, 5, context.probe_update_args_buffer)
      .push_constants_from(variables.distance_push)
      .dispatch_indirect(context.probe_update_args_buffer, offsetof(GPU::ProbeUpdateArgs, distance))
      .end_compute()
  );
}

auto RendererInstance::apply_ddgi(
  this RendererInstance& self,
  vir::Module& module,
  FrameRecording& recording,
  DDGIApplyContext& context,
  vir::ValueID dst
) -> vir::ValueID {
  ZoneScoped;
  auto& prepared = recording.prepared;
  auto& values = recording.values;

  auto& render_context = *self.renderer.render_context;
  auto& variables = prepared.ddgi;
  variables.apply_push = module.declare_bytes_var("ddgi apply push", sizeof(DDGIApplyPush));

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
    dst,
    context.depth_attachment,
    context.albedo_attachment,
    context.normal_attachment,
    context.metallic_roughness_occlusion_attachment,
    context.ambient_occlusion_attachment,
    context.irradiance_attachment,
    context.distance_attachment,
    values.camera,
    context.probe_volumes_buffer,
    context.probe_states_buffer
  ) = std::tuple_cat( //
    module
      .begin_rendering(
        {{dst, vir::Access::ColorRW},
         {context.depth_attachment, vir::Access::FragmentSampled},
         {context.albedo_attachment, vir::Access::FragmentSampled},
         {context.normal_attachment, vir::Access::FragmentSampled},
         {context.metallic_roughness_occlusion_attachment, vir::Access::FragmentSampled},
         {context.ambient_occlusion_attachment, vir::Access::FragmentSampled},
         {context.irradiance_attachment, vir::Access::FragmentSampled},
         {context.distance_attachment, vir::Access::FragmentSampled},
         {values.camera, vir::Access::FragmentUniformRead},
         {context.probe_volumes_buffer, vir::Access::FragmentRead},
         {context.probe_states_buffer, vir::Access::FragmentRead}}
      )
      .with_name("ddgi apply")
      .bind_graphics_pipeline(render_context.pipeline("ddgi_apply"))
      .set_color_blend(0, additive_blend)
      .set_viewport(0, vir::Rect2D::framebuffer())
      .set_scissor(0, vir::Rect2D::framebuffer())
      .bind_sampler(0, 0, render_context.cached_sampler_handle(LinearSamplerClamped))
      .bind_buffer(0, 1, values.camera)
      .bind_buffer(0, 2, context.probe_volumes_buffer)
      .bind_image(0, 3, context.depth_attachment)
      .bind_image(0, 4, context.albedo_attachment)
      .bind_image(0, 5, context.normal_attachment)
      .bind_image(0, 6, context.metallic_roughness_occlusion_attachment)
      .bind_image(0, 7, context.ambient_occlusion_attachment)
      .bind_image(0, 8, context.irradiance_attachment)
      .bind_image(0, 9, context.distance_attachment)
      .bind_buffer(0, 10, context.probe_states_buffer)
      .push_constants_from(prepared.ddgi.apply_push)
      .draw(3, 1)
      .end_rendering()
  );

  return dst;
}

auto RendererInstance::draw_ddgi_probes(
  vir::Module& module,
  RenderContext& render_context,
  FrameRecording& recording,
  DDGIDebugContext& context,
  vir::ValueID dst
) -> vir::ValueID {
  ZoneScoped;
  auto& prepared = recording.prepared;
  auto& values = recording.values;

  memory::ScopedStack stack;
  prepared.ddgi.volumes.resize(recording.shape.probe_volume_count);

  auto pass = //
    module
      .begin_rendering(
        {{dst, vir::Access::ColorWrite},
         {context.depth_attachment, vir::Access::DepthStencilRW},
         {values.camera, vir::Access::VertexUniformRead},
         {context.probe_volumes_buffer, vir::Access::VertexRead},
         {context.irradiance_attachment, vir::Access::FragmentSampled},
         {context.probe_states_buffer, vir::Access::VertexRead}}
      )
      .with_name("ddgi debug probes")
      .bind_graphics_pipeline(render_context.pipeline("ddgi_debug_probes"))
      .set_depth({.is_test_enabled = true, .is_write_enabled = true, .compare_op = VK_COMPARE_OP_GREATER_OR_EQUAL})
      .set_viewport(0, vir::Rect2D::framebuffer())
      .set_scissor(0, vir::Rect2D::framebuffer())
      .bind_buffer(0, 0, values.camera)
      .bind_buffer(0, 1, context.probe_volumes_buffer)
      .bind_image(0, 2, context.irradiance_attachment)
      .bind_sampler(0, 3, render_context.cached_sampler_handle(LinearSamplerClamped))
      .bind_buffer(0, 4, context.probe_states_buffer);

  for (u32 volume_index = 0; volume_index < recording.shape.probe_volume_count; volume_index++) {
    auto& volume = prepared.ddgi.volumes[volume_index];
    volume.debug_push = module.declare_bytes_var(
      stack.format("ddgi debug probes push {}", volume_index),
      sizeof(DDGIDebugPush)
    );
    volume.debug_probe_count = module.declare_u32_var(stack.format("ddgi debug volume {} probes", volume_index), 0);

    module.push_constants_from(volume.debug_push).draw(GPU::DDGI_DEBUG_SPHERE_VERTEX_COUNT, volume.debug_probe_count);
  }

  std::tie(
    dst,
    context.depth_attachment,
    values.camera,
    context.probe_volumes_buffer,
    context.irradiance_attachment,
    context.probe_states_buffer
  ) = std::tuple_cat(pass.end_rendering());

  return dst;
}
} // namespace ox
