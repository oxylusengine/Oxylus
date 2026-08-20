#include <vuk/runtime/CommandBuffer.hpp>

#include "Render/RendererInstance.hpp"
#include "Render/Utils/VukCommon.hpp"

namespace ox {
auto RendererInstance::allocate_ddgi_atlases(this RendererInstance& self, u32 probe_count) -> void {
  ZoneScoped;

  if (self.ddgi_atlas_probe_count == probe_count) {
    return;
  }

  auto& allocator = *self.renderer.render_context->superframe_allocator;

  self.ddgi_irradiance_attachment = vuk::ImageAttachment{
    // eTransferDst: a freshly allocated atlas is cleared before its first update.
    .usage = vuk::ImageUsageFlagBits::eStorage | vuk::ImageUsageFlagBits::eSampled |
             vuk::ImageUsageFlagBits::eTransferDst,
    .extent = GPU::ddgi_atlas_extent(probe_count, GPU::DDGI_IRRADIANCE_TEXELS),
    .format = vuk::Format::eR16G16B16A16Sfloat,
    .sample_count = vuk::Samples::e1,
    .view_type = vuk::ImageViewType::e2D,
    .base_level = 0,
    .level_count = 1,
    .base_layer = 0,
    .layer_count = 1,
  };
  self.ddgi_irradiance = *vuk::allocate_image(allocator, self.ddgi_irradiance_attachment);
  self.ddgi_irradiance_attachment.image = *self.ddgi_irradiance;
  self.ddgi_irradiance_view = *vuk::allocate_image_view(allocator, self.ddgi_irradiance_attachment);
  self.ddgi_irradiance_attachment.image_view = *self.ddgi_irradiance_view;

  self.ddgi_distance_attachment = vuk::ImageAttachment{
    // eTransferDst: a freshly allocated atlas is cleared before its first update.
    .usage = vuk::ImageUsageFlagBits::eStorage | vuk::ImageUsageFlagBits::eSampled |
             vuk::ImageUsageFlagBits::eTransferDst,
    .extent = GPU::ddgi_atlas_extent(probe_count, GPU::DDGI_DISTANCE_TEXELS),
    .format = vuk::Format::eR16G16Sfloat,
    .sample_count = vuk::Samples::e1,
    .view_type = vuk::ImageViewType::e2D,
    .base_level = 0,
    .level_count = 1,
    .base_layer = 0,
    .layer_count = 1,
  };
  self.ddgi_distance = *vuk::allocate_image(allocator, self.ddgi_distance_attachment);
  self.ddgi_distance_attachment.image = *self.ddgi_distance;
  self.ddgi_distance_view = *vuk::allocate_image_view(allocator, self.ddgi_distance_attachment);
  self.ddgi_distance_attachment.image_view = *self.ddgi_distance_view;

  self.ddgi_probe_states = self.renderer.render_context->allocate_buffer_super(
    vuk::MemoryUsage::eGPUonly,
    sizeof(GPU::ProbeState) * probe_count
  );

  self.ddgi_atlas_probe_count = probe_count;
  self.ddgi_history_valid = false;
}

auto RendererInstance::select_ddgi_probes(this RendererInstance& self, DDGISelectContext& context) -> void {
  ZoneScoped;

  auto probe_counts = ankerl::svector<u32, 8>{};
  auto scroll_deltas = ankerl::svector<glm::ivec3, 8>{};
  const auto scrolls_known = self.probe_volume_scrolls.size() == self.probe_volumes.size();

  for (u32 index = 0; index < static_cast<u32>(self.probe_volumes.size()); index++) {
    const auto& volume = self.probe_volumes[index];
    probe_counts.emplace_back(volume.probe_count);

    scroll_deltas.emplace_back(
      scrolls_known ? volume.scroll - self.probe_volume_scrolls[index] : glm::ivec3(volume.counts)
    );
  }

  self.probe_volume_scrolls.clear();
  for (const auto& volume : self.probe_volumes) {
    self.probe_volume_scrolls.emplace_back(volume.scroll);
  }

  auto select_pass = vuk::make_pass(
    "ddgi select probes",
    [probe_counts,
     scroll_deltas,
     frame_index = context.frame_index,
     max_interval = context.max_interval,
     full_rate_distance = context.full_rate_distance,
     update_all = static_cast<u32>(context.update_all)](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eComputeRW) probe_states,
      VUK_BA(vuk::eComputeRead) probe_volumes,
      VUK_BA(vuk::eComputeUniformRead) camera
    ) {
      cmd_list //
        .bind_compute_pipeline("ddgi_select_probes")
        .bind_buffer(0, 0, camera)
        .bind_buffer(0, 1, probe_volumes)
        .bind_buffer(0, 2, probe_states);

      for (u32 volume_index = 0; volume_index < static_cast<u32>(probe_counts.size()); volume_index++) {
        cmd_list //
          .push_constants(
            vuk::ShaderStageFlagBits::eCompute,
            0,
            PushConstants(
              volume_index,
              frame_index,
              max_interval,
              full_rate_distance,
              update_all,
              scroll_deltas[volume_index]
            )
          )
          .dispatch_invocations(probe_counts[volume_index], 1, 1);
      }

      return std::make_tuple(probe_states, probe_volumes, camera);
    }
  );

