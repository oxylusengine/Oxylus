#include <ankerl/svector.h>
#include <ranges>
#include <vir/RenderGraph.hpp>
#include <vir/Types.hpp>

#include "Asset/Texture.hpp"
#include "Core/Enum.hpp"
#include "Render/RenderContext.hpp"
#include "Render/RendererInstance.hpp"
#include "Render/Utils/PushConstants.hpp"
#include "Render/Utils/Samplers.hpp"

namespace ox {
constexpr static auto TRANSIENT_BUFFER_USAGE = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

static constexpr auto TRANSPARENCY_RASTERIZATION = vir::RasterizationState{
  .cull_mode = VK_CULL_MODE_NONE,
  .front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE,
};
static constexpr auto TRANSPARENCY_DEPTH = vir::DepthState{
  .is_test_enabled = true,
  .is_write_enabled = false,
  .compare_op = VK_COMPARE_OP_GREATER_OR_EQUAL,
};
static constexpr auto ACCUMULATION_BLEND = vir::ColorBlendAttachmentState{
  .is_blend_enabled = true,
  .src_color_blend_factor = VK_BLEND_FACTOR_ONE,
  .dst_color_blend_factor = VK_BLEND_FACTOR_ONE,
  .color_blend_op = VK_BLEND_OP_ADD,
  .src_alpha_blend_factor = VK_BLEND_FACTOR_ONE,
  .dst_alpha_blend_factor = VK_BLEND_FACTOR_ONE,
  .alpha_blend_op = VK_BLEND_OP_ADD,
};
static constexpr auto REVEALAGE_BLEND = vir::ColorBlendAttachmentState{
  .is_blend_enabled = true,
  .src_color_blend_factor = VK_BLEND_FACTOR_ZERO,
  .dst_color_blend_factor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
  .color_blend_op = VK_BLEND_OP_ADD,
  .src_alpha_blend_factor = VK_BLEND_FACTOR_ZERO,
  .dst_alpha_blend_factor = VK_BLEND_FACTOR_ONE,
  .alpha_blend_op = VK_BLEND_OP_ADD,
};

struct TransparencyBinding {
  vir::ValueID* value = nullptr;
  vir::Access access = vir::Access::Empty;
};

// the ddgi fields trail the forward push, a frame without ddgi pushes zeros its shaders never read
static constexpr auto DDGI_PUSH_OFFSET = 48_u32;

static auto push_ddgi_apply(vir::Module& module, const FrameRecording& recording, u32 offset) -> void {
  if (recording.shape.has_ddgi) {
    module.push_constants_from_at(offset, recording.prepared.ddgi.apply_push);
    return;
  }

  module.push_constants_at(offset, std::array<u32, 7>{});
}

auto RendererInstance::draw_transparency(
  this RendererInstance& self, vir::Module& module, FrameRecording& recording, TransparencyContext& context
) -> void {
  ZoneScoped;
  auto& prepared = recording.prepared;
  auto& values = recording.values;
  const auto& shape = recording.shape;

  auto& render_context = *self.renderer.render_context;
  const auto render_extent = vir::Extent3D{shape.render_size.x, shape.render_size.y, 1};
  const auto zero = vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 0.0f);
  const auto white = vir::ClearValue::rgba_f32(1.0f, 1.0f, 1.0f, 1.0f);
  const auto linear_clamped = render_context.cached_sampler_handle(LinearSamplerClamped);
  const auto nearest_clamped = render_context.cached_sampler_handle(NearestSamplerClamped);
  const auto scene_flags = std::to_underlying(shape.scene_flags);
  const auto has_directional_shadows = static_cast<u32>(shape.has_directional_shadows && shape.has_meshes);
  // fragments pick the rear or front pair against the refractors' depth, which only exists next to refractors
  const auto is_split = static_cast<u32>(context.refractor_depth_attachment != vir::ValueID::Invalid);

