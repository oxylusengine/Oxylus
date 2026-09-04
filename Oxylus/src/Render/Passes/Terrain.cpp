#include "Scene/Terrain.hpp"

#include <cmath>
#include <vuk/runtime/CommandBuffer.hpp>

#include "Core/App.hpp"
#include "Core/Enum.hpp"
#include "Memory/Stack.hpp"
#include "Render/RendererInstance.hpp"
#include "Render/Utils/VukCommon.hpp"

namespace ox {
auto RendererInstance::build_terrain_buffer(this RendererInstance& self, const Terrain& terrain)
  -> vuk::Value<vuk::Buffer> {
  ZoneScoped;

  auto data = GPU::TerrainData{
    .world_min = terrain.world_min(),
    .world_size = terrain.world_size,
    .inv_world_size = 1.0f / terrain.world_size,
    .patch_count = terrain.patch_count,
    .base_height = terrain.base_height(),
    .height_scale = terrain.height_scale(),
    .target_edge_pixels = terrain.target_edge_pixels,
    .max_tessellation = terrain.max_tessellation,
    .layer_tiling = terrain.layer_tiling,
    .triplanar_begin = terrain.triplanar_begin,
    .layer_material_indices = terrain.layer_material_indices,
    .brush_radius = terrain.brush.active ? terrain.brush.radius_world : 0.0f,
  };

  return self.renderer.render_context->scratch_buffer(data);
}

auto RendererInstance::apply_terrain_brush(this RendererInstance& self, TerrainBrushContext& context) -> void {
  ZoneScoped;

  const auto& terrain = *context.terrain;
  const auto& brush = terrain.brush;
  auto& render_context = *self.renderer.render_context;

  const auto texel_size = terrain.texel_world_size();
  const auto radius_texels = brush.radius_world / glm::max(texel_size.x, texel_size.y);

  const auto height_scale = glm::max(terrain.height_scale(), 1e-3f);
  const auto delta_time = glm::clamp(static_cast<f32>(App::get_timestep().get_seconds()), 0.0f, 1.0f / 30.0f);
  const auto displaces = brush.mode == GPU::TerrainBrushMode::Raise || brush.mode == GPU::TerrainBrushMode::Noise;
  const auto strength = displaces ? brush.height_rate * delta_time / height_scale
                                  : glm::clamp(brush.blend_rate * delta_time, 0.0f, 1.0f);

  auto params = GPU::TerrainBrushParams{
    .ray_origin = brush.ray_origin,
    .radius_texels = radius_texels,
    .ray_direction = brush.ray_direction,
    .strength = brush.invert ? -strength : strength,
    .resolution = terrain.resolution,
    .world_min = terrain.world_min(),
    .world_size = terrain.world_size,
    .inv_world_size = 1.0f / terrain.world_size,
    .patch_count = terrain.patch_count,
    .base_height = terrain.base_height(),
    .height_scale = terrain.height_scale(),
    .falloff = brush.falloff,
    .flatten_height = glm::clamp((brush.flatten_height_world - terrain.base_height()) / height_scale, 0.0f, 1.0f),
    .mode = std::to_underlying(brush.mode),
    .layer = brush.layer,
  };

  context.hit_buffer = render_context.alloc_transient_buffer(vuk::MemoryUsage::eGPUonly, sizeof(GPU::TerrainBrushHit));
  context.maps.region = render_context.alloc_transient_buffer(vuk::MemoryUsage::eGPUonly, sizeof(GPU::TerrainRegion));

  auto trace_pass = vuk::make_pass(
    "terrain brush trace",
    [params](
      vuk::CommandBuffer& cmd_list, //
      VUK_IA(vuk::eComputeSampled) heightmap,
      VUK_BA(vuk::eComputeWrite) hit,
      VUK_BA(vuk::eComputeWrite) region
    ) {
      cmd_list //
        .bind_compute_pipeline("terrain_brush_trace")
        .bind_image(0, 0, heightmap)
        .bind_sampler(0, 1, vuk::LinearSamplerClamped)
        .bind_buffer(0, 2, hit)
        .bind_buffer(0, 3, region)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, params)
        .dispatch(1, 1, 1);

      return std::make_tuple(heightmap, hit, region);
    }
  );