  std::tie(context.probe_states_buffer, context.probe_volumes_buffer, self.prepared_frame.camera_buffer) = select_pass(
    std::move(context.probe_states_buffer),
    std::move(context.probe_volumes_buffer),
    std::move(self.prepared_frame.camera_buffer)
  );
}

auto RendererInstance::relocate_ddgi_probes(this RendererInstance& self, DDGIRelocateContext& context) -> void {
  ZoneScoped;

  auto probe_counts = ankerl::svector<u32, 8>{};
  for (const auto& volume : self.probe_volumes) {
    probe_counts.emplace_back(volume.probe_count);
  }

  auto relocate_pass = vuk::make_pass(
    "ddgi relocate probes",
    [probe_counts,
     rays_per_probe = context.rays_per_probe,
     frame_index = context.frame_index,
     min_frontface_distance = context.min_frontface_distance](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eComputeRW) probe_states,
      VUK_IA(vuk::eComputeSampled) ray_data,
      VUK_BA(vuk::eComputeRead) probe_volumes
    ) {
      cmd_list //
        .bind_compute_pipeline("ddgi_relocate_probes")
        .bind_buffer(0, 0, probe_volumes)
        .bind_image(0, 1, ray_data)
        .bind_buffer(0, 2, probe_states);

      for (u32 volume_index = 0; volume_index < static_cast<u32>(probe_counts.size()); volume_index++) {
        cmd_list //
          .push_constants(
            vuk::ShaderStageFlagBits::eCompute,
            0,
            PushConstants(volume_index, rays_per_probe, frame_index, min_frontface_distance)
          )
          .dispatch((probe_counts[volume_index] + 63) / 64, 1, 1);
      }

      return std::make_tuple(probe_states, ray_data, probe_volumes);
    }
  );

  std::tie(context.probe_states_buffer, context.ray_data_attachment, context.probe_volumes_buffer) = relocate_pass(
    std::move(context.probe_states_buffer),
    std::move(context.ray_data_attachment),
    std::move(context.probe_volumes_buffer)
  );
}

