#include <vir/RenderGraph.hpp>
#include <vir/Types.hpp>

#include "Asset/Texture.hpp"
#include "Core/Enum.hpp"
#include "Memory/Stack.hpp"
#include "Render/RenderContext.hpp"
#include "Render/RendererInstance.hpp"
#include "Render/Utils/PushConstants.hpp"
#include "Render/Utils/Samplers.hpp"

namespace ox {
using CullMeshletsPush = PushConstants<GPU::CullCamera, u32>;

constexpr static auto TRANSIENT_BUFFER_USAGE = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

auto RendererInstance::bind_cull_variables(
  this const RendererInstance& self, vir::Program& program, const CullVariables& variables
) -> void {
  ZoneScoped;

  const auto& data = self.frame_data;
  program.set(variables.mesh_instance_count, data.mesh_instance_count);
  program.set_bytes(variables.camera, self.frame_params.cull_camera);

  if (program.has_variable(variables.shadow_camera) || program.has_variable(variables.clipmap_push)) {
    constexpr auto clipmap_index = RMVSMContext::MAX_DIRECTIONAL_CLIPMAP_COUNT - 1;
    const auto& clipmap = self.frame_params.vsm_directional_clipmaps[clipmap_index];
    const auto shadow_camera = GPU::CullCamera{
      .projection_view = clipmap.projection_view_mat,
      .position = -self.directional_light.direction,
      .acceptable_lod_error = self.camera_data.acceptable_lod_error,
      .resolution = self.camera_data.resolution,
      .near_clip = clipmap.z_near,
      .mesh_instance_count = data.mesh_instance_count,
    };
    program.set_bytes(variables.shadow_camera, shadow_camera);
    program.set_bytes(
      variables.clipmap_push,
      CullMeshletsPush(shadow_camera, RMVSMContext::MAX_DIRECTIONAL_CLIPMAP_COUNT)
    );
  }
  for (auto mip = 0_u32; mip < RMVSMContext::POINT_SPOT_MIP_COUNT; mip++) {
    if (
      !program.has_variable(variables.pointspot_contexts[mip]) &&
      !program.has_variable(variables.pointspot_cull_meshes_commands[mip])
    ) {
      continue;
    }
    auto context = self.frame_params.vsm_pointspot;
    context.curr_mip = static_cast<i32>(mip);
    program.set_bytes(variables.pointspot_contexts[mip], context);
    program.set_bytes(
      variables.pointspot_cull_meshes_commands[mip],
      VkDispatchIndirectCommand{
        .x = (context.mesh_instance_count + RMVSMContext::CULLING_MESH_COUNT - 1) / RMVSMContext::CULLING_MESH_COUNT,
        .y = 0,
        .z = 1,
      }
    );
  }
}

auto RendererInstance::generate_hiz(vir::Module& module, RenderContext& render_context, MainGeometryContext& context)
  -> void {
  ZoneScoped;

  const auto extent = *module.image_extent(context.hiz_attachment);
  const auto level_count = std::min(Texture::calculate_mip_count(extent), 13_u32);
  const auto counter_value = 0_u32;
  auto counter = module.transient_buffer({
    .size = sizeof(counter_value),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "hiz atomic",
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
        {context.depth_attachment, vir::Access::ComputeSampled},
        {context.hiz_attachment, vir::Access::ComputeRW},
      })
      .with_name("hiz generate")
      .bind_compute_pipeline(render_context.pipeline("hiz"))
      .specialize_constant(0, 1_u32)
      .specialize_constant(1, extent.width)
      .specialize_constant(2, extent.height)
      .bind_buffer(0, 0, counter)
      .bind_sampler(0, 1, render_context.cached_sampler_handle(NearestSamplerClamped))
      .bind_image(0, 2, context.depth_attachment);
  for (auto mip = 0_u32; mip < 13; mip++) {
    module.bind_image(0, 3 + mip, module.mip(context.hiz_attachment, std::min(mip, level_count - 1)));
  }
  module.push_constants(PushConstants(level_count, groups_x * groups_y, glm::mat2(1.0f)))
    .dispatch(groups_x, groups_y, 1);
  const auto result = pass.end_compute();
  context.depth_attachment = result[1];
  context.hiz_attachment = result[2];
}

auto RendererInstance::skin_vertices(this RendererInstance& self, vir::Module& module, FrameRecording& recording)
  -> void {
  ZoneScoped;
  auto& values = recording.values;

  auto& render_context = *self.renderer.render_context;
  // jobs and the arena change per frame, so the dispatches go through a callback that reads them.
  // Positions and normals share one arena, and the shader writes them through two bound subranges
  // so it never has to write through a device address. Where they split moves with the vertex
  // total, which is why the callback binds them rather than the program
  const auto arena = values.skinned_vertices;
  const auto body = module.declare_callback_var("skin vertices", [&self, arena](vir::Recorder& recorder) -> void {
    const auto& data = self.frame_data;
    const auto positions_bytes = static_cast<u64>(data.skinned_vertex_total) * sizeof(glm::u16vec4);
    const auto normals_bytes = static_cast<u64>(data.skinned_vertex_total) * sizeof(u32);
    if (positions_bytes == 0) {
      return;
    }

    const auto vertices = recorder.buffer(arena);
    recorder.bind_buffer(0, 3, vertices, 0, positions_bytes)
      .bind_buffer(0, 4, vertices, positions_bytes, normals_bytes);
    for (const auto& job : data.skin_jobs) {
      recorder.push_constants(job).dispatch((job.vertex_count + 63) / 64, 1, 1);
    }
  });

  std::tie(values.meshes, values.mesh_instances, values.skinning_transforms, values.skinned_vertices) = std::tuple_cat(
    module
      .begin_compute(
        {{values.meshes, vir::Access::ComputeRead},
         // declared as written even though the shader only reads it: every consumer reaches the
         // skinned vertices through the pointers in here and names only this buffer, so this edge is
         // what orders them after the skinning
         {values.mesh_instances, vir::Access::ComputeRW},
         {values.skinning_transforms, vir::Access::ComputeRead},
         {values.skinned_vertices, vir::Access::ComputeWrite}}
      )
      .with_name("skin vertices")
      .bind_compute_pipeline(render_context.pipeline("skin_vertices"))
      .bind_buffer(0, 0, values.meshes)
      .bind_buffer(0, 1, values.mesh_instances)
      .bind_buffer(0, 2, values.skinning_transforms)
      .record_from(body)
      .end_compute()
  );
}

auto RendererInstance::cull_geometry(
  vir::Module& module, RenderContext& render_context, FrameRecording& recording, CullGeometryContext& context
) -> void {
  ZoneScoped;
  auto& prepared = recording.prepared;
  auto& values = recording.values;
  memory::ScopedStack stack;

  const auto cull_flags = context.cull_flags;
  const auto is_late = static_cast<bool>(cull_flags & GPU::CullFlag::LatePass);
  auto& variables = prepared.cull;
  if (variables.mesh_instance_count == vir::ValueID::Invalid) {
    variables.mesh_instance_count = module.declare_u32_var("cull mesh instance count", 0);
  }

  if (context.use_hpb) {
    if (variables.shadow_camera == vir::ValueID::Invalid) {
      variables.shadow_camera = module.declare_bytes_var("vsm cull camera", sizeof(GPU::CullCamera));
    }

    if (variables.clipmap_push == vir::ValueID::Invalid) {
      variables.clipmap_push = module.declare_bytes_var("rmvsm cull meshlets push", sizeof(CullMeshletsPush));
    }

  } else if (variables.camera == vir::ValueID::Invalid) {
    variables.camera = module.declare_bytes_var("cull camera", sizeof(GPU::CullCamera));
  }

  const auto cull_camera = context.use_hpb ? prepared.cull.shadow_camera : prepared.cull.camera;

  // --- Stage 1: cull_meshes (only on the first cull of a sequence) ---
  if (context.init_cull_meshes) {
    const auto visibility = GPU::MeshletInstanceVisibility{};
    context.visibility_buffer = module.transient_buffer({
      .size = sizeof(visibility),
      .usage = TRANSIENT_BUFFER_USAGE,
      .location = vir::MemoryLocation::GpuOnly,
      .name = "meshlet visibility",
    });
    context.visibility_buffer = module.update_buffer(
      context.visibility_buffer,
      std::span{reinterpret_cast<const u8*>(&visibility), sizeof(visibility)}
    );
    const auto cull_meshlets_command = VkDispatchIndirectCommand{.x = 0, .y = 1, .z = 1};
    context.cull_meshlets_cmd_buffer = module.transient_buffer({
      .size = sizeof(cull_meshlets_command),
      .usage = TRANSIENT_BUFFER_USAGE,
      .location = vir::MemoryLocation::GpuOnly,
      .name = "cull meshlets command",
    });
    context.cull_meshlets_cmd_buffer = module.update_buffer(
      context.cull_meshlets_cmd_buffer,
      std::span{reinterpret_cast<const u8*>(&cull_meshlets_command), sizeof(cull_meshlets_command)}
    );

    std::tie(
      values.meshes,
      values.transforms_world,
      values.mesh_instances,
      values.meshlet_instances,
      context.visibility_buffer,
      context.cull_meshlets_cmd_buffer
    ) = std::tuple_cat( //
      module
        .begin_compute(
          {{values.meshes, vir::Access::ComputeRead},
           {values.transforms_world, vir::Access::ComputeRead},
           {values.mesh_instances, vir::Access::ComputeRW},
           {values.meshlet_instances, vir::Access::ComputeRW},
           {context.visibility_buffer, vir::Access::ComputeRW},
           {context.cull_meshlets_cmd_buffer, vir::Access::ComputeRW}}
        )
        .with_name(context.use_hiz ? "vis cull meshes" : "cull meshes")
        .bind_compute_pipeline(render_context.pipeline("cull_meshes"))
        .bind_buffer(0, 0, values.meshes)
        .bind_buffer(0, 1, values.transforms_world)
        .bind_buffer(0, 2, values.mesh_instances)
        .bind_buffer(0, 3, values.meshlet_instances)
        .bind_buffer(0, 4, context.visibility_buffer)
        .bind_buffer(0, 5, context.cull_meshlets_cmd_buffer)
        .specialize_constant(0, std::to_underlying(cull_flags))
        .push_constants_from(cull_camera)
        .dispatch_invocations(prepared.cull.mesh_instance_count, 1, 1)
        .end_compute()
    );
  }

  if (recording.shape.use_mesh_shaders) {
    context.draw_geometry_cmd_buffer = context.cull_meshlets_cmd_buffer;
    return;
  }

  // --- Stage 2: cull_meshlets (three versions with different descriptor sets) ---
  const auto cull_triangles_command = VkDispatchIndirectCommand{.x = 0, .y = 1, .z = 1};
  auto cull_triangles_cmd_buffer = module.transient_buffer({
    .size = sizeof(cull_triangles_command),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "cull triangles command",
  });
  cull_triangles_cmd_buffer = module.update_buffer(
    cull_triangles_cmd_buffer,
    std::span{reinterpret_cast<const u8*>(&cull_triangles_command), sizeof(cull_triangles_command)}
  );

  if (context.use_hiz) {
    std::tie(
      context.cull_meshlets_cmd_buffer,
      context.hiz_attachment,
      values.meshes,
      values.mesh_instances,
      values.meshlet_instances,
      values.transforms_world,
      context.visibility_buffer,
      values.visible_meshlet_instances_indices,
      values.meshlet_instance_visibility_mask,
      cull_triangles_cmd_buffer
    ) = std::tuple_cat( //
      module
        .begin_compute(
          {{context.cull_meshlets_cmd_buffer, vir::Access::IndirectRead},
           {context.hiz_attachment, vir::Access::ComputeSampled},
           {values.meshes, vir::Access::ComputeRead},
           {values.mesh_instances, vir::Access::ComputeRead},
           {values.meshlet_instances, vir::Access::ComputeRead},
           {values.transforms_world, vir::Access::ComputeRead},
           {context.visibility_buffer, vir::Access::ComputeRW},
           {values.visible_meshlet_instances_indices, vir::Access::ComputeRW},
           {values.meshlet_instance_visibility_mask, vir::Access::ComputeRW},
           {cull_triangles_cmd_buffer, vir::Access::ComputeRW}}
        )
        .with_name(stack.format("vis cull meshlets {}", is_late ? "late" : "early"))
        .bind_compute_pipeline(render_context.pipeline("cull_meshlets_hiz"))
        .bind_image(0, 0, context.hiz_attachment)
        .bind_buffer(0, 1, values.meshes)
        .bind_buffer(0, 2, values.mesh_instances)
        .bind_buffer(0, 3, values.meshlet_instances)
        .bind_buffer(0, 4, values.transforms_world)
        .bind_buffer(0, 5, context.visibility_buffer)
        .bind_buffer(0, 6, values.visible_meshlet_instances_indices)
        .bind_buffer(0, 7, values.meshlet_instance_visibility_mask)
        .bind_buffer(0, 8, cull_triangles_cmd_buffer)
        .specialize_constant(0, std::to_underlying(cull_flags))
        .push_constants_from(cull_camera)
        .dispatch_indirect(context.cull_meshlets_cmd_buffer)
        .end_compute()
    );
  } else if (context.use_hpb) {
    std::tie(
      context.cull_meshlets_cmd_buffer,
      values.meshes,
      values.mesh_instances,
      values.meshlet_instances,
      values.transforms_world,
      context.visibility_buffer,
      values.visible_meshlet_instances_indices,
      cull_triangles_cmd_buffer,
      context.hpb_attachment,
      context.vsm_clipmaps_buffer,
      context.vsm_clipmap_dirty_flags_buffer
    ) = std::tuple_cat( //
      module
        .begin_compute(
          {{context.cull_meshlets_cmd_buffer, vir::Access::IndirectRead},
           {values.meshes, vir::Access::ComputeRead},
           {values.mesh_instances, vir::Access::ComputeRead},
           {values.meshlet_instances, vir::Access::ComputeRead},
           {values.transforms_world, vir::Access::ComputeRead},
           {context.visibility_buffer, vir::Access::ComputeRW},
           {values.visible_meshlet_instances_indices, vir::Access::ComputeRW},
           {cull_triangles_cmd_buffer, vir::Access::ComputeRW},
           {context.hpb_attachment, vir::Access::ComputeSampled},
           {context.vsm_clipmaps_buffer, vir::Access::ComputeRead},
           {context.vsm_clipmap_dirty_flags_buffer, vir::Access::ComputeRead}}
        )
        .with_name("rmvsm cull meshlets")
        .bind_compute_pipeline(render_context.pipeline("cull_meshlets_hpb"))
        .bind_buffer(0, 0, values.meshes)
        .bind_buffer(0, 1, values.mesh_instances)
        .bind_buffer(0, 2, values.meshlet_instances)
        .bind_buffer(0, 3, values.transforms_world)
        .bind_buffer(0, 4, context.visibility_buffer)
        .bind_buffer(0, 5, values.visible_meshlet_instances_indices)
        .bind_buffer(0, 6, cull_triangles_cmd_buffer)
        .bind_image(0, 7, context.hpb_attachment)
        .bind_sampler(0, 8, render_context.cached_sampler_handle(NearestSamplerClamped))
        .bind_buffer(0, 9, context.vsm_clipmaps_buffer)
        .bind_buffer(0, 10, context.vsm_clipmap_dirty_flags_buffer)
        .push_constants_from(prepared.cull.clipmap_push)
        .dispatch_indirect(context.cull_meshlets_cmd_buffer)
        .end_compute()
    );
  } else {
    static constexpr auto cull_meshlets_flags = GPU::CullFlag::TestFrustum;
    std::tie(
      context.cull_meshlets_cmd_buffer,
      values.meshes,
      values.mesh_instances,
      values.meshlet_instances,
      values.transforms_world,
      context.visibility_buffer,
      values.visible_meshlet_instances_indices,
      cull_triangles_cmd_buffer
    ) = std::tuple_cat( //
      module
        .begin_compute(
          {{context.cull_meshlets_cmd_buffer, vir::Access::IndirectRead},
           {values.meshes, vir::Access::ComputeRead},
           {values.mesh_instances, vir::Access::ComputeRead},
           {values.meshlet_instances, vir::Access::ComputeRead},
           {values.transforms_world, vir::Access::ComputeRead},
           {context.visibility_buffer, vir::Access::ComputeRW},
           {values.visible_meshlet_instances_indices, vir::Access::ComputeRW},
           {cull_triangles_cmd_buffer, vir::Access::ComputeRW}}
        )
        .with_name("cull meshlets")
        .bind_compute_pipeline(render_context.pipeline("cull_meshlets"))
        .bind_buffer(0, 0, values.meshes)
        .bind_buffer(0, 1, values.mesh_instances)
        .bind_buffer(0, 2, values.meshlet_instances)
        .bind_buffer(0, 3, values.transforms_world)
        .bind_buffer(0, 4, context.visibility_buffer)
        .bind_buffer(0, 5, values.visible_meshlet_instances_indices)
        .bind_buffer(0, 6, cull_triangles_cmd_buffer)
        .specialize_constant(0, std::to_underlying(cull_meshlets_flags))
        .push_constants_from(cull_camera)
        .dispatch_indirect(context.cull_meshlets_cmd_buffer)
        .end_compute()
    );
  }

  // --- Stage 3: cull_triangles (shared by every version) ---
  const auto draw_geometry_command = VkDrawIndexedIndirectCommand{
    .indexCount = 0,
    .instanceCount = 1,
    .firstIndex = 0,
    .vertexOffset = 0,
    .firstInstance = 0
  };
  context.draw_geometry_cmd_buffer = module.transient_buffer({
    .size = sizeof(draw_geometry_command),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "draw geometry command",
  });
  context.draw_geometry_cmd_buffer = module.update_buffer(
    context.draw_geometry_cmd_buffer,
    std::span{reinterpret_cast<const u8*>(&draw_geometry_command), sizeof(draw_geometry_command)}
  );
  std::tie(
    std::ignore,
    values.meshes,
    values.mesh_instances,
    values.meshlet_instances,
    context.visibility_buffer,
    values.transforms_world,
    values.visible_meshlet_instances_indices,
    values.reordered_indices,
    context.draw_geometry_cmd_buffer
  ) = std::tuple_cat( //
    module
      .begin_compute(
        {{cull_triangles_cmd_buffer, vir::Access::IndirectRead},
         {values.meshes, vir::Access::ComputeRead},
         {values.mesh_instances, vir::Access::ComputeRead},
         {values.meshlet_instances, vir::Access::ComputeRead},
         {context.visibility_buffer, vir::Access::ComputeRead},
         {values.transforms_world, vir::Access::ComputeRead},
         {values.visible_meshlet_instances_indices, vir::Access::ComputeRead},
         {values.reordered_indices, vir::Access::ComputeRW},
         {context.draw_geometry_cmd_buffer, vir::Access::ComputeRW}}
      )
      .with_name(context.use_hiz ? stack.format("cull triangles {}", is_late ? "late" : "early") : "cull triangles")
      .bind_compute_pipeline(render_context.pipeline("cull_triangles"))
      .bind_buffer(0, 0, values.meshes)
      .bind_buffer(0, 1, values.mesh_instances)
      .bind_buffer(0, 2, values.meshlet_instances)
      .bind_buffer(0, 3, context.visibility_buffer)
      .bind_buffer(0, 4, values.transforms_world)
      .bind_buffer(0, 5, values.visible_meshlet_instances_indices)
      .bind_buffer(0, 6, values.reordered_indices)
      .bind_buffer(0, 7, context.draw_geometry_cmd_buffer)
      .specialize_constant(0, std::to_underlying(cull_flags))
      .push_constants_from(cull_camera)
      .dispatch_indirect(cull_triangles_cmd_buffer)
      .end_compute()
  );
}

auto RendererInstance::cull_geometry_pointspot(
  vir::Module& module, RenderContext& render_context, FrameRecording& recording, CullGeometryPointSpotContext& context
) -> void {
  ZoneScoped;
  auto& prepared = recording.prepared;
  auto& values = recording.values;

  memory::ScopedStack stack;
  auto& variables = prepared.cull;
  const auto mip = context.curr_mip;
  if (variables.pointspot_contexts[mip] == vir::ValueID::Invalid) {
    variables.pointspot_contexts[mip] = module.declare_bytes_var(
      stack.format("pointspot cull context {}", mip),
      sizeof(GPU::VSMPointSpotContext)
    );
    variables.pointspot_cull_meshes_commands[mip] = module.declare_bytes_var(
      stack.format("vsm pointspot cull meshes command {}", mip),
      sizeof(VkDispatchIndirectCommand)
    );
  }

  const auto ps_ctx = prepared.cull.pointspot_contexts[context.curr_mip];

  if (context.init) {
    context.vsm_meshlet_instances_buffer = module.transient_buffer({
      .size = std::max<u64>(RMVSMContext::POINT_SPOT_MAX_MESHLET_INSTANCES * sizeof(GPU::VSMMeshletInstance), 4),
      .usage = TRANSIENT_BUFFER_USAGE,
      .location = vir::MemoryLocation::GpuOnly,
      .name = "vsm pointspot meshlet instances",
    });
    if (!recording.shape.use_mesh_shaders) {
      context.visible_indices_buffer = module.transient_buffer({
        .size = std::max<u64>(RMVSMContext::POINT_SPOT_MAX_MESHLET_INSTANCES * sizeof(u32), 4),
        .usage = TRANSIENT_BUFFER_USAGE,
        .location = vir::MemoryLocation::GpuOnly,
        .name = "vsm pointspot visible indices",
      });
      context.reordered_indices_buffer = module.transient_buffer({
        .size = std::max<u64>(RMVSMContext::POINT_SPOT_MAX_INDEX_COUNT * sizeof(u32), 4),
        .usage = TRANSIENT_BUFFER_USAGE,
        .location = vir::MemoryLocation::GpuOnly,
        .name = "vsm pointspot reordered indices",
      });
    }
  }

  const auto meshlet_count = 0_u32;
  auto vsm_meshlet_instance_count_buffer = module.transient_buffer({
    .size = sizeof(meshlet_count),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "vsm pointspot meshlet count",
  });
  vsm_meshlet_instance_count_buffer = module.update_buffer(
    vsm_meshlet_instance_count_buffer,
    std::span{reinterpret_cast<const u8*>(&meshlet_count), sizeof(meshlet_count)}
  );
  const auto cull_meshlets_command = VkDispatchIndirectCommand{.x = 0, .y = 1, .z = 1};
  auto cull_meshlets_cmd_buffer = module.transient_buffer({
    .size = sizeof(cull_meshlets_command),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "vsm pointspot cull meshlets command",
  });
  cull_meshlets_cmd_buffer = module.update_buffer(
    cull_meshlets_cmd_buffer,
    std::span{reinterpret_cast<const u8*>(&cull_meshlets_command), sizeof(cull_meshlets_command)}
  );

  // compact active layers into the indirect dispatch's Y dimension
  auto active_layers_buffer = module.transient_buffer({
    .size = std::max<u64>(RMVSMContext::POINT_SPOT_LAYER_COUNT * sizeof(u32), 4),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "vsm pointspot active layers",
  });
  auto cull_meshes_cmd_buffer = module.transient_buffer({
    .size = sizeof(VkDispatchIndirectCommand),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "vsm pointspot cull meshes command",
  });
  cull_meshes_cmd_buffer = module.update_buffer_from(
    cull_meshes_cmd_buffer,
    prepared.cull.pointspot_cull_meshes_commands[context.curr_mip]
  );

  std::tie(
    context.views_buffer,
    context.layer_dirty_mask_buffer,
    active_layers_buffer,
    cull_meshes_cmd_buffer
  ) = std::tuple_cat( //
    module
      .begin_compute(
        {{context.views_buffer, vir::Access::ComputeRead},
         {context.layer_dirty_mask_buffer, vir::Access::ComputeRead},
         {active_layers_buffer, vir::Access::ComputeWrite},
         {cull_meshes_cmd_buffer, vir::Access::ComputeRW}}
      )
      .with_name("rmvsm pointspot build active layers")
      .bind_compute_pipeline(render_context.pipeline("rmvsm_pointspot_build_active_layers"))
      .bind_buffer(0, 0, context.views_buffer)
      .bind_buffer(0, 1, context.layer_dirty_mask_buffer)
      .bind_buffer(0, 2, active_layers_buffer)
      .bind_buffer(0, 3, cull_meshes_cmd_buffer)
      .push_constants_from(ps_ctx)
      .dispatch_invocations(RMVSMContext::POINT_SPOT_LAYER_COUNT, 1, 1)
      .end_compute()
  );

  std::tie(
    std::ignore,
    values.meshes,
    values.transforms_world,
    values.mesh_instances,
    context.views_buffer,
    active_layers_buffer,
    context.vsm_meshlet_instances_buffer,
    vsm_meshlet_instance_count_buffer,
    cull_meshlets_cmd_buffer
  ) = std::tuple_cat( //
    module
      .begin_compute(
        {{cull_meshes_cmd_buffer, vir::Access::IndirectRead},
         {values.meshes, vir::Access::ComputeRead},
         {values.transforms_world, vir::Access::ComputeRead},
         {values.mesh_instances, vir::Access::ComputeRead},
         {context.views_buffer, vir::Access::ComputeRead},
         {active_layers_buffer, vir::Access::ComputeRead},
         {context.vsm_meshlet_instances_buffer, vir::Access::ComputeWrite},
         {vsm_meshlet_instance_count_buffer, vir::Access::ComputeRW},
         {cull_meshlets_cmd_buffer, vir::Access::ComputeRW}}
      )
      .with_name("rmvsm pointspot cull meshes")
      .bind_compute_pipeline(render_context.pipeline("rmvsm_pointspot_cull_meshes"))
      .bind_buffer(0, 0, values.meshes)
      .bind_buffer(0, 1, values.transforms_world)
      .bind_buffer(0, 2, values.mesh_instances)
      .bind_buffer(0, 3, context.views_buffer)
      .bind_buffer(0, 4, active_layers_buffer)
      .bind_buffer(0, 5, context.vsm_meshlet_instances_buffer)
      .bind_buffer(0, 6, vsm_meshlet_instance_count_buffer)
      .bind_buffer(0, 7, cull_meshlets_cmd_buffer)
      .push_constants_from(ps_ctx)
      .dispatch_indirect(cull_meshes_cmd_buffer)
      .end_compute()
  );

  // each page-table mip has an independent occupancy pyramid, built one level per pass so every
  // level reads the one before it with the barrier the graph puts between them
  const auto curr_mip = context.curr_mip;
  const auto base_extent = static_cast<u32>(RMVSMContext::POINT_SPOT_PAGE_TABLE_SIZE) >> curr_mip;
  const auto level_count = RMVSMContext::POINT_SPOT_HPB_LEVEL_COUNT - curr_mip;
  constexpr auto layer_count = RMVSMContext::POINT_SPOT_LAYER_COUNT;
  for (auto i = 0_u32; i < level_count; i++) {
    const auto level_extent = vir::Extent3D{
      std::max(1u, base_extent >> i),
      std::max(1u, base_extent >> i),
      layer_count
    };
    const auto src = module.mip(context.hpb_attachment, i == 0 ? 0 : i - 1);
    const auto dst = module.mip(context.hpb_attachment, i);
    const auto [page_table, _src, written] = //
      module
        .begin_compute(
          {{context.page_table_attachment, vir::Access::ComputeSampled},
           {src, vir::Access::ComputeSampled},
           {dst, vir::Access::ComputeRW}}
        )
        .with_name("rmvsm pointspot downsample hpb")
        .bind_compute_pipeline(render_context.pipeline("rmvsm_pointspot_downsample_hpb"))
        .specialize_constant(0, i == 0)
        .bind_image(0, 0, context.page_table_attachment)
        .bind_image(0, 1, src)
        .bind_image(0, 2, dst)
        .push_constants(PushConstants(level_extent, curr_mip))
        .dispatch_invocations(level_extent.width, level_extent.height, level_extent.depth)
        .end_compute();
    context.page_table_attachment = page_table;
    context.hpb_attachment = module.whole(written);
  }

  if (recording.shape.use_mesh_shaders) {
    context.vsm_meshlet_instance_count_buffer = vsm_meshlet_instance_count_buffer;
    context.draw_cmd_buffer = cull_meshlets_cmd_buffer;
    return;
  }

  const auto cull_triangles_command = VkDispatchIndirectCommand{.x = 0, .y = 1, .z = 1};
  auto cull_triangles_cmd_buffer = module.transient_buffer({
    .size = sizeof(cull_triangles_command),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "vsm pointspot cull triangles command",
  });
  cull_triangles_cmd_buffer = module.update_buffer(
    cull_triangles_cmd_buffer,
    std::span{reinterpret_cast<const u8*>(&cull_triangles_command), sizeof(cull_triangles_command)}
  );
  const auto draw_command = VkDrawIndexedIndirectCommand{
    .indexCount = 0,
    .instanceCount = 1,
    .firstIndex = 0,
    .vertexOffset = 0,
    .firstInstance = 0
  };
  context.draw_cmd_buffer = module.transient_buffer({
    .size = sizeof(draw_command),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "vsm pointspot draw command",
  });
  context.draw_cmd_buffer = module.update_buffer(
    context.draw_cmd_buffer,
    std::span{reinterpret_cast<const u8*>(&draw_command), sizeof(draw_command)}
  );

  auto cull_pass = //
    module
      .begin_compute(
        {{cull_meshlets_cmd_buffer, vir::Access::IndirectRead},
         {values.meshes, vir::Access::ComputeRead},
         {values.mesh_instances, vir::Access::ComputeRead},
         {values.transforms_world, vir::Access::ComputeRead},
         {context.views_buffer, vir::Access::ComputeRead},
         {context.vsm_meshlet_instances_buffer, vir::Access::ComputeRead},
         {vsm_meshlet_instance_count_buffer, vir::Access::ComputeRead},
         {context.hpb_attachment, vir::Access::ComputeSampled},
         {context.visible_indices_buffer, vir::Access::ComputeWrite},
         {cull_triangles_cmd_buffer, vir::Access::ComputeRW}}
      )
      .with_name("rmvsm pointspot cull meshlets")
      .bind_compute_pipeline(render_context.pipeline("rmvsm_pointspot_cull_meshlets"));
  std::tie(
    std::ignore,
    values.meshes,
    values.mesh_instances,
    values.transforms_world,
    context.views_buffer,
    context.vsm_meshlet_instances_buffer,
    vsm_meshlet_instance_count_buffer,
    context.hpb_attachment,
    context.visible_indices_buffer,
    cull_triangles_cmd_buffer
  ) = std::tuple_cat( //
    bind_vsm_pointspot_spec_constants(cull_pass)
      .bind_buffer(0, 0, values.meshes)
      .bind_buffer(0, 1, values.mesh_instances)
      .bind_buffer(0, 2, values.transforms_world)
      .bind_buffer(0, 3, context.views_buffer)
      .bind_buffer(0, 4, context.vsm_meshlet_instances_buffer)
      .bind_buffer(0, 5, vsm_meshlet_instance_count_buffer)
      .bind_image(0, 6, context.hpb_attachment)
      .bind_buffer(0, 7, context.visible_indices_buffer)
      .bind_buffer(0, 8, cull_triangles_cmd_buffer)
      .push_constants_from(ps_ctx)
      .dispatch_indirect(cull_meshlets_cmd_buffer)
      .end_compute()
  );

  std::tie(
    std::ignore,
    values.meshes,
    values.mesh_instances,
    values.transforms_world,
    context.views_buffer,
    context.vsm_meshlet_instances_buffer,
    context.visible_indices_buffer,
    context.reordered_indices_buffer,
    context.draw_cmd_buffer
  ) = std::tuple_cat( //
    module
      .begin_compute(
        {{cull_triangles_cmd_buffer, vir::Access::IndirectRead},
         {values.meshes, vir::Access::ComputeRead},
         {values.mesh_instances, vir::Access::ComputeRead},
         {values.transforms_world, vir::Access::ComputeRead},
         {context.views_buffer, vir::Access::ComputeRead},
         {context.vsm_meshlet_instances_buffer, vir::Access::ComputeRead},
         {context.visible_indices_buffer, vir::Access::ComputeRead},
         {context.reordered_indices_buffer, vir::Access::ComputeWrite},
         {context.draw_cmd_buffer, vir::Access::ComputeRW}}
      )
      .with_name("rmvsm pointspot cull triangles")
      .bind_compute_pipeline(render_context.pipeline("rmvsm_pointspot_cull_triangles"))
      .bind_buffer(0, 0, values.meshes)
      .bind_buffer(0, 1, values.mesh_instances)
      .bind_buffer(0, 2, values.transforms_world)
      .bind_buffer(0, 3, context.views_buffer)
      .bind_buffer(0, 4, context.vsm_meshlet_instances_buffer)
      .bind_buffer(0, 5, context.visible_indices_buffer)
      .bind_buffer(0, 6, context.reordered_indices_buffer)
      .bind_buffer(0, 7, context.draw_cmd_buffer)
      .push_constants_from(ps_ctx)
      .dispatch_indirect(cull_triangles_cmd_buffer)
      .end_compute()
  );
}
} // namespace ox