  context.accumulation_attachment = module.transient_image(
    {.extent = render_extent, .format = vir::Format::RGBA16Sfloat, .name = "wboit accumulation"}
  );
  context.accumulation_attachment = module.clear(context.accumulation_attachment, zero);
  context.revealage_attachment = module.transient_image(
    {.extent = render_extent, .format = vir::Format::R8Unorm, .name = "wboit revealage"}
  );
  context.revealage_attachment = module.clear(context.revealage_attachment, white);

  if (is_split) {
    context.accumulation_front_attachment = module.transient_image(
      {.extent = render_extent, .format = vir::Format::RGBA16Sfloat, .name = "wboit accumulation front"}
    );
    context.accumulation_front_attachment = module.clear(context.accumulation_front_attachment, zero);
    context.revealage_front_attachment = module.transient_image(
      {.extent = render_extent, .format = vir::Format::R8Unorm, .name = "wboit revealage front"}
    );
    context.revealage_front_attachment = module.clear(context.revealage_front_attachment, white);
  } else {
    context.refractor_depth_attachment = module.transient_image(
      {.extent = {1, 1, 1}, .format = vir::Format::R32Sfloat, .name = "transparency unused refractor depth"}
    );
  }

  // forward shading pushes its addresses from here, the mesh path puts the cull camera in front of them
  const auto push_base = shape.use_mesh_shaders ? 8_u32 : 0_u32;

  // the targets after the revealage are the front pair, which only exists next to refractors
  auto bindings = ankerl::svector<TransparencyBinding, 40>{};
  auto cull_camera = vir::ValueID::Invalid;
  if (shape.use_mesh_shaders) {
    cull_camera = module.transient_buffer({
      .size = sizeof(GPU::CullCamera),
      .usage = TRANSIENT_BUFFER_USAGE,
      .location = vir::MemoryLocation::GpuOnly,
      .name = "transparency cull camera",
    });
    cull_camera = module.update_buffer_from(cull_camera, prepared.cull.camera);
    static constexpr auto scene_access = vir::Access::TaskRead | vir::Access::MeshRead;
    bindings = {
      {&context.draw_cmd_buffer, vir::Access::IndirectRead},
      {&context.hiz_attachment, vir::Access::TaskSampled},
      {&context.visibility_buffer, vir::Access::TaskRead},
      {&cull_camera, vir::Access::TaskRead},
      {&values.camera, vir::Access::MeshUniformRead | vir::Access::FragmentUniformRead},
      {&values.meshes, scene_access},
      {&values.mesh_instances, scene_access},
      {&values.transparent_meshlet_instances, scene_access},
      {&values.transforms_world, scene_access},
      {&values.skinned_vertices, vir::Access::MeshRead},
      {&values.materials, scene_access | vir::Access::FragmentRead},
    };
  } else {
    bindings = {
      {&context.draw_cmd_buffer, vir::Access::IndirectRead},
      {&values.reordered_indices, vir::Access::IndexRead},
      {&values.camera, vir::Access::VertexUniformRead | vir::Access::FragmentUniformRead},
      {&values.meshes, vir::Access::VertexRead},
      {&values.mesh_instances, vir::Access::VertexRead},
      {&values.transparent_meshlet_instances, vir::Access::VertexRead},
      {&values.transforms_world, vir::Access::VertexRead},
      {&values.skinned_vertices, vir::Access::VertexRead},
      {&values.materials, vir::Access::FragmentRead},
    };
  }