auto RendererInstance::update_ddgi_probes(this RendererInstance& self, DDGIUpdateContext& context) -> void {
  ZoneScoped;

  auto probe_counts = ankerl::svector<u32, 8>{};
  for (const auto& volume : self.probe_volumes) {
    probe_counts.emplace_back(volume.probe_count);
  }

  const auto hysteresis = self.ddgi_history_valid ? context.hysteresis : 0.0f;

  auto irradiance_pass = vuk::make_pass(
    "ddgi update irradiance",
    [probe_counts, rays_per_probe = context.rays_per_probe, frame_index = context.frame_index, hysteresis](
      vuk::CommandBuffer& cmd_list,
      VUK_IA(vuk::eComputeRW) irradiance,
      VUK_IA(vuk::eComputeSampled) ray_data,
      VUK_BA(vuk::eComputeRead) probe_volumes,
      VUK_BA(vuk::eComputeRead) probe_states
    ) {
      cmd_list //
        .bind_compute_pipeline("ddgi_update_irradiance")
        .bind_buffer(0, 0, probe_volumes)
        .bind_image(0, 1, ray_data)
        .bind_image(0, 2, irradiance)
        .bind_buffer(0, 3, probe_states);

      for (u32 volume_index = 0; volume_index < static_cast<u32>(probe_counts.size()); volume_index++) {
        cmd_list //
          .push_constants(
            vuk::ShaderStageFlagBits::eCompute,
            0,
            PushConstants(volume_index, rays_per_probe, frame_index, hysteresis)
          )
          .dispatch(probe_counts[volume_index], 1, 1);
      }

      return std::make_tuple(irradiance, ray_data, probe_volumes, probe_states);
    }
  );

  std::tie(
    context.irradiance_attachment,
    context.ray_data_attachment,
    context.probe_volumes_buffer,
    context.probe_states_buffer
  ) =
    irradiance_pass(
      std::move(context.irradiance_attachment),
      std::move(context.ray_data_attachment),
      std::move(context.probe_volumes_buffer),
      std::move(context.probe_states_buffer)
    );

  auto distance_pass = vuk::make_pass(
    "ddgi update distance",
    [probe_counts, rays_per_probe = context.rays_per_probe, frame_index = context.frame_index, hysteresis](
      vuk::CommandBuffer& cmd_list,
      VUK_IA(vuk::eComputeRW) probe_distance,
      VUK_IA(vuk::eComputeSampled) ray_data,
      VUK_BA(vuk::eComputeRead) probe_volumes,
      VUK_BA(vuk::eComputeRead) probe_states
    ) {
      cmd_list //
        .bind_compute_pipeline("ddgi_update_distance")
        .bind_buffer(0, 0, probe_volumes)
        .bind_image(0, 1, ray_data)
        .bind_image(0, 2, probe_distance)
        .bind_buffer(0, 3, probe_states);

      for (u32 volume_index = 0; volume_index < static_cast<u32>(probe_counts.size()); volume_index++) {
        cmd_list //
          .push_constants(
            vuk::ShaderStageFlagBits::eCompute,
            0,
            PushConstants(volume_index, rays_per_probe, frame_index, hysteresis)
          )
          .dispatch(probe_counts[volume_index], 1, 1);
      }

      return std::make_tuple(probe_distance, ray_data, probe_volumes, probe_states);
    }
  );

  std::tie(
    context.distance_attachment,
    context.ray_data_attachment,
    context.probe_volumes_buffer,
    context.probe_states_buffer
  ) =
    distance_pass(
      std::move(context.distance_attachment),
      std::move(context.ray_data_attachment),
      std::move(context.probe_volumes_buffer),
      std::move(context.probe_states_buffer)
    );

  auto border_pass = vuk::make_pass(
    "ddgi update borders",
    [probe_counts](
      vuk::CommandBuffer& cmd_list,
      VUK_IA(vuk::eComputeRW) irradiance,
      VUK_IA(vuk::eComputeRW) probe_distance,
      VUK_BA(vuk::eComputeRead) probe_volumes,
      VUK_BA(vuk::eComputeRead) probe_states
    ) {
      cmd_list //
        .bind_compute_pipeline("ddgi_update_borders")
        .bind_buffer(0, 0, probe_volumes)
        .bind_image(0, 1, irradiance)
        .bind_image(0, 2, probe_distance)
        .bind_buffer(0, 3, probe_states);

      for (u32 volume_index = 0; volume_index < static_cast<u32>(probe_counts.size()); volume_index++) {
        cmd_list //
          .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(volume_index))
          .dispatch(probe_counts[volume_index], 1, 1);
      }

      return std::make_tuple(irradiance, probe_distance, probe_volumes, probe_states);
    }
  );

  std::tie(
    context.irradiance_attachment,
    context.distance_attachment,
    context.probe_volumes_buffer,
    context.probe_states_buffer
  ) =
    border_pass(
      std::move(context.irradiance_attachment),
      std::move(context.distance_attachment),
      std::move(context.probe_volumes_buffer),
      std::move(context.probe_states_buffer)
    );

  self.ddgi_history_valid = true;
}

