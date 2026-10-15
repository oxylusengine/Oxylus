#include "Scene/Terrain.hpp"

#include <cmath>

#include "Core/App.hpp"
#include "Core/Enum.hpp"
#include "Memory/Stack.hpp"
#include "Render/Graph/RenderGraph.hpp"
#include "Render/Graph/Types.hpp"
#include "Render/RenderContext.hpp"
#include "Render/RendererInstance.hpp"
#include "Render/Utils/Samplers.hpp"
#include "Scene/Scene.hpp"

namespace ox {
constexpr static auto TRANSIENT_BUFFER_USAGE = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

static auto brush_radius_texels(const Terrain& terrain) -> f32 {
  const auto texel_size = terrain.texel_world_size();
  return terrain.brush.radius_world / glm::max(texel_size.x, texel_size.y);
}

// texels a stroke can change, which is how far the derived maps have to be rebuilt around it
static auto brush_derive_texels(const Terrain& terrain) -> u32 {
  return 2_u32 * static_cast<u32>(std::ceil(brush_radius_texels(terrain) + 1.0f)) + 1_u32;
}

auto RendererInstance::bind_terrain_variables(
  this const RendererInstance& self, vir::Program& program, const TerrainVariables& variables
) -> void {
  ZoneScoped;

  if (!self.frame_params.shape.has_terrain) {
    return;
  }

  auto& terrain = *self.scene.terrain;
  program.set_bytes(
    variables.data,
    GPU::TerrainData{
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
      .brush_radius = self.frame_params.is_terrain_brush_active ? terrain.brush.radius_world : 0.0f,
    }
  );

  auto cull_camera = self.frame_params.cull_camera;
  cull_camera.mesh_instance_count = terrain.patch_count.x * terrain.patch_count.y;
  program.set_bytes(variables.cull_camera, cull_camera);
  program.set(variables.patch_total, terrain.patch_count.x * terrain.patch_count.y);

  const auto& brush = terrain.brush;
  const auto height_scale = glm::max(terrain.height_scale(), 1e-3f);
  const auto delta_time = glm::clamp(static_cast<f32>(App::get_timestep().get_seconds()), 0.0f, 1.0f / 30.0f);
  const auto is_displacing = brush.mode == GPU::TerrainBrushMode::Raise || brush.mode == GPU::TerrainBrushMode::Noise;
  const auto strength = is_displacing ? brush.height_rate * delta_time / height_scale
                                      : glm::clamp(brush.blend_rate * delta_time, 0.0f, 1.0f);
  program.set_bytes(
    variables.brush_params,
    GPU::TerrainBrushParams{
      .ray_origin = brush.ray_origin,
      .radius_texels = brush_radius_texels(terrain),
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
    }
  );

  auto generate_settings = terrain.generate_settings;
  generate_settings.resolution = terrain.resolution;
  program.set_bytes(variables.brush_generate, generate_settings);

  auto derive_settings = terrain.derive_settings;
  derive_settings.resolution = terrain.resolution;
  derive_settings.texel_world_size = terrain.texel_world_size();
  derive_settings.height_range = terrain.height_range.y - terrain.height_range.x;
  program.set_bytes(variables.brush_derive, derive_settings);

  program.set_bytes(
    variables.brush_minmax,
    GPU::TerrainMinMax{.resolution = terrain.resolution, .patch_count = terrain.patch_count}
  );

  const auto footprint = 2_u32 * static_cast<u32>(std::ceil(brush_radius_texels(terrain))) + 1_u32;
  program.set(variables.brush_apply_groups, (footprint + 15) / 16);
  program.set(variables.brush_derive_groups, (brush_derive_texels(terrain) + 15) / 16);

  const auto patch_texels = glm::vec2(terrain.resolution) / glm::vec2(terrain.patch_count);
  const auto derive_patches = static_cast<u32>(std::ceil(
                                static_cast<f32>(brush_derive_texels(terrain)) /
                                glm::min(patch_texels.x, patch_texels.y)
                              )) +
                              1_u32;
  program.set(variables.brush_minmax_groups, (derive_patches + 7) / 8);

  program.set(variables.is_brush_active, self.frame_params.is_terrain_brush_active);
  program.set(variables.is_brush_painting, self.frame_params.is_terrain_brush_painting);

  program.set(variables.heightmap, terrain.heightmap.attachment_info());
  program.set(variables.ridgemap, terrain.ridgemap.attachment_info());
  program.set(variables.normalmap, terrain.normalmap.attachment_info());
  program.set(variables.splatmap, terrain.splatmap.attachment_info());
  program.set(variables.patch_minmax, terrain.patch_minmax.attachment_info());
  program.set(variables.height_edit, terrain.height_edit.attachment_info());
  program.set(variables.splat_edit, terrain.splat_edit.attachment_info());
}

auto RendererInstance::build_terrain_buffer(this RendererInstance&, vir::Module& module, FrameRecording& recording)
  -> vir::ValueID {
  ZoneScoped;
  auto& prepared = recording.prepared;
  auto& variables = prepared.terrain;
  variables.data = module.declare_bytes_var("terrain", sizeof(GPU::TerrainData));

  auto terrain = module.transient_buffer({
    .size = sizeof(GPU::TerrainData),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "terrain",
  });
  terrain = module.update_buffer_from(terrain, prepared.terrain.data);
  return terrain;
}

auto RendererInstance::apply_terrain_brush(
  this RendererInstance& self, vir::Module& module, FrameRecording& recording, TerrainBrushContext& context
) -> void {
  ZoneScoped;
  auto& prepared = recording.prepared;

  auto& render_context = *self.renderer.render_context;
  auto& variables = prepared.terrain;
  variables.brush_params = module.declare_bytes_var("terrain brush params", sizeof(GPU::TerrainBrushParams));
  variables.brush_generate = module.declare_bytes_var("terrain brush generate", sizeof(GPU::TerrainGenerate));
  variables.brush_derive = module.declare_bytes_var("terrain brush derive", sizeof(GPU::TerrainDerive));
  variables.brush_minmax = module.declare_bytes_var("terrain brush minmax", sizeof(GPU::TerrainMinMax));
  variables.brush_apply_groups = module.declare_u32_var("terrain brush apply groups", 0);
  variables.brush_derive_groups = module.declare_u32_var("terrain brush derive groups", 0);
  variables.brush_minmax_groups = module.declare_u32_var("terrain brush minmax groups", 0);
  variables.is_brush_active = module.declare_bool_var("terrain brush active", false);
  variables.is_brush_painting = module.declare_bool_var("terrain brush painting", false);

  auto& maps = context.maps;
  context.hit_buffer = module.transient_buffer({
    .size = sizeof(GPU::TerrainBrushHit),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "terrain brush hit",
  });
  maps.region = module.transient_buffer({
    .size = sizeof(GPU::TerrainRegion),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "terrain brush region",
  });

  const auto incoming = std::array{
    context.hit_buffer,
    maps.region,
    maps.heightmap,
    maps.ridgemap,
    maps.normalmap,
    maps.splatmap,
    maps.patch_minmax,
    maps.height_edit,
    maps.splat_edit
  };
  std::tie(
    context.hit_buffer,
    maps.region,
    maps.heightmap,
    maps.ridgemap,
    maps.normalmap,
    maps.splatmap,
    maps.patch_minmax,
    maps.height_edit,
    maps.splat_edit
  ) =
    std::tuple_cat(module.set_condition(
      variables.is_brush_active,
      [&](vir::Module& arm) {
        arm
          .begin_compute(
            {{maps.heightmap, vir::Access::ComputeSampled},
             {context.hit_buffer, vir::Access::ComputeWrite},
             {maps.region, vir::Access::ComputeWrite}}
          )
          .with_name("terrain brush trace")
          .bind_compute_pipeline(render_context.pipeline("terrain_brush_trace"))
          .bind_image(0, 0, maps.heightmap)
          .bind_sampler(0, 1, render_context.cached_sampler_handle(vir::LinearSamplerClamped))
          .bind_buffer(0, 2, context.hit_buffer)
          .bind_buffer(0, 3, maps.region)
          .push_constants_from(variables.brush_params)
          .dispatch(1, 1, 1);
        std::tie(maps.heightmap, context.hit_buffer, maps.region) = std::tuple_cat(arm.end_compute<3>());

        const auto traced = std::array{
          context.hit_buffer,
          maps.region,
          maps.heightmap,
          maps.ridgemap,
          maps.normalmap,
          maps.splatmap,
          maps.patch_minmax,
          maps.height_edit,
          maps.splat_edit
        };
        return arm.set_condition(
          variables.is_brush_painting,
          [&](vir::Module& paint) {
            paint
              .begin_compute(
                {{maps.heightmap, vir::Access::ComputeSampled},
                 {maps.height_edit, vir::Access::ComputeRW},
                 {maps.splat_edit, vir::Access::ComputeRW},
                 {context.hit_buffer, vir::Access::ComputeRead}}
              )
              .with_name("terrain brush apply")
              .bind_compute_pipeline(render_context.pipeline("terrain_brush_apply"))
              .bind_image(0, 0, maps.heightmap)
              .bind_image(0, 1, maps.height_edit)
              .bind_image(0, 2, maps.splat_edit)
              .bind_buffer(0, 3, context.hit_buffer)
              .push_constants_from(variables.brush_params)
              .dispatch(variables.brush_apply_groups, variables.brush_apply_groups, 1);
            std::tie(maps.heightmap, maps.height_edit, maps.splat_edit, context.hit_buffer) = std::tuple_cat(
              paint.end_compute<4>()
            );

            record_terrain_generate(
              paint,
              render_context,
              maps,
              variables.brush_generate,
              variables.brush_derive_groups,
              variables.brush_derive_groups
            );
            record_terrain_derive(
              paint,
              render_context,
              maps,
              variables.brush_derive,
              variables.brush_derive_groups,
              variables.brush_derive_groups
            );
            record_terrain_minmax(
              paint,
              render_context,
              maps,
              variables.brush_minmax,
              variables.brush_minmax_groups,
              variables.brush_minmax_groups
            );
            return std::array{
              context.hit_buffer,
              maps.region,
              maps.heightmap,
              maps.ridgemap,
              maps.normalmap,
              maps.splatmap,
              maps.patch_minmax,
              maps.height_edit,
              maps.splat_edit
            };
          },
          [&](vir::Module&) { return traced; }
        );
      },
      [&](vir::Module& arm) {
        auto skipped = incoming;
        skipped[0] = arm.fill_buffer(incoming[0], 0);
        return skipped;
      }
    ));
}

auto RendererInstance::cull_terrain(
  vir::Module& module, RenderContext& render_context, FrameRecording& recording, TerrainContext& context
) -> void {
  ZoneScoped;
  auto& prepared = recording.prepared;
  memory::ScopedStack stack;

  auto& variables = prepared.terrain;
  if (variables.cull_camera == vir::ValueID::Invalid) {
    variables.cull_camera = module.declare_bytes_var("terrain cull camera", sizeof(GPU::CullCamera));
  }
  if (variables.patch_total == vir::ValueID::Invalid) {
    variables.patch_total = module.declare_u32_var("terrain cull patch total", 0);
  }

  const auto is_late = static_cast<bool>(context.cull_flags & GPU::CullFlag::LatePass);
  const auto name = stack.format("terrain cull {}", is_late ? "late" : "early");

  const auto
    draw_command = VkDrawIndirectCommand{.vertexCount = 4, .instanceCount = 0, .firstVertex = 0, .firstInstance = 0};
  context.draw_cmd_buffer = module.transient_buffer({
    .size = sizeof(draw_command),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = std::string(stack.format("terrain draw command {}", is_late ? "late" : "early")),
  });
  context.draw_cmd_buffer = module.update_buffer(
    context.draw_cmd_buffer,
    std::span{reinterpret_cast<const u8*>(&draw_command), sizeof(draw_command)}
  );

  module
    .begin_compute(
      {{context.terrain_buffer, vir::Access::ComputeUniformRead},
       {context.patch_minmax_attachment, vir::Access::ComputeSampled},
       {context.hiz_attachment, vir::Access::ComputeSampled},
       {context.visible_patches_buffer, vir::Access::ComputeRW},
       {context.patch_visibility_mask_buffer, vir::Access::ComputeRW},
       {context.draw_cmd_buffer, vir::Access::ComputeRW}}
    )
    .with_name(name)
    .bind_compute_pipeline(render_context.pipeline("terrain_cull"))
    .bind_buffer(0, 0, context.terrain_buffer)
    .bind_image(0, 1, context.patch_minmax_attachment)
    .bind_image(0, 2, context.hiz_attachment)
    .bind_buffer(0, 3, context.visible_patches_buffer)
    .bind_buffer(0, 4, context.patch_visibility_mask_buffer)
    .bind_buffer(0, 5, context.draw_cmd_buffer)
    .specialize_constant(0, std::to_underlying(context.cull_flags))
    .push_constants_from(prepared.terrain.cull_camera)
    .dispatch_invocations(prepared.terrain.patch_total, 1, 1);
  std::tie(
    context.terrain_buffer,
    context.patch_minmax_attachment,
    context.hiz_attachment,
    context.visible_patches_buffer,
    context.patch_visibility_mask_buffer,
    context.draw_cmd_buffer
  ) = std::tuple_cat(module.end_compute<6>());
}

auto RendererInstance::draw_terrain_for_visbuffer(
  this RendererInstance& self, vir::Module& module, FrameRecording& recording, TerrainContext& context
) -> void {
  ZoneScoped;
  auto& values = recording.values;
  memory::ScopedStack stack;

  auto& render_context = *self.renderer.render_context;
  const auto is_late = static_cast<bool>(context.cull_flags & GPU::CullFlag::LatePass);
  static constexpr auto uniform_access = vir::Access::VertexUniformRead | vir::Access::TessellationControlUniformRead |
                                         vir::Access::TessellationEvaluationUniformRead;
  static constexpr auto patch_access = vir::Access::VertexRead | vir::Access::TessellationControlRead |
                                       vir::Access::TessellationEvaluationRead;
  static constexpr auto sampled_access = vir::Access::VertexSampled | vir::Access::TessellationControlSampled |
                                         vir::Access::TessellationEvaluationSampled;

  module
    .begin_rendering(
      {{context.draw_cmd_buffer, vir::Access::IndirectRead},
       {values.camera, uniform_access},
       {context.terrain_buffer, uniform_access},
       {context.visible_patches_buffer, patch_access},
       {context.heightmap_attachment, sampled_access},
       {context.visbuffer_attachment, vir::Access::ColorRW},
       {context.depth_attachment, vir::Access::DepthStencilRW}}
    )
    .with_name(stack.format("terrain vis encode {}", is_late ? "late" : "early"))
    .bind_graphics_pipeline(render_context.pipeline("terrain_visbuffer"))
    .set_rasterization({.cull_mode = VK_CULL_MODE_BACK_BIT})
    .set_depth({.is_test_enabled = true, .is_write_enabled = true, .compare_op = VK_COMPARE_OP_GREATER_OR_EQUAL})
    .set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_PATCH_LIST)
    .set_patch_control_points(4)
    .set_viewport(0, vir::Rect2D::framebuffer())
    .set_scissor(0, vir::Rect2D::framebuffer())
    .bind_buffer(0, 0, values.camera)
    .bind_buffer(0, 1, context.terrain_buffer)
    .bind_buffer(0, 2, context.visible_patches_buffer)
    .bind_image(0, 3, context.heightmap_attachment)
    .bind_sampler(0, 4, render_context.cached_sampler_handle(vir::LinearSamplerClamped))
    .draw_indirect(context.draw_cmd_buffer, 1);
  std::tie(
    std::ignore,
    values.camera,
    context.terrain_buffer,
    context.visible_patches_buffer,
    context.heightmap_attachment,
    context.visbuffer_attachment,
    context.depth_attachment
  ) = std::tuple_cat(module.end_rendering<7>());
}