  std::tie(context.maps.heightmap, context.hit_buffer, context.maps.region) = trace_pass(
    std::move(context.maps.heightmap),
    std::move(context.hit_buffer),
    std::move(context.maps.region)
  );

  if (!brush.painting) {
    return;
  }

  auto apply_pass = vuk::make_pass(
    "terrain brush apply",
    [params, radius_texels](
      vuk::CommandBuffer& cmd_list, //
      VUK_IA(vuk::eComputeSampled) heightmap,
      VUK_IA(vuk::eComputeRW) height_edit,
      VUK_IA(vuk::eComputeRW) splat_edit,
      VUK_BA(vuk::eComputeRead) hit
    ) {
      const auto footprint = 2_u32 * static_cast<u32>(std::ceil(radius_texels)) + 1_u32;

      cmd_list //
        .bind_compute_pipeline("terrain_brush_apply")
        .bind_image(0, 0, heightmap)
        .bind_image(0, 1, height_edit)
        .bind_image(0, 2, splat_edit)
        .bind_buffer(0, 3, hit)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, params)
        .dispatch((footprint + 15) / 16, (footprint + 15) / 16, 1);

      return std::make_tuple(heightmap, height_edit, splat_edit, hit);
    }
  );

  std::tie(context.maps.heightmap, context.maps.height_edit, context.maps.splat_edit, context.hit_buffer) = apply_pass(
    std::move(context.maps.heightmap),
    std::move(context.maps.height_edit),
    std::move(context.maps.splat_edit),
    std::move(context.hit_buffer)
  );

  auto generate_settings = terrain.generate_settings;
  generate_settings.resolution = terrain.resolution;

  auto derive_settings = terrain.derive_settings;
  derive_settings.resolution = terrain.resolution;
  derive_settings.texel_world_size = texel_size;
  derive_settings.height_range = terrain.height_range.y - terrain.height_range.x;

  const auto minmax_settings = GPU::TerrainMinMax{
    .resolution = terrain.resolution,
    .patch_count = terrain.patch_count,
  };

  const auto derive_texels = 2_u32 * static_cast<u32>(std::ceil(radius_texels + 1.0f)) + 1_u32;
  terrain_generate_pass(context.maps, generate_settings, glm::uvec2(derive_texels));
  terrain_derive_pass(context.maps, derive_settings, glm::uvec2(derive_texels));

  const auto patch_texels = glm::vec2(terrain.resolution) / glm::vec2(terrain.patch_count);
  const auto derive_patches = static_cast<u32>(
                                std::ceil(static_cast<f32>(derive_texels) / glm::min(patch_texels.x, patch_texels.y))
                              ) +
                              1_u32;
  terrain_minmax_pass(context.maps, minmax_settings, glm::uvec2(derive_patches));
}

