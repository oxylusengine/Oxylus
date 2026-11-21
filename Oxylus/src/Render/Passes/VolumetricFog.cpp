#include <array>
#include <utility>
#include <vir/RenderGraph.hpp>
#include <vir/Types.hpp>

#include "Render/RenderContext.hpp"
#include "Render/RendererInstance.hpp"
#include "Render/Utils/Samplers.hpp"
#include "Utils/Log.hpp"

namespace ox {
constexpr static auto FROXEL_HISTORY_REST = vir::Access::ComputeSampled;

// one texel deeper than the slices, see FROXEL_DEPTH_TEXELS
static auto froxel_texture_extent(const glm::uvec2 render_size) -> vir::Extent3D {
  return {
    (render_size.x + GPU::FROXEL_TILE_SIZE - 1) / GPU::FROXEL_TILE_SIZE,
    (render_size.y + GPU::FROXEL_TILE_SIZE - 1) / GPU::FROXEL_TILE_SIZE,
    GPU::FROXEL_DEPTH_TEXELS,
  };
}

// matches DDGIApplyPush, zeroed when the frame has no probes to sample
static auto push_ddgi_ambient(vir::Module& module, const FrameRecording& recording, u32 offset) -> void {
  if (recording.shape.has_ddgi) {
    module.push_constants_from_at(offset, recording.prepared.ddgi.apply_push);
    return;
  }

  module.push_constants_at(offset, std::array<u32, 7>{});
}

auto RendererInstance::allocate_volumetric_fog_resources(this RendererInstance& self, const glm::uvec2 render_size)
  -> void {
  ZoneScoped;

  if (self.volumetric_fog_render_size == render_size) {
    return;
  }

  const auto extent = froxel_texture_extent(render_size);
  for (auto& history : self.volumetric_fog_history) {
    history = Texture::create({
      .format = vir::Format::RGBA16Sfloat,
      .extent = extent,
      .image_type = vir::ImageType::e3D,
      .usage = vir::ImageUsageFlags::Storage | vir::ImageUsageFlags::Sampled | vir::ImageUsageFlags::TransferDst,
      .view_type = vir::ImageViewType::e3D,
    });
  }

  auto module = vir::Module{};
  for (const auto& history : self.volumetric_fog_history) {
    const auto cleared = module.clear(
      history.import_discarded(module, vir::Access::None),
      vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 0.0f)
    );
    module.export_to(cleared, FROXEL_HISTORY_REST);
  }

  if (const auto initialized = self.renderer.render_context->execute_blocking(std::move(module)); !initialized) {
    OX_LOG_FATAL("failed to initialize the volumetric fog history ({})", std::to_underlying(initialized.error()));
  }

  self.volumetric_fog_render_size = render_size;
  self.volumetric_fog_history_valid = false;
}

auto RendererInstance::record_volumetric_fog_tile_depth(
  this RendererInstance& self, vir::Module& module, FrameRecording& recording, vir::ValueID& depth_attachment
) -> vir::ValueID {
  ZoneScoped;
  auto& render_context = *self.renderer.render_context;
  const auto tile_extent = froxel_texture_extent(recording.shape.render_size);

  auto tile_depth = module.transient_image({
    .extent = {tile_extent.width, tile_extent.height, 1},
    .format = vir::Format::R32Sfloat,
    .name = "volumetric fog tile depth",
  });
  std::tie(recording.values.camera, depth_attachment, tile_depth) = std::tuple_cat(
    module
      .begin_compute(
        {{recording.values.camera, vir::Access::ComputeUniformRead},
         {depth_attachment, vir::Access::ComputeSampled},
         {tile_depth, vir::Access::ComputeWrite}}
      )
      .with_name("volumetric fog tile depth")
      .bind_compute_pipeline(render_context.pipeline("volumetric_fog_tile_depth"))
      .bind_buffer(0, 0, recording.values.camera)
      .bind_image(0, 1, depth_attachment)
      .bind_image(0, 2, tile_depth)
      .dispatch(tile_extent.width, tile_extent.height, 1)
      .end_compute()
  );

  return tile_depth;
}

