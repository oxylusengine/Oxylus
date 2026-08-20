#include <vuk/runtime/CommandBuffer.hpp>

#include "Render/RendererInstance.hpp"
#include "Render/Utils/VukCommon.hpp"

namespace ox {
auto RendererInstance::trace_ddgi_probes(this RendererInstance& self, DDGITraceContext& context) -> void {
  ZoneScoped;

  auto probe_counts = ankerl::svector<u32, 4>{};
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
     normal_bias = context.normal_bias,
     sun_direction = context.sun_direction,
     sun_intensity = context.sun_intensity,
     ambient_color = context.ambient_color](
      vuk::CommandBuffer& cmd_list,
      VUK_IA(vuk::eComputeWrite) ray_data,
      VUK_BA(vuk::eComputeRead | vuk::eAccelerationStructureBuildRead) tlas_buffer,
      VUK_BA(vuk::eComputeRead) probe_volumes,
      VUK_BA(vuk::eComputeRead) mesh_instances,
      VUK_BA(vuk::eComputeRead) meshes,
      VUK_BA(vuk::eComputeRead) transforms,
      VUK_BA(vuk::eComputeRead) materials,
      VUK_BA(vuk::eComputeRead) lights,
      VUK_BA(vuk::eComputeRead) atmosphere,
      VUK_IA(vuk::eComputeSampled) sky_view_lut,
      VUK_IA(vuk::eComputeSampled) sky_transmittance_lut
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
        .specialize_constants(0, std::to_underlying(scene_flags));

      for (u32 volume_index = 0; volume_index < static_cast<u32>(probe_counts.size()); volume_index++) {
        cmd_list //
          .push_constants(
            vuk::ShaderStageFlagBits::eCompute,
            0,
            PushConstants(
              atmosphere->device_address,
              volume_index,
              rays_per_probe,
              frame_index,
              light_count,
              max_ray_distance,
              normal_bias,
              sun_direction,
              sun_intensity,
              ambient_color
            )
          )
          .dispatch((rays_per_probe + 31) / 32, probe_counts[volume_index], 1);
      }

      return std::make_tuple(
        ray_data,
        tlas_buffer,
        probe_volumes,
        mesh_instances,
        meshes,
        transforms,
        materials,
        lights,
        atmosphere,
        sky_view_lut,
        sky_transmittance_lut
      );
    }
  );

  std::tie(
    context.ray_data_attachment,
    context.tlas_buffer,
    context.probe_volumes_buffer,
    self.prepared_frame.mesh_instances_buffer,
    self.prepared_frame.meshes_buffer,
    self.prepared_frame.transforms_world_buffer,
    self.prepared_frame.materials_buffer,
    self.prepared_frame.lights_buffer,
    self.prepared_frame.atmosphere_buffer,
    context.sky_view_lut_attachment,
    context.sky_transmittance_lut_attachment
  ) =
    trace_pass(
      std::move(context.ray_data_attachment),
      std::move(context.tlas_buffer),
      std::move(context.probe_volumes_buffer),
      std::move(self.prepared_frame.mesh_instances_buffer),
      std::move(self.prepared_frame.meshes_buffer),
      std::move(self.prepared_frame.transforms_world_buffer),
      std::move(self.prepared_frame.materials_buffer),
      std::move(self.prepared_frame.lights_buffer),
      std::move(self.prepared_frame.atmosphere_buffer),
      std::move(context.sky_view_lut_attachment),
      std::move(context.sky_transmittance_lut_attachment)
    );
}

auto RendererInstance::draw_ddgi_probes(
  this RendererInstance& self, DDGIDebugContext& context, vuk::Value<vuk::ImageAttachment>&& dst_attachment
) -> vuk::Value<vuk::ImageAttachment> {
  ZoneScoped;

  auto probe_counts = ankerl::svector<u32, 4>{};
  for (const auto& volume : self.probe_volumes) {
    probe_counts.emplace_back(volume.probe_count);
  }

  auto probe_pass = vuk::make_pass(
    "ddgi debug probes",
    [probe_counts,
     probe_radius = context.probe_radius,
     rays_per_probe = context.rays_per_probe,
     frame_index = context.frame_index](
      vuk::CommandBuffer& cmd_list,
      VUK_IA(vuk::eColorWrite) dst,
      VUK_IA(vuk::eDepthStencilRW) depth,
      VUK_BA(vuk::eVertexRead) camera,
      VUK_BA(vuk::eVertexRead) probe_volumes,
      VUK_IA(vuk::eFragmentSampled) ray_data
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
        .bind_image(0, 2, ray_data);

      for (u32 volume_index = 0; volume_index < static_cast<u32>(probe_counts.size()); volume_index++) {
        cmd_list //
          .push_constants(
            vuk::ShaderStageFlagBits::eVertex | vuk::ShaderStageFlagBits::eFragment,
            0,
            PushConstants(volume_index, rays_per_probe, frame_index, probe_radius)
          )
          .draw(GPU::DDGI_DEBUG_SPHERE_VERTEX_COUNT, probe_counts[volume_index], 0, 0);
      }

      return std::make_tuple(dst, depth, camera, probe_volumes, ray_data);
    }
  );

  std::tie(
    dst_attachment,
    context.depth_attachment,
    self.prepared_frame.camera_buffer,
    context.probe_volumes_buffer,
    context.ray_data_attachment
  ) =
    probe_pass(
      std::move(dst_attachment),
      std::move(context.depth_attachment),
      std::move(self.prepared_frame.camera_buffer),
      std::move(context.probe_volumes_buffer),
      std::move(context.ray_data_attachment)
    );

  return dst_attachment;
}
} // namespace ox