auto RendererInstance::cull_terrain(this RendererInstance& self, TerrainContext& context) -> void {
  ZoneScoped;

  memory::ScopedStack stack;

  const auto late_pass = context.cull_flags & GPU::CullFlag::LatePass;
  const auto patch_total = context.terrain->patch_count.x * context.terrain->patch_count.y;

  context.draw_cmd_buffer = self.renderer.render_context->scratch_buffer<vuk::DrawIndirectCommand>(
    {.vertexCount = 4, .instanceCount = 0, .firstVertex = 0, .firstInstance = 0}
  );

  context.cull_camera.mesh_instance_count = patch_total;

  auto cull_pass = vuk::make_pass(
    stack.format("terrain cull {}", late_pass ? "late" : "early"),
    [cull_camera = context.cull_camera, cull_flags = context.cull_flags](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eComputeRead) terrain,
      VUK_IA(vuk::eComputeSampled) patch_minmax,
      VUK_IA(vuk::eComputeSampled) hiz,
      VUK_BA(vuk::eComputeRW) visible_patches,
      VUK_BA(vuk::eComputeRW) visibility_mask,
      VUK_BA(vuk::eComputeRW) draw_cmd
    ) {
      cmd_list //
        .bind_compute_pipeline("terrain_cull")
        .bind_buffer(0, 0, terrain)
        .bind_image(0, 1, patch_minmax)
        .bind_image(0, 2, hiz)
        .bind_buffer(0, 3, visible_patches)
        .bind_buffer(0, 4, visibility_mask)
        .bind_buffer(0, 5, draw_cmd)
        .specialize_constants(0, std::to_underlying(cull_flags))
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, cull_camera)
        .dispatch_invocations(cull_camera.mesh_instance_count);

      return std::make_tuple(terrain, patch_minmax, hiz, visible_patches, visibility_mask, draw_cmd);
    }
  );

  std::tie(
    context.terrain_buffer,
    context.patch_minmax_attachment,
    context.hiz_attachment,
    context.visible_patches_buffer,
    context.patch_visibility_mask_buffer,
    context.draw_cmd_buffer
  ) =
    cull_pass(
      std::move(context.terrain_buffer),
      std::move(context.patch_minmax_attachment),
      std::move(context.hiz_attachment),
      std::move(context.visible_patches_buffer),
      std::move(context.patch_visibility_mask_buffer),
      std::move(context.draw_cmd_buffer)
    );
}

auto RendererInstance::draw_terrain_for_visbuffer(this RendererInstance& self, TerrainContext& context) -> void {
  ZoneScoped;

  memory::ScopedStack stack;

  const auto late_pass = context.cull_flags & GPU::CullFlag::LatePass;

  auto draw_pass = vuk::make_pass(
    stack.format("terrain vis encode {}", late_pass ? "late" : "early"),
    [](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eIndirectRead) draw_cmd,
      VUK_BA(vuk::eVertexRead) camera,
      VUK_BA(vuk::eVertexRead) terrain,
      VUK_BA(vuk::eVertexRead) visible_patches,
      VUK_IA(vuk::eVertexSampled | vuk::eTessellationSampled) heightmap,
      VUK_IA(vuk::eColorRW) visbuffer,
      VUK_IA(vuk::eDepthStencilRW) depth
    ) {
      cmd_list //
        .bind_graphics_pipeline("terrain_visbuffer")
        .set_rasterization({.cullMode = vuk::CullModeFlagBits::eBack})
        .set_depth_stencil(
          {.depthTestEnable = true, .depthWriteEnable = true, .depthCompareOp = vuk::CompareOp::eGreaterOrEqual}
        )
        .set_color_blend(visbuffer, vuk::BlendPreset::eOff)
        .set_primitive_topology(vuk::PrimitiveTopology::ePatchList)
        .set_patch_control_points(4)
        .set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
        .set_viewport(0, vuk::Rect2D::framebuffer())
        .set_scissor(0, vuk::Rect2D::framebuffer())
        .bind_buffer(0, 0, camera)
        .bind_buffer(0, 1, terrain)
        .bind_buffer(0, 2, visible_patches)
        .bind_image(0, 3, heightmap)
        .bind_sampler(0, 4, vuk::LinearSamplerClamped)
        .draw_indirect(1, draw_cmd);

      return std::make_tuple(camera, terrain, visible_patches, heightmap, visbuffer, depth);
    }
  );

  std::tie(
    self.prepared_frame.camera_buffer,
    context.terrain_buffer,
    context.visible_patches_buffer,
    context.heightmap_attachment,
    context.visbuffer_attachment,
    context.depth_attachment
  ) =
    draw_pass(
      std::move(context.draw_cmd_buffer),
      std::move(self.prepared_frame.camera_buffer),
      std::move(context.terrain_buffer),
      std::move(context.visible_patches_buffer),
      std::move(context.heightmap_attachment),
      std::move(context.visbuffer_attachment),
      std::move(context.depth_attachment)
    );
}