auto RendererInstance::record_volumetric_fog(
  this RendererInstance& self, vir::Module& module, FrameRecording& recording, VolumetricFogContext& context
) -> void {
  ZoneScoped;
  auto& values = recording.values;
  auto& variables = recording.prepared.volumetric_fog;
  auto& render_context = *self.renderer.render_context;
  const auto& shape = recording.shape;
  const auto texture_extent = froxel_texture_extent(shape.render_size);

  variables.params = module.declare_buffer_var("volumetric fog params", vir::Access::None);
  variables.fog_volumes = module.declare_buffer_var("fog volumes", vir::Access::None);
  variables.history = module
                        .declare_image_var("volumetric fog history", vir::Format::RGBA16Sfloat, FROXEL_HISTORY_REST);
  variables.previous_history = module.declare_image_var(
    "volumetric fog previous history",
    vir::Format::RGBA16Sfloat,
    FROXEL_HISTORY_REST
  );
  auto params = variables.params;
  auto fog_volumes = variables.fog_volumes;
  auto history = variables.history;
  auto previous_history = variables.previous_history;

  auto inject = bind_vsm_pointspot_spec_constants(
                  module.begin_compute(
                    {{values.camera, vir::Access::ComputeUniformRead},
                     {params, vir::Access::ComputeUniformRead},
                     {fog_volumes, vir::Access::ComputeRead},
                     {values.lights, vir::Access::ComputeRead},
                     {context.lighting_buffer, vir::Access::ComputeRead},
                     {context.light_grid_buffer, vir::Access::ComputeRead},
                     {context.pointspot_views_buffer, vir::Access::ComputeRead},
                     {context.directional_clipmaps_buffer, vir::Access::ComputeRead},
                     {context.vsm_page_table_attachment, vir::Access::ComputeSampled},
                     {context.pointspot_page_table_attachment, vir::Access::ComputeSampled},
                     {context.vsm_physical_pages_attachment, vir::Access::ComputeSampled},
                     {context.probe_volumes_buffer, vir::Access::ComputeRead},
                     {context.probe_states_buffer, vir::Access::ComputeRead},
                     {context.irradiance_attachment, vir::Access::ComputeSampled},
                     {context.distance_attachment, vir::Access::ComputeSampled},
                     {context.sky_cubemap_attachment, vir::Access::ComputeSampled},
                     {context.tile_depth_attachment, vir::Access::ComputeSampled},
                     {previous_history, vir::Access::ComputeSampled},
                     {history, vir::Access::ComputeWrite}}
                  )
  )
                  .with_name("volumetric fog inject")
                  .bind_compute_pipeline(render_context.pipeline("volumetric_fog_inject"))
                  .bind_buffer(0, 0, values.camera)
                  .bind_buffer(0, 1, params)
                  .bind_sampler(0, 2, render_context.cached_sampler_handle(NearestSamplerClamped))
                  .bind_sampler(0, 3, render_context.cached_sampler_handle(LinearSamplerClamped))
                  .bind_image(0, 4, context.vsm_page_table_attachment)
                  .bind_image(0, 5, context.pointspot_page_table_attachment)
                  .bind_image(0, 6, context.vsm_physical_pages_attachment)
                  .bind_image(0, 7, previous_history)
                  .bind_image(0, 8, history)
                  .bind_buffer(0, 9, context.probe_volumes_buffer)
                  .bind_buffer(0, 10, context.probe_states_buffer)
                  .bind_image(0, 11, context.irradiance_attachment)
                  .bind_image(0, 12, context.distance_attachment)
                  .bind_image(0, 13, context.sky_cubemap_attachment)
                  .bind_image(0, 14, context.tile_depth_attachment)
                  .push_constant_address(0, context.lighting_buffer)
                  .push_constant_address(8, values.lights)
                  .push_constant_address(16, context.light_grid_buffer)
                  .push_constant_address(24, context.pointspot_views_buffer)
                  .push_constant_address(32, context.directional_clipmaps_buffer)
                  .push_constant_address(40, fog_volumes)
                  .specialize_constant(0, std::to_underlying(shape.scene_flags))
                  .specialize_constant(1, static_cast<u32>(shape.has_directional_shadows));
  push_ddgi_ambient(module, recording, 48);
  std::tie(
    values.camera,
    params,
    fog_volumes,
    values.lights,
    context.lighting_buffer,
    context.light_grid_buffer,
    context.pointspot_views_buffer,
    context.directional_clipmaps_buffer,
    context.vsm_page_table_attachment,
    context.pointspot_page_table_attachment,
    context.vsm_physical_pages_attachment,
    context.probe_volumes_buffer,
    context.probe_states_buffer,
    context.irradiance_attachment,
    context.distance_attachment,
    context.sky_cubemap_attachment,
    context.tile_depth_attachment,
    previous_history,
    history
  ) = std::tuple_cat(inject.dispatch_invocations(texture_extent.width, texture_extent.height, GPU::FROXEL_DEPTH_SLICES)
                       .end_compute());

  context.integrated_attachment = module.transient_image({
    .extent = texture_extent,
    .format = vir::Format::RGBA16Sfloat,
    .image_type = vir::ImageType::e3D,
    .view_type = vir::ImageViewType::e3D,
    .name = "volumetric fog integrated",
  });
  std::tie(values.camera, params, history, context.integrated_attachment) = std::tuple_cat(
    module
      .begin_compute(
        {{values.camera, vir::Access::ComputeUniformRead},
         {params, vir::Access::ComputeUniformRead},
         {history, vir::Access::ComputeSampled},
         {context.integrated_attachment, vir::Access::ComputeWrite}}
      )
      .with_name("volumetric fog integrate")
      .bind_compute_pipeline(render_context.pipeline("volumetric_fog_integrate"))
      .bind_buffer(0, 0, values.camera)
      .bind_buffer(0, 1, params)
      .bind_image(0, 2, history)
      .bind_image(0, 3, context.integrated_attachment)
      .dispatch_invocations(texture_extent.width, texture_extent.height, 1)
      .end_compute()
  );

  module.export_to(history, FROXEL_HISTORY_REST);
}
} // namespace ox