auto RendererInstance::trace_ddgi_probes(this RendererInstance& self, DDGITraceContext& context) -> void {
  ZoneScoped;

  auto probe_counts = ankerl::svector<u32, 8>{};
  for (const auto& volume : self.probe_volumes) {
    probe_counts.emplace_back(volume.probe_count);
  }

  auto trace_pass = vuk::make_pass(
    "ddgi trace",
    [probe_counts,
     &descriptor_set = *context.bindless_set,
     tlas = *context.tlas->acceleration_structure.handle,
     scene_flags = context.scene_flags,
     rays_per_probe = context.rays_per_probe,
     frame_index = context.frame_index,
     light_count = context.light_count,
     max_ray_distance = context.max_ray_distance,
     shadow_ray_offset = context.shadow_ray_offset,
     normal_bias = context.normal_bias,
     sun_direction = context.sun_direction,
     sun_intensity = context.sun_intensity,
     ambient_color = context.ambient_color,
     max_ray_radiance = context.max_ray_radiance,
     volume_count = context.volume_count,
     bounce_valid = static_cast<u32>(context.bounce_valid),
     view_bias = context.view_bias](
      vuk::CommandBuffer& cmd_list,
      VUK_IA(vuk::eComputeWrite) ray_data,
      VUK_IA(vuk::eComputeSampled) irradiance,
      VUK_IA(vuk::eComputeSampled) probe_distance,
      VUK_BA(vuk::eComputeRead | vuk::eAccelerationStructureBuildRead) tlas_buffer,
      VUK_BA(vuk::eComputeRead) probe_volumes,
      VUK_BA(vuk::eComputeRead) mesh_instances,
      VUK_BA(vuk::eComputeRead) meshes,
      VUK_BA(vuk::eComputeRead) transforms,
      VUK_BA(vuk::eComputeRead) materials,
      VUK_BA(vuk::eComputeRead) lights,
      VUK_BA(vuk::eComputeRead) atmosphere,
      VUK_IA(vuk::eComputeSampled) sky_view_lut,
      VUK_IA(vuk::eComputeSampled) sky_transmittance_lut,
      VUK_BA(vuk::eComputeRead) probe_states
    ) {
      cmd_list //
        .bind_compute_pipeline("ddgi_trace")
        .bind_persistent(1, descriptor_set)
        .bind_acceleration_structure(0, 0, tlas)
        .bind_buffer(0, 1, probe_volumes)
        .bind_buffer(0, 2, mesh_instances)
        .bind_buffer(0, 3, meshes)
        .bind_buffer(0, 4, transforms)
        .bind_buffer(0, 5, materials)
        .bind_buffer(0, 6, lights)
        .bind_image(0, 7, sky_view_lut)
        .bind_image(0, 8, sky_transmittance_lut)
        .bind_sampler(0, 9, vuk::LinearSamplerClamped)
        .bind_image(0, 10, ray_data)
        .bind_image(0, 11, irradiance)
        .bind_image(0, 12, probe_distance)
        .bind_buffer(0, 13, probe_states)
        .specialize_constants(0, std::to_underlying(scene_flags));

      for (u32 volume_index = 0; volume_index < static_cast<u32>(probe_counts.size()); volume_index++) {
        cmd_list //
          .push_constants(
            vuk::ShaderStageFlagBits::eCompute,
            0,
            PushConstants(
              atmosphere->device_address,
              volume_index,
              volume_count,
              rays_per_probe,
              frame_index,
              light_count,
              bounce_valid,
              max_ray_distance,
              shadow_ray_offset,
              normal_bias,
              view_bias,
              sun_direction,
              sun_intensity,
              ambient_color,
              max_ray_radiance
            )
          )
          .dispatch((rays_per_probe + 31) / 32, probe_counts[volume_index], 1);
      }

      return std::make_tuple(
        ray_data,
        irradiance,
        probe_distance,
        tlas_buffer,
        probe_volumes,
        mesh_instances,
        meshes,
        transforms,
        materials,
        lights,
        atmosphere,
        sky_view_lut,
        sky_transmittance_lut,
        probe_states
      );
    }
  );

  std::tie(
    context.ray_data_attachment,
    context.irradiance_attachment,
    context.distance_attachment,
    context.tlas_buffer,
    context.probe_volumes_buffer,
    self.prepared_frame.mesh_instances_buffer,
    self.prepared_frame.meshes_buffer,
    self.prepared_frame.transforms_world_buffer,
    self.prepared_frame.materials_buffer,
    self.prepared_frame.lights_buffer,
    self.prepared_frame.atmosphere_buffer,
    context.sky_view_lut_attachment,
    context.sky_transmittance_lut_attachment,
    context.probe_states_buffer
  ) =
    trace_pass(
      std::move(context.ray_data_attachment),
      std::move(context.irradiance_attachment),
      std::move(context.distance_attachment),
      std::move(context.tlas_buffer),
      std::move(context.probe_volumes_buffer),
      std::move(self.prepared_frame.mesh_instances_buffer),
      std::move(self.prepared_frame.meshes_buffer),
      std::move(self.prepared_frame.transforms_world_buffer),
      std::move(self.prepared_frame.materials_buffer),
      std::move(self.prepared_frame.lights_buffer),
      std::move(self.prepared_frame.atmosphere_buffer),
      std::move(context.sky_view_lut_attachment),
      std::move(context.sky_transmittance_lut_attachment),
      std::move(context.probe_states_buffer)
    );
}