  for (const auto& binding : std::array{
         TransparencyBinding{&context.environment_buffer, vir::Access::FragmentRead},
         TransparencyBinding{&context.lighting_buffer, vir::Access::FragmentRead},
         TransparencyBinding{&values.lights, vir::Access::FragmentRead},
         TransparencyBinding{&context.light_grid_buffer, vir::Access::FragmentRead},
         TransparencyBinding{&context.pointspot_views_buffer, vir::Access::FragmentRead},
         TransparencyBinding{&context.directional_clipmaps_buffer, vir::Access::FragmentRead},
         TransparencyBinding{&context.vsm_page_table_attachment, vir::Access::FragmentSampled},
         TransparencyBinding{&context.pointspot_page_table_attachment, vir::Access::FragmentSampled},
         TransparencyBinding{&context.vsm_physical_pages_attachment, vir::Access::FragmentSampled},
         TransparencyBinding{&context.sky_transmittance_lut_attachment, vir::Access::FragmentSampled},
         TransparencyBinding{&context.sky_aerial_perspective_lut_attachment, vir::Access::FragmentSampled},
         TransparencyBinding{&context.sky_cubemap_attachment, vir::Access::FragmentSampled},
         TransparencyBinding{&context.probe_volumes_buffer, vir::Access::FragmentRead},
         TransparencyBinding{&context.probe_states_buffer, vir::Access::FragmentRead},
         TransparencyBinding{&context.irradiance_attachment, vir::Access::FragmentSampled},
         TransparencyBinding{&context.distance_attachment, vir::Access::FragmentSampled},
         TransparencyBinding{&context.froxel_attachment, vir::Access::FragmentSampled},
         TransparencyBinding{&context.accumulation_attachment, vir::Access::ColorRW},
         TransparencyBinding{&context.revealage_attachment, vir::Access::ColorRW},
       }) {
    bindings.push_back(binding);
  }

  if (is_split) {
    bindings.push_back({&context.accumulation_front_attachment, vir::Access::ColorRW});
    bindings.push_back({&context.revealage_front_attachment, vir::Access::ColorRW});
  }

  bindings.push_back({&context.refractor_depth_attachment, vir::Access::FragmentSampled});
  bindings.push_back({&context.depth_attachment, vir::Access::DepthStencilRead});

  auto accesses = ankerl::svector<vir::ResourceAccess, 40>{};
  for (const auto& binding : bindings) {
    accesses.push_back({*binding.value, binding.access});
  }

  auto& pass = bind_vsm_pointspot_spec_constants(module.begin_rendering(std::span<const vir::ResourceAccess>(accesses)))
                 .with_name(shape.use_mesh_shaders ? "transparency accumulate ms" : "transparency accumulate")
                 .bind_graphics_pipeline(
                   render_context.pipeline(shape.use_mesh_shaders ? "transparent_forward_ms" : "transparent_forward")
                 )
                 .set_rasterization(TRANSPARENCY_RASTERIZATION)
                 .set_depth(TRANSPARENCY_DEPTH)
                 .set_color_blend(0, ACCUMULATION_BLEND)
                 .set_color_blend(1, REVEALAGE_BLEND)
                 .set_viewport(0, vir::Rect2D::framebuffer())
                 .set_scissor(0, vir::Rect2D::framebuffer())
                 .bind_sampler(0, 0, linear_clamped)
                 .bind_sampler(0, 1, nearest_clamped)
                 .bind_buffer(0, 2, values.camera)
                 .bind_buffer(0, 3, values.materials)
                 .bind_image(0, 4, context.vsm_page_table_attachment)
                 .bind_image(0, 5, context.pointspot_page_table_attachment)
                 .bind_image(0, 6, context.vsm_physical_pages_attachment)
                 .bind_image(0, 7, context.sky_transmittance_lut_attachment)
                 .bind_image(0, 8, context.sky_aerial_perspective_lut_attachment)
                 .bind_image(0, 9, context.sky_cubemap_attachment)
                 .bind_buffer(0, 10, context.probe_volumes_buffer)
                 .bind_buffer(0, 11, context.probe_states_buffer)
                 .bind_image(0, 12, context.irradiance_attachment)
                 .bind_image(0, 13, context.distance_attachment)
                 .bind_buffer(0, 14, values.meshes)
                 .bind_buffer(0, 15, values.mesh_instances)
                 .bind_buffer(0, 16, values.transparent_meshlet_instances)
                 .bind_buffer(0, 17, values.transforms_world)
                 .bind_image(0, 20, context.refractor_depth_attachment)
                 .bind_image(0, 23, context.froxel_attachment)
                 .push_constant_address(push_base + 0, context.environment_buffer)
                 .push_constant_address(push_base + 8, context.lighting_buffer)
                 .push_constant_address(push_base + 16, values.lights)
                 .push_constant_address(push_base + 24, context.light_grid_buffer)
                 .push_constant_address(push_base + 32, context.pointspot_views_buffer)
                 .push_constant_address(push_base + 40, context.directional_clipmaps_buffer)
                 .specialize_constant(0, scene_flags)
                 .specialize_constant(1, has_directional_shadows)
                 .specialize_constant(3, is_split);