auto RendererInstance::decode_terrain(this RendererInstance& self, TerrainDecodeContext& context) -> void {
  ZoneScoped;

  auto decode_pass = vuk::make_pass(
    "terrain decode",
    [&descriptor_set = *context.bindless_set](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eFragmentRead) camera,
      VUK_BA(vuk::eFragmentRead) materials,
      VUK_BA(vuk::eFragmentRead) terrain,
      VUK_BA(vuk::eFragmentRead) brush_hit,
      VUK_IA(vuk::eFragmentSampled) visbuffer,
      VUK_IA(vuk::eFragmentSampled) depth,
      VUK_IA(vuk::eFragmentSampled) normalmap,
      VUK_IA(vuk::eFragmentSampled) splatmap,
      VUK_IA(vuk::eColorRW) albedo,
      VUK_IA(vuk::eColorRW) normal,
      VUK_IA(vuk::eColorRW) emissive,
      VUK_IA(vuk::eColorRW) metallic_roughness_occlusion,
      VUK_IA(vuk::eColorRW) velocity
    ) {
      cmd_list //
        .bind_graphics_pipeline("terrain_decode")
        .set_rasterization({.cullMode = vuk::CullModeFlagBits::eNone})
        .set_depth_stencil({})
        .set_color_blend(albedo, vuk::BlendPreset::eOff)
        .set_color_blend(normal, vuk::BlendPreset::eOff)
        .set_color_blend(emissive, vuk::BlendPreset::eOff)
        .set_color_blend(metallic_roughness_occlusion, vuk::BlendPreset::eOff)
        .set_color_blend(velocity, vuk::BlendPreset::eOff)
        .set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
        .set_viewport(0, vuk::Rect2D::framebuffer())
        .set_scissor(0, vuk::Rect2D::framebuffer())
        .bind_persistent(1, descriptor_set)
        .bind_buffer(0, 0, camera)
        .bind_buffer(0, 1, materials)
        .bind_buffer(0, 2, terrain)
        .bind_image(0, 3, visbuffer)
        .bind_image(0, 4, depth)
        .bind_image(0, 5, normalmap)
        .bind_image(0, 6, splatmap)
        .bind_sampler(0, 7, vuk::LinearSamplerClamped)
        .bind_buffer(0, 8, brush_hit)
        .draw(3, 1, 0, 1);

      return std::make_tuple(
        camera,
        materials,
        terrain,
        brush_hit,
        visbuffer,
        depth,
        normalmap,
        splatmap,
        albedo,
        normal,
        emissive,
        metallic_roughness_occlusion,
        velocity
      );
    }
  );

  std::tie(
    self.prepared_frame.camera_buffer,
    self.prepared_frame.materials_buffer,
    context.terrain_buffer,
    context.brush_hit_buffer,
    context.visbuffer_attachment,
    context.depth_attachment,
    context.normalmap_attachment,
    context.splatmap_attachment,
    context.albedo_attachment,
    context.normal_attachment,
    context.emissive_attachment,
    context.metallic_roughness_occlusion_attachment,
    context.velocity_attachment
  ) =
    decode_pass(
      std::move(self.prepared_frame.camera_buffer),
      std::move(self.prepared_frame.materials_buffer),
      std::move(context.terrain_buffer),
      std::move(context.brush_hit_buffer),
      std::move(context.visbuffer_attachment),
      std::move(context.depth_attachment),
      std::move(context.normalmap_attachment),
      std::move(context.splatmap_attachment),
      std::move(context.albedo_attachment),
      std::move(context.normal_attachment),
      std::move(context.emissive_attachment),
      std::move(context.metallic_roughness_occlusion_attachment),
      std::move(context.velocity_attachment)
    );
}
} // namespace ox