auto RendererInstance::apply_ddgi(
  this RendererInstance& self, DDGIApplyContext& context, vuk::Value<vuk::ImageAttachment>&& dst
) -> vuk::Value<vuk::ImageAttachment> {
  ZoneScoped;

  auto apply_pass = vuk::make_pass(
    "ddgi apply",
    [volume_count = context.volume_count,
     normal_bias = context.normal_bias,
     view_bias = context.view_bias,
     intensity = context.intensity,
     ambient_color = context.ambient_color](
      vuk::CommandBuffer& cmd_list,
      VUK_IA(vuk::eColorRW) target,
      VUK_IA(vuk::eFragmentSampled) depth,
      VUK_IA(vuk::eFragmentSampled) albedo,
      VUK_IA(vuk::eFragmentSampled) normal,
      VUK_IA(vuk::eFragmentSampled) metallic_roughness_occlusion,
      VUK_IA(vuk::eFragmentSampled) gtao,
      VUK_IA(vuk::eFragmentSampled) irradiance,
      VUK_IA(vuk::eFragmentSampled) probe_distance,
      VUK_BA(vuk::eFragmentUniformRead) camera,
      VUK_BA(vuk::eFragmentRead) probe_volumes,
      VUK_BA(vuk::eFragmentRead) probe_states
    ) {
      cmd_list //
        .bind_graphics_pipeline("ddgi_apply")
        .set_rasterization({})
        .set_color_blend(
          target,
          vuk::PipelineColorBlendAttachmentState{
            .blendEnable = true,
            .srcColorBlendFactor = vuk::BlendFactor::eOne,
            .dstColorBlendFactor = vuk::BlendFactor::eOne,
            .colorBlendOp = vuk::BlendOp::eAdd,
            .srcAlphaBlendFactor = vuk::BlendFactor::eZero,
            .dstAlphaBlendFactor = vuk::BlendFactor::eOne,
            .alphaBlendOp = vuk::BlendOp::eAdd,
          }
        )
        .set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
        .set_viewport(0, vuk::Rect2D::framebuffer())
        .set_scissor(0, vuk::Rect2D::framebuffer())
        .bind_sampler(0, 0, vuk::LinearSamplerClamped)
        .bind_buffer(0, 1, camera)
        .bind_buffer(0, 2, probe_volumes)
        .bind_image(0, 3, depth)
        .bind_image(0, 4, albedo)
        .bind_image(0, 5, normal)
        .bind_image(0, 6, metallic_roughness_occlusion)
        .bind_image(0, 7, gtao)
        .bind_image(0, 8, irradiance)
        .bind_image(0, 9, probe_distance)
        .bind_buffer(0, 10, probe_states)
        .push_constants(
          vuk::ShaderStageFlagBits::eFragment,
          0,
          PushConstants(volume_count, normal_bias, view_bias, intensity, ambient_color)
        )
        .draw(3, 1, 0, 0);

      return std::make_tuple(
        target,
        depth,
        albedo,
        normal,
        metallic_roughness_occlusion,
        gtao,
        irradiance,
        probe_distance,
        camera,
        probe_volumes,
        probe_states
      );
    }
  );

  std::tie(
    dst,
    context.depth_attachment,
    context.albedo_attachment,
    context.normal_attachment,
    context.metallic_roughness_occlusion_attachment,
    context.ambient_occlusion_attachment,
    context.irradiance_attachment,
    context.distance_attachment,
    self.prepared_frame.camera_buffer,
    context.probe_volumes_buffer,
    context.probe_states_buffer
  ) =
    apply_pass(
      std::move(dst),
      std::move(context.depth_attachment),
      std::move(context.albedo_attachment),
      std::move(context.normal_attachment),
      std::move(context.metallic_roughness_occlusion_attachment),
      std::move(context.ambient_occlusion_attachment),
      std::move(context.irradiance_attachment),
      std::move(context.distance_attachment),
      std::move(self.prepared_frame.camera_buffer),
      std::move(context.probe_volumes_buffer),
      std::move(context.probe_states_buffer)
    );

  return dst;
}