  if (is_split) {
    pass.set_color_blend(2, ACCUMULATION_BLEND).set_color_blend(3, REVEALAGE_BLEND);
  }

  push_ddgi_apply(module, recording, push_base + DDGI_PUSH_OFFSET);

  if (shape.use_mesh_shaders) {
    pass.bind_image(0, 18, context.hiz_attachment)
      .bind_buffer(0, 19, context.visibility_buffer)
      .push_constant_address(0, cull_camera)
      .specialize_constant(2, std::to_underlying(GPU::CullFlag::Transparent | GPU::CullFlag::TestFrustum))
      .draw_mesh_tasks_indirect(context.draw_cmd_buffer, 1);
  } else {
    pass.bind_index_buffer(values.reordered_indices, VK_INDEX_TYPE_UINT32)
      .draw_indexed_indirect(context.draw_cmd_buffer, 1);
  }

  const auto results = pass.end_rendering_values();
  for (const auto& [binding, result] : std::views::zip(bindings, results)) {
    *binding.value = result;
  }
}

auto RendererInstance::resolve_transparency(
  vir::Module& module, RenderContext& render_context, TransparencyContext& context
) -> void {
  ZoneScoped;

  std::tie(
    context.final_attachment,
    context.reactive_mask_attachment,
    context.accumulation_attachment,
    context.revealage_attachment
  ) = std::tuple_cat( //
    module
      .begin_rendering(
        {{context.final_attachment, vir::Access::ColorRW},
         {context.reactive_mask_attachment, vir::Access::ColorRW},
         {context.accumulation_attachment, vir::Access::FragmentSampled},
         {context.revealage_attachment, vir::Access::FragmentSampled}}
      )
      .with_name("transparency resolve")
      .bind_graphics_pipeline(render_context.pipeline("wboit_resolve"))
      .set_color_blend(0, vir::BlendPreset::PremultipliedAlphaBlend)
      .set_color_blend(1, reactive_mask_blend_state())
      .set_viewport(0, vir::Rect2D::framebuffer())
      .set_scissor(0, vir::Rect2D::framebuffer())
      .bind_image(0, 0, context.accumulation_attachment)
      .bind_image(0, 1, context.revealage_attachment)
      .draw(3, 1)
      .end_rendering()
  );
}

auto RendererInstance::build_transmission_source(
  vir::Module& module, RenderContext& render_context, vir::ValueID final_attachment
) -> std::pair<vir::ValueID, vir::ValueID> {
  ZoneScoped;

  const auto extent = module.image_extent(final_attachment).value_or(vir::Extent3D{1, 1, 1});
  // refractor_shade samples up to mip 7
  const auto level_count = std::min(Texture::calculate_mip_count(extent), 8_u32);

  // always rgba16f, the hdr target's B10G11R11 has no portable storage write and can't be copied into this
  auto transmission = module.transient_image({
    .extent = extent,
    .format = vir::Format::RGBA16Sfloat,
    .mip_levels = level_count,
    .name = "transmission source",
  });

  auto destination_mip = module.mip(transmission, 0);
  std::tie(final_attachment, destination_mip) = std::tuple_cat( //
    module
      .begin_compute({{final_attachment, vir::Access::ComputeSampled}, {destination_mip, vir::Access::ComputeWrite}})
      .with_name("transmission copy")
      .bind_compute_pipeline(render_context.pipeline("transmission_downsample"))
      .specialize_constant(0, true)
      .bind_image(0, 0, destination_mip)
      .bind_image(0, 1, final_attachment)
      .bind_sampler(0, 2, render_context.cached_sampler_handle(LinearSamplerClamped))
      .push_constants(PushConstants(extent.width, extent.height))
      .dispatch_invocations(extent.width, extent.height, 1)
      .end_compute()
  );
  transmission = module.whole(destination_mip);

  for (auto level = 1_u32; level < level_count; level++) {
    const auto mip_width = std::max(1_u32, extent.width >> level);
    const auto mip_height = std::max(1_u32, extent.height >> level);
    auto source_mip = module.mip(transmission, level - 1);
    destination_mip = module.mip(transmission, level);
    std::tie(source_mip, destination_mip) = std::tuple_cat( //
      module.begin_compute({{source_mip, vir::Access::ComputeSampled}, {destination_mip, vir::Access::ComputeWrite}})
        .with_name("transmission downsample")
        .bind_compute_pipeline(render_context.pipeline("transmission_downsample"))
        .specialize_constant(0, false)
        .bind_image(0, 0, destination_mip)
        .bind_image(0, 1, source_mip)
        .bind_sampler(0, 2, render_context.cached_sampler_handle(LinearSamplerClamped))
        .push_constants(PushConstants(mip_width, mip_height))
        .dispatch_invocations(mip_width, mip_height, 1)
        .end_compute()
    );
    transmission = module.whole(destination_mip);
  }

  return {final_attachment, transmission};
}