auto RendererInstance::decode_terrain(
  this RendererInstance& self, vir::Module& module, FrameRecording& recording, TerrainDecodeContext& context
) -> void {
  ZoneScoped;
  auto& values = recording.values;

  auto& render_context = *self.renderer.render_context;
  module
    .begin_rendering(
      {{values.camera, vir::Access::FragmentUniformRead},
       {values.materials, vir::Access::FragmentRead},
       {context.terrain_buffer, vir::Access::FragmentUniformRead},
       {context.brush_hit_buffer, vir::Access::FragmentRead},
       {context.visbuffer_attachment, vir::Access::FragmentSampled},
       {context.depth_attachment, vir::Access::FragmentSampled},
       {context.normalmap_attachment, vir::Access::FragmentSampled},
       {context.splatmap_attachment, vir::Access::FragmentSampled},
       {context.albedo_attachment, vir::Access::ColorRW},
       {context.normal_attachment, vir::Access::ColorRW},
       {context.emissive_attachment, vir::Access::ColorRW},
       {context.metallic_roughness_occlusion_attachment, vir::Access::ColorRW},
       {context.velocity_attachment, vir::Access::ColorRW}}
    )
    .with_name("terrain decode")
    .bind_graphics_pipeline(render_context.pipeline("terrain_decode"))
    .set_viewport(0, vir::Rect2D::framebuffer())
    .set_scissor(0, vir::Rect2D::framebuffer())
    .bind_buffer(0, 0, values.camera)
    .bind_buffer(0, 1, values.materials)
    .bind_buffer(0, 2, context.terrain_buffer)
    .bind_image(0, 3, context.visbuffer_attachment)
    .bind_image(0, 4, context.depth_attachment)
    .bind_image(0, 5, context.normalmap_attachment)
    .bind_image(0, 6, context.splatmap_attachment)
    .bind_sampler(0, 7, render_context.cached_sampler_handle(vir::LinearSamplerClamped))
    .bind_buffer(0, 8, context.brush_hit_buffer)
    .draw_range(3, 1, 0, 1);
  std::tie(
    values.camera,
    values.materials,
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
  ) = std::tuple_cat(module.end_rendering<13>());
}
} // namespace ox