auto RendererInstance::draw_ddgi_probes(
  this RendererInstance& self, DDGIDebugContext& context, vuk::Value<vuk::ImageAttachment>&& dst_attachment
) -> vuk::Value<vuk::ImageAttachment> {
  ZoneScoped;

  auto probe_counts = ankerl::svector<u32, 8>{};
  for (const auto& volume : self.probe_volumes) {
    probe_counts.emplace_back(volume.probe_count);
  }

  auto probe_pass = vuk::make_pass(
    "ddgi debug probes",
    [probe_counts, probe_radius = context.probe_radius, atlas_valid = static_cast<u32>(context.atlas_valid)](
      vuk::CommandBuffer& cmd_list,
      VUK_IA(vuk::eColorWrite) dst,
      VUK_IA(vuk::eDepthStencilRW) depth,
      VUK_BA(vuk::eVertexRead) camera,
      VUK_BA(vuk::eVertexRead) probe_volumes,
      VUK_IA(vuk::eFragmentSampled) irradiance,
      VUK_BA(vuk::eVertexRead) probe_states
    ) {
      cmd_list //
        .bind_graphics_pipeline("ddgi_debug_probes")
        .set_rasterization({.cullMode = vuk::CullModeFlagBits::eNone})
        .set_depth_stencil(
          {.depthTestEnable = true, .depthWriteEnable = true, .depthCompareOp = vuk::CompareOp::eGreaterOrEqual}
        )
        .set_color_blend(dst, vuk::BlendPreset::eOff)
        .set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
        .set_viewport(0, vuk::Rect2D::framebuffer())
        .set_scissor(0, vuk::Rect2D::framebuffer())
        .bind_buffer(0, 0, camera)
        .bind_buffer(0, 1, probe_volumes)
        .bind_image(0, 2, irradiance)
        .bind_sampler(0, 3, vuk::LinearSamplerClamped)
        .bind_buffer(0, 4, probe_states);

      for (u32 volume_index = 0; volume_index < static_cast<u32>(probe_counts.size()); volume_index++) {
        cmd_list //
          .push_constants(
            vuk::ShaderStageFlagBits::eVertex | vuk::ShaderStageFlagBits::eFragment,
            0,
            PushConstants(volume_index, atlas_valid, probe_radius)
          )
          .draw(GPU::DDGI_DEBUG_SPHERE_VERTEX_COUNT, probe_counts[volume_index], 0, 0);
      }

      return std::make_tuple(dst, depth, camera, probe_volumes, irradiance, probe_states);
    }
  );

  std::tie(
    dst_attachment,
    context.depth_attachment,
    self.prepared_frame.camera_buffer,
    context.probe_volumes_buffer,
    context.irradiance_attachment,
    context.probe_states_buffer
  ) =
    probe_pass(
      std::move(dst_attachment),
      std::move(context.depth_attachment),
      std::move(self.prepared_frame.camera_buffer),
      std::move(context.probe_volumes_buffer),
      std::move(context.irradiance_attachment),
      std::move(context.probe_states_buffer)
    );

  return dst_attachment;
}
} // namespace ox