auto RendererInstance::shade_refractors(
  this RendererInstance& self, vir::Module& module, FrameRecording& recording, TransparencyContext& context
) -> void {
  ZoneScoped;
  auto& values = recording.values;
  const auto& shape = recording.shape;

  auto& render_context = *self.renderer.render_context;
  const auto linear_clamped = render_context.cached_sampler_handle(LinearSamplerClamped);
  const auto nearest_clamped = render_context.cached_sampler_handle(NearestSamplerClamped);
  const auto scene_flags = std::to_underlying(shape.scene_flags);
  const auto has_directional_shadows = static_cast<u32>(shape.has_directional_shadows && shape.has_meshes);

  if (context.refractor_reflection_attachment == vir::ValueID::Invalid) {
    context.refractor_reflection_attachment = module.clear(
      module.transient_image({.extent = {1, 1, 1}, .format = vir::Format::RGBA16Uint, .name = "refractor reflection"}),
      vir::ClearValue::rgba_u32(0, 0, 0, 0)
    );
  }

  auto pass =                          //
    bind_vsm_pointspot_spec_constants( //
      module.begin_rendering(
        {{context.final_attachment, vir::Access::ColorRW},
         {context.reactive_mask_attachment, vir::Access::ColorRW},
         {values.camera, vir::Access::FragmentUniformRead},
         {values.meshes, vir::Access::FragmentRead},
         {values.mesh_instances, vir::Access::FragmentRead},
         {values.transparent_meshlet_instances, vir::Access::FragmentRead},
         {values.transforms_world, vir::Access::FragmentRead},
         {values.materials, vir::Access::FragmentRead},
         {context.environment_buffer, vir::Access::FragmentRead},
         {context.lighting_buffer, vir::Access::FragmentRead},
         {values.lights, vir::Access::FragmentRead},
         {context.light_grid_buffer, vir::Access::FragmentRead},
         {context.pointspot_views_buffer, vir::Access::FragmentRead},
         {context.directional_clipmaps_buffer, vir::Access::FragmentRead},
         {context.vsm_page_table_attachment, vir::Access::FragmentSampled},
         {context.pointspot_page_table_attachment, vir::Access::FragmentSampled},
         {context.vsm_physical_pages_attachment, vir::Access::FragmentSampled},
         {context.sky_transmittance_lut_attachment, vir::Access::FragmentSampled},
         {context.sky_aerial_perspective_lut_attachment, vir::Access::FragmentSampled},
         {context.sky_cubemap_attachment, vir::Access::FragmentSampled},
         {context.probe_volumes_buffer, vir::Access::FragmentRead},
         {context.probe_states_buffer, vir::Access::FragmentRead},
         {context.irradiance_attachment, vir::Access::FragmentSampled},
         {context.distance_attachment, vir::Access::FragmentSampled},
         {context.refractor_visbuffer_attachment, vir::Access::FragmentSampled},
         {context.refractor_depth_attachment, vir::Access::FragmentSampled},
         {context.depth_attachment, vir::Access::FragmentSampled},
         {context.transmission_attachment, vir::Access::FragmentSampled},
         {context.refractor_reflection_attachment, vir::Access::FragmentSampled},
         {context.froxel_attachment, vir::Access::FragmentSampled}}
      )
    )
      .with_name("refractor shade")
      .bind_graphics_pipeline(render_context.pipeline("refractor_shade"))
      .set_color_blend(0, vir::ColorBlendAttachmentState{})
      .set_color_blend(1, reactive_mask_blend_state())
      .set_viewport(0, vir::Rect2D::framebuffer())
      .set_scissor(0, vir::Rect2D::framebuffer())
      .bind_sampler(0, 0, linear_clamped)
      .bind_sampler(0, 1, nearest_clamped)
      .bind_buffer(0, 2, values.camera)
      .bind_buffer(0, 3, values.materials)
      .bind_image(0, 4, context.vsm_page_table_attachment)
      .bind_image(0, 5, context.pointspot_page_table_attachment)
      .bind_image(0, 6, context.vsm_physical_pages_attachment)
      .bind_image(0, 7, context.sky_transmittance_lut_attachment)
      .bind_image(0, 8, context.sky_aerial_perspective_lut_attachment)
      .bind_image(0, 9, context.sky_cubemap_attachment)
      .bind_buffer(0, 10, context.probe_volumes_buffer)
      .bind_buffer(0, 11, context.probe_states_buffer)
      .bind_image(0, 12, context.irradiance_attachment)
      .bind_image(0, 13, context.distance_attachment)
      .bind_buffer(0, 14, values.meshes)
      .bind_buffer(0, 15, values.mesh_instances)
      .bind_buffer(0, 16, values.transparent_meshlet_instances)
      .bind_buffer(0, 17, values.transforms_world)
      .bind_image(0, 18, context.refractor_visbuffer_attachment)
      .bind_image(0, 19, context.depth_attachment)
      .bind_image(0, 20, context.refractor_depth_attachment)
      .bind_image(0, 21, context.transmission_attachment)
      .bind_image(0, 22, context.refractor_reflection_attachment)
      .bind_image(0, 23, context.froxel_attachment)
      .push_constant_address(0, context.environment_buffer)
      .push_constant_address(8, context.lighting_buffer)
      .push_constant_address(16, values.lights)
      .push_constant_address(24, context.light_grid_buffer)
      .push_constant_address(32, context.pointspot_views_buffer)
      .push_constant_address(40, context.directional_clipmaps_buffer)
      .specialize_constant(0, scene_flags)
      .specialize_constant(1, has_directional_shadows);
  push_ddgi_apply(module, recording, DDGI_PUSH_OFFSET);

  std::tie(
    context.final_attachment,
    context.reactive_mask_attachment,
    values.camera,
    values.meshes,
    values.mesh_instances,
    values.transparent_meshlet_instances,
    values.transforms_world,
    values.materials,
    context.environment_buffer,
    context.lighting_buffer,
    values.lights,
    context.light_grid_buffer,
    context.pointspot_views_buffer,
    context.directional_clipmaps_buffer,
    context.vsm_page_table_attachment,
    context.pointspot_page_table_attachment,
    context.vsm_physical_pages_attachment,
    context.sky_transmittance_lut_attachment,
    context.sky_aerial_perspective_lut_attachment,
    context.sky_cubemap_attachment,
    context.probe_volumes_buffer,
    context.probe_states_buffer,
    context.irradiance_attachment,
    context.distance_attachment,
    context.refractor_visbuffer_attachment,
    context.refractor_depth_attachment,
    context.depth_attachment,
    context.transmission_attachment,
    context.refractor_reflection_attachment,
    context.froxel_attachment
  ) = std::tuple_cat(pass.draw(3, 1).end_rendering());
}
} // namespace ox
