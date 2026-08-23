#include <vuk/runtime/CommandBuffer.hpp>

#include "Core/App.hpp"
#include "Core/Enum.hpp"
#include "Memory/Stack.hpp"
#include "Render/RendererInstance.hpp"
#include "Render/Utils/VukCommon.hpp"

namespace ox {
auto RendererInstance::generate_hiz(this RendererInstance& self, MainGeometryContext& context) -> void {
  ZoneScoped;

  auto spd_global_atomic = self.renderer.render_context->scratch_buffer<u32>(0u);

  auto hiz_generate_pass = vuk::make_pass(
    "hiz generate",
    [](
      vuk::CommandBuffer& cmd_list, //
      VUK_BA(vuk::eComputeRW) spd_atomic,
      VUK_IA(vuk::eComputeSampled) src,
      VUK_IA(vuk::eComputeRW) dst
    ) {
      const auto extent = dst->extent;
      const auto mip_count = std::min(dst->level_count, 13u);
      const auto work_group_count = (extent.width / 64) * (extent.height / 64);

      cmd_list //
        .bind_compute_pipeline("hiz")
        .specialize_constants(0, 1_u32)
        .specialize_constants(1, extent.width)
        .specialize_constants(2, extent.height)
        .bind_buffer(0, 0, spd_atomic)
        .bind_sampler(0, 1, vuk::NearestSamplerClamped)
        .bind_image(0, 2, src);

      for (auto i = 0_u32; i < 13; i++) {
        cmd_list.bind_image(0, 3 + i, dst->mip(std::min(i, dst->level_count - 1)));
      }

      cmd_list //
        .push_constants(
          vuk::ShaderStageFlagBits::eCompute,
          0,
          PushConstants(mip_count, work_group_count, glm::mat2x2(1.0f))
        )
        .dispatch(extent.width / 64, extent.height / 64, 1);

      return std::make_tuple(spd_atomic, src, dst);
    }
  );

  auto [_atomic, depth, hiz] = hiz_generate_pass(
    std::move(spd_global_atomic),
    std::move(context.depth_attachment),
    std::move(context.hiz_attachment)
  );
  context.depth_attachment = std::move(depth);
  context.hiz_attachment = std::move(hiz);
}

auto RendererInstance::cull_geometry(this RendererInstance& self, CullGeometryContext& context) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  const auto cull_flags = context.cull_flags;
  const auto& cull_camera = context.cull_camera;

  // --- Stage 1: cull_meshes (only on the first cull of a sequence) ---
  if (context.init_cull_meshes) {
    auto cull_meshes_pass = vuk::make_pass(
      context.use_hiz ? "vis cull meshes" : "cull meshes",
      [cull_camera, cull_flags](
        vuk::CommandBuffer& cmd_list,
        VUK_BA(vuk::eComputeRead) meshes,
        VUK_BA(vuk::eComputeRead) transforms,
        VUK_BA(vuk::eComputeRW) mesh_instances,
        VUK_BA(vuk::eComputeRW) meshlet_instances,
        VUK_BA(vuk::eComputeRW) visibility,
        VUK_BA(vuk::eComputeRW) cull_meshlets_cmd
      ) {
        cmd_list //
          .bind_compute_pipeline("cull_meshes")
          .bind_buffer(0, 0, meshes)
          .bind_buffer(0, 1, transforms)
          .bind_buffer(0, 2, mesh_instances)
          .bind_buffer(0, 3, meshlet_instances)
          .bind_buffer(0, 4, visibility)
          .bind_buffer(0, 5, cull_meshlets_cmd)
          .specialize_constants(0, std::to_underlying(cull_flags))
          .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, cull_camera)
          .dispatch_invocations(cull_camera.mesh_instance_count);

        return std::make_tuple(meshes, transforms, mesh_instances, meshlet_instances, visibility, cull_meshlets_cmd);
      }
    );

    context.visibility_buffer = self.renderer.render_context->scratch_buffer<GPU::MeshletInstanceVisibility>({});
    context.cull_meshlets_cmd_buffer = self.renderer.render_context->scratch_buffer<vuk::DispatchIndirectCommand>(
      {.x = 0, .y = 1, .z = 1}
    );
    std::tie(
      self.prepared_frame.meshes_buffer,
      self.prepared_frame.transforms_world_buffer,
      self.prepared_frame.mesh_instances_buffer,
      self.prepared_frame.meshlet_instances_buffer,
      context.visibility_buffer,
      context.cull_meshlets_cmd_buffer
    ) =
      cull_meshes_pass(
        std::move(self.prepared_frame.meshes_buffer),
        std::move(self.prepared_frame.transforms_world_buffer),
        std::move(self.prepared_frame.mesh_instances_buffer),
        std::move(self.prepared_frame.meshlet_instances_buffer),
        std::move(context.visibility_buffer),
        std::move(context.cull_meshlets_cmd_buffer)
      );
  }

  if (self.prepared_frame.use_mesh_shaders) {
    context.draw_geometry_cmd_buffer = std::move(context.cull_meshlets_cmd_buffer);
    return;
  }

  // --- Stage 2: cull_meshlets (two versions due to different descriptor sets) ---
  auto cull_triangles_cmd_buffer = self.renderer.render_context->scratch_buffer<vuk::DispatchIndirectCommand>(
    {.x = 0, .y = 1, .z = 1}
  );

  if (context.use_hiz) {
    auto cull_meshlets_pass = vuk::make_pass(
      stack.format("vis cull meshlets {}", cull_flags & GPU::CullFlag::LatePass ? "late" : "early"),
      [cull_camera, cull_flags](
        vuk::CommandBuffer& cmd_list,
        VUK_BA(vuk::eIndirectRead) dispatch_cmd,
        VUK_IA(vuk::eComputeSampled) hiz,
        VUK_BA(vuk::eComputeRead) meshes,
        VUK_BA(vuk::eComputeRead) mesh_instances,
        VUK_BA(vuk::eComputeRead) meshlet_instances,
        VUK_BA(vuk::eComputeRead) transforms,
        VUK_BA(vuk::eComputeRW) visibility,
        VUK_BA(vuk::eComputeRW) visible_meshlet_instances_indices,
        VUK_BA(vuk::eComputeRW) meshlet_instance_visibility_mask,
        VUK_BA(vuk::eComputeRW) cull_triangles_cmd
      ) {
        cmd_list //
          .bind_compute_pipeline("cull_meshlets_hiz")
          .bind_image(0, 0, hiz)
          .bind_buffer(0, 1, meshes)
          .bind_buffer(0, 2, mesh_instances)
          .bind_buffer(0, 3, meshlet_instances)
          .bind_buffer(0, 4, transforms)
          .bind_buffer(0, 5, visibility)
          .bind_buffer(0, 6, visible_meshlet_instances_indices)
          .bind_buffer(0, 7, meshlet_instance_visibility_mask)
          .bind_buffer(0, 8, cull_triangles_cmd)
          .specialize_constants(0, std::to_underlying(cull_flags))
          .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, cull_camera)
          .dispatch_indirect(dispatch_cmd);

        return std::make_tuple(
          dispatch_cmd,
          hiz,
          meshes,
          mesh_instances,
          meshlet_instances,
          transforms,
          visibility,
          visible_meshlet_instances_indices,
          meshlet_instance_visibility_mask,
          cull_triangles_cmd
        );
      }
    );

    std::tie(
      context.cull_meshlets_cmd_buffer,
      context.hiz_attachment,
      self.prepared_frame.meshes_buffer,
      self.prepared_frame.mesh_instances_buffer,
      self.prepared_frame.meshlet_instances_buffer,
      self.prepared_frame.transforms_world_buffer,
      context.visibility_buffer,
      self.prepared_frame.visible_meshlet_instances_indices_buffer,
      self.prepared_frame.meshlet_instance_visibility_mask_buffer,
      cull_triangles_cmd_buffer
    ) =
      cull_meshlets_pass(
        std::move(context.cull_meshlets_cmd_buffer),
        std::move(context.hiz_attachment),
        std::move(self.prepared_frame.meshes_buffer),
        std::move(self.prepared_frame.mesh_instances_buffer),
        std::move(self.prepared_frame.meshlet_instances_buffer),
        std::move(self.prepared_frame.transforms_world_buffer),
        std::move(context.visibility_buffer),
        std::move(self.prepared_frame.visible_meshlet_instances_indices_buffer),
        std::move(self.prepared_frame.meshlet_instance_visibility_mask_buffer),
        std::move(cull_triangles_cmd_buffer)
      );
  } else if (context.use_hpb) {
    auto cull_meshlets_pass = vuk::make_pass(
      "rmvsm cull meshlets",
      [cull_camera, clipmap_count = context.vsm_clipmap_count](
        vuk::CommandBuffer& cmd_list,
        VUK_BA(vuk::eIndirectRead) dispatch_cmd,
        VUK_BA(vuk::eComputeRead) meshes,
        VUK_BA(vuk::eComputeRead) mesh_instances,
        VUK_BA(vuk::eComputeRead) meshlet_instances,
        VUK_BA(vuk::eComputeRead) transforms,
        VUK_BA(vuk::eComputeRW) visibility,
        VUK_BA(vuk::eComputeRW) visible_meshlet_instances_indices,
        VUK_BA(vuk::eComputeRW) cull_triangles_cmd,
        VUK_IA(vuk::eComputeSampled) hpb,
        VUK_BA(vuk::eComputeRead) clipmaps,
        VUK_BA(vuk::eComputeRead) clipmap_dirty_flags
      ) {
        cmd_list //
          .bind_compute_pipeline("cull_meshlets_hpb")
          .bind_buffer(0, 0, meshes)
          .bind_buffer(0, 1, mesh_instances)
          .bind_buffer(0, 2, meshlet_instances)
          .bind_buffer(0, 3, transforms)
          .bind_buffer(0, 4, visibility)
          .bind_buffer(0, 5, visible_meshlet_instances_indices)
          .bind_buffer(0, 6, cull_triangles_cmd)
          .bind_image(0, 7, hpb)
          .bind_sampler(0, 8, vuk::NearestSamplerClamped)
          .bind_buffer(0, 9, clipmaps)
          .bind_buffer(0, 10, clipmap_dirty_flags)
          .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(cull_camera, clipmap_count))
          .dispatch_indirect(dispatch_cmd);

        return std::make_tuple(
          dispatch_cmd,
          meshes,
          mesh_instances,
          meshlet_instances,
          transforms,
          visibility,
          visible_meshlet_instances_indices,
          cull_triangles_cmd,
          hpb,
          clipmaps,
          clipmap_dirty_flags
        );
      }
    );

    std::tie(
      context.cull_meshlets_cmd_buffer,
      self.prepared_frame.meshes_buffer,
      self.prepared_frame.mesh_instances_buffer,
      self.prepared_frame.meshlet_instances_buffer,
      self.prepared_frame.transforms_world_buffer,
      context.visibility_buffer,
      self.prepared_frame.visible_meshlet_instances_indices_buffer,
      cull_triangles_cmd_buffer,
      context.hpb_attachment,
      context.vsm_clipmaps_buffer,
      context.vsm_clipmap_dirty_flags_buffer
    ) =
      cull_meshlets_pass(
        std::move(context.cull_meshlets_cmd_buffer),
        std::move(self.prepared_frame.meshes_buffer),
        std::move(self.prepared_frame.mesh_instances_buffer),
        std::move(self.prepared_frame.meshlet_instances_buffer),
        std::move(self.prepared_frame.transforms_world_buffer),
        std::move(context.visibility_buffer),
        std::move(self.prepared_frame.visible_meshlet_instances_indices_buffer),
        std::move(cull_triangles_cmd_buffer),
        std::move(context.hpb_attachment),
        std::move(context.vsm_clipmaps_buffer),
        std::move(context.vsm_clipmap_dirty_flags_buffer)
      );
  } else {
    static constexpr auto cull_meshlets_flags = GPU::CullFlag::TestFrustum;
    auto cull_meshlets_pass = vuk::make_pass(
      "cull meshlets",
      [cull_camera](
        vuk::CommandBuffer& cmd_list,
        VUK_BA(vuk::eIndirectRead) dispatch_cmd,
        VUK_BA(vuk::eComputeRead) meshes,
        VUK_BA(vuk::eComputeRead) mesh_instances,
        VUK_BA(vuk::eComputeRead) meshlet_instances,
        VUK_BA(vuk::eComputeRead) transforms,
        VUK_BA(vuk::eComputeRW) visibility,
        VUK_BA(vuk::eComputeRW) visible_meshlet_instances_indices,
        VUK_BA(vuk::eComputeRW) cull_triangles_cmd
      ) {
        cmd_list //
          .bind_compute_pipeline("cull_meshlets")
          .bind_buffer(0, 0, meshes)
          .bind_buffer(0, 1, mesh_instances)
          .bind_buffer(0, 2, meshlet_instances)
          .bind_buffer(0, 3, transforms)
          .bind_buffer(0, 4, visibility)
          .bind_buffer(0, 5, visible_meshlet_instances_indices)
          .bind_buffer(0, 6, cull_triangles_cmd)
          .specialize_constants(0, std::to_underlying(cull_meshlets_flags))
          .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, cull_camera)
          .dispatch_indirect(dispatch_cmd);

        return std::make_tuple(
          dispatch_cmd,
          meshes,
          mesh_instances,
          meshlet_instances,
          transforms,
          visibility,
          visible_meshlet_instances_indices,
          cull_triangles_cmd
        );
      }
    );

    std::tie(
      context.cull_meshlets_cmd_buffer,
      self.prepared_frame.meshes_buffer,
      self.prepared_frame.mesh_instances_buffer,
      self.prepared_frame.meshlet_instances_buffer,
      self.prepared_frame.transforms_world_buffer,
      context.visibility_buffer,
      self.prepared_frame.visible_meshlet_instances_indices_buffer,
      cull_triangles_cmd_buffer
    ) =
      cull_meshlets_pass(
        std::move(context.cull_meshlets_cmd_buffer),
        std::move(self.prepared_frame.meshes_buffer),
        std::move(self.prepared_frame.mesh_instances_buffer),
        std::move(self.prepared_frame.meshlet_instances_buffer),
        std::move(self.prepared_frame.transforms_world_buffer),
        std::move(context.visibility_buffer),
        std::move(self.prepared_frame.visible_meshlet_instances_indices_buffer),
        std::move(cull_triangles_cmd_buffer)
      );
  }

  // --- Stage 3: cull_triangles (shared by both versions) ---
  auto cull_triangles_pass = vuk::make_pass(
    context.use_hiz ? stack.format("cull triangles {}", cull_flags & GPU::CullFlag::LatePass ? "late" : "early")
                    : "cull triangles",
    [cull_flags, cull_camera](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eIndirectRead) cull_triangles_cmd,
      VUK_BA(vuk::eComputeRead) meshes,
      VUK_BA(vuk::eComputeRead) mesh_instances,
      VUK_BA(vuk::eComputeRead) meshlet_instances,
      VUK_BA(vuk::eComputeRead) visibility,
      VUK_BA(vuk::eComputeRead) transforms,
      VUK_BA(vuk::eComputeRead) visible_meshlet_instances_indices,
      VUK_BA(vuk::eComputeRW) reordered_indices,
      VUK_BA(vuk::eComputeRW) draw_indexed_cmd
    ) {
      cmd_list //
        .bind_compute_pipeline("cull_triangles")
        .bind_buffer(0, 0, meshes)
        .bind_buffer(0, 1, mesh_instances)
        .bind_buffer(0, 2, meshlet_instances)
        .bind_buffer(0, 3, visibility)
        .bind_buffer(0, 4, transforms)
        .bind_buffer(0, 5, visible_meshlet_instances_indices)
        .bind_buffer(0, 6, reordered_indices)
        .bind_buffer(0, 7, draw_indexed_cmd)
        .specialize_constants(0, std::to_underlying(cull_flags))
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, cull_camera)
        .dispatch_indirect(cull_triangles_cmd);

      return std::make_tuple(
        meshes,
        mesh_instances,
        meshlet_instances,
        visibility,
        transforms,
        visible_meshlet_instances_indices,
        reordered_indices,
        draw_indexed_cmd
      );
    }
  );

  context.draw_geometry_cmd_buffer = self.renderer.render_context->scratch_buffer<vuk::DrawIndexedIndirectCommand>(
    {.instanceCount = 1}
  );
  std::tie(
    self.prepared_frame.meshes_buffer,
    self.prepared_frame.mesh_instances_buffer,
    self.prepared_frame.meshlet_instances_buffer,
    context.visibility_buffer,
    self.prepared_frame.transforms_world_buffer,
    self.prepared_frame.visible_meshlet_instances_indices_buffer,
    self.prepared_frame.reordered_indices_buffer,
    context.draw_geometry_cmd_buffer
  ) =
    cull_triangles_pass(
      std::move(cull_triangles_cmd_buffer),
      std::move(self.prepared_frame.meshes_buffer),
      std::move(self.prepared_frame.mesh_instances_buffer),
      std::move(self.prepared_frame.meshlet_instances_buffer),
      std::move(context.visibility_buffer),
      std::move(self.prepared_frame.transforms_world_buffer),
      std::move(self.prepared_frame.visible_meshlet_instances_indices_buffer),
      std::move(self.prepared_frame.reordered_indices_buffer),
      std::move(context.draw_geometry_cmd_buffer)
    );
}

auto RendererInstance::cull_geometry_pointspot(this RendererInstance& self, CullGeometryPointSpotContext& context)
  -> void {
  ZoneScoped;

  auto& render_context = self.renderer.render_context;
  const auto ps_ctx = context.ps_ctx;

  if (context.init) {
    context.vsm_meshlet_instances_buffer = render_context->alloc_transient_buffer(
      vuk::MemoryUsage::eGPUonly,
      RMVSMContext::POINT_SPOT_MAX_MESHLET_INSTANCES * sizeof(GPU::VSMMeshletInstance)
    );
    if (!self.prepared_frame.use_mesh_shaders) {
      context.visible_indices_buffer = render_context->alloc_transient_buffer(
        vuk::MemoryUsage::eGPUonly,
        RMVSMContext::POINT_SPOT_MAX_MESHLET_INSTANCES * sizeof(u32)
      );
      context.reordered_indices_buffer = render_context->alloc_transient_buffer(
        vuk::MemoryUsage::eGPUonly,
        RMVSMContext::POINT_SPOT_MAX_INDEX_COUNT * sizeof(u32)
      );
    }
  }

  auto vsm_meshlet_instance_count_buffer = render_context->scratch_buffer<u32>(0u);
  auto cull_meshlets_cmd_buffer = render_context->scratch_buffer<vuk::DispatchIndirectCommand>(
    {.x = 0, .y = 1, .z = 1}
  );

  // `cull_meshes` used to run over every one of POINT_SPOT_LAYER_COUNT layers,
  // re-reading each mesh, transform and view once per layer just to fail a
  // frustum test. The layers worth visiting are compacted first, and the Y group
  // count of the indirect dispatch doubles as the append cursor: it starts at 0
  // and ends up holding the active layer count.
  auto active_layers_buffer = render_context->alloc_transient_buffer(
    vuk::MemoryUsage::eGPUonly,
    RMVSMContext::POINT_SPOT_LAYER_COUNT * sizeof(u32)
  );
  auto cull_meshes_cmd_buffer = render_context->scratch_buffer<vuk::DispatchIndirectCommand>({
    .x = (ps_ctx.mesh_instance_count + RMVSMContext::CULLING_MESH_COUNT - 1) / RMVSMContext::CULLING_MESH_COUNT,
    .y = 0,
    .z = 1,
  });

  auto build_active_layers_pass = vuk::make_pass(
    "rmvsm pointspot build active layers",
    [ps_ctx](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eComputeRead) views,
      VUK_BA(vuk::eComputeRead) layer_dirty_mask,
      VUK_BA(vuk::eComputeWrite) active_layers,
      VUK_BA(vuk::eComputeRW) cull_meshes_cmd
    ) {
      cmd_list //
        .bind_compute_pipeline("rmvsm_pointspot_build_active_layers")
        .bind_buffer(0, 0, views)
        .bind_buffer(0, 1, layer_dirty_mask)
        .bind_buffer(0, 2, active_layers)
        .bind_buffer(0, 3, cull_meshes_cmd)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, ps_ctx)
        .dispatch_invocations(RMVSMContext::POINT_SPOT_LAYER_COUNT);

      return std::make_tuple(views, layer_dirty_mask, active_layers, cull_meshes_cmd);
    }
  );

  std::tie(context.views_buffer, context.layer_dirty_mask_buffer, active_layers_buffer, cull_meshes_cmd_buffer) =
    build_active_layers_pass(
      std::move(context.views_buffer),
      std::move(context.layer_dirty_mask_buffer),
      std::move(active_layers_buffer),
      std::move(cull_meshes_cmd_buffer)
    );

  auto cull_meshes_pass = vuk::make_pass(
    "rmvsm pointspot cull meshes",
    [ps_ctx](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eIndirectRead) dispatch_cmd,
      VUK_BA(vuk::eComputeRead) meshes,
      VUK_BA(vuk::eComputeRead) transforms,
      VUK_BA(vuk::eComputeRead) mesh_instances,
      VUK_BA(vuk::eComputeRead) views,
      VUK_BA(vuk::eComputeRead) active_layers,
      VUK_BA(vuk::eComputeWrite) vsm_meshlet_instances,
      VUK_BA(vuk::eComputeRW) vsm_meshlet_instance_count,
      VUK_BA(vuk::eComputeRW) cull_meshlets_cmd
    ) {
      cmd_list //
        .bind_compute_pipeline("rmvsm_pointspot_cull_meshes")
        .bind_buffer(0, 0, meshes)
        .bind_buffer(0, 1, transforms)
        .bind_buffer(0, 2, mesh_instances)
        .bind_buffer(0, 3, views)
        .bind_buffer(0, 4, active_layers)
        .bind_buffer(0, 5, vsm_meshlet_instances)
        .bind_buffer(0, 6, vsm_meshlet_instance_count)
        .bind_buffer(0, 7, cull_meshlets_cmd)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, ps_ctx)
        .dispatch_indirect(dispatch_cmd);

      return std::make_tuple(
        meshes,
        transforms,
        mesh_instances,
        views,
        active_layers,
        vsm_meshlet_instances,
        vsm_meshlet_instance_count,
        cull_meshlets_cmd
      );
    }
  );

  std::tie(
    self.prepared_frame.meshes_buffer,
    self.prepared_frame.transforms_world_buffer,
    self.prepared_frame.mesh_instances_buffer,
    context.views_buffer,
    active_layers_buffer,
    context.vsm_meshlet_instances_buffer,
    vsm_meshlet_instance_count_buffer,
    cull_meshlets_cmd_buffer
  ) =
    cull_meshes_pass(
      std::move(cull_meshes_cmd_buffer),
      std::move(self.prepared_frame.meshes_buffer),
      std::move(self.prepared_frame.transforms_world_buffer),
      std::move(self.prepared_frame.mesh_instances_buffer),
      std::move(context.views_buffer),
      std::move(active_layers_buffer),
      std::move(context.vsm_meshlet_instances_buffer),
      std::move(vsm_meshlet_instance_count_buffer),
      std::move(cull_meshlets_cmd_buffer)
    );

  // Occupancy pyramid over this mip's page grid. Rebuilt every mip iteration
  // because each page-table mip is an independent page grid, not a reduction of
  // the one below it.
  auto downsample_hpb_pass = vuk::make_pass(
    "rmvsm pointspot downsample hpb",
    [curr_mip = ps_ctx.curr_mip](
      vuk::CommandBuffer& cmd_list, //
      VUK_IA(vuk::eComputeSampled) page_table,
      VUK_IA(vuk::eComputeRW | vuk::eComputeSampled) hpb
    ) {
      const auto base_extent = static_cast<u32>(RMVSMContext::POINT_SPOT_PAGE_TABLE_SIZE) >> curr_mip;
      const auto level_count = RMVSMContext::POINT_SPOT_HPB_LEVEL_COUNT - static_cast<u32>(curr_mip);

      for (auto i = 0_u32; i < level_count; i++) {
        auto src = i == 0 ? hpb->mip(0) : hpb->mip(i - 1);
        auto dst = hpb->mip(i);

        auto level_extent = vuk::Extent3D{
          std::max(1u, base_extent >> i),
          std::max(1u, base_extent >> i),
          hpb->layer_count,
        };

        if (i > 0) {
          cmd_list.image_barrier(src, vuk::eComputeWrite, vuk::eComputeSampled);
        }

        cmd_list //
          .bind_compute_pipeline("rmvsm_pointspot_downsample_hpb")
          .specialize_constants(0, i == 0)
          .bind_image(0, 0, page_table)
          .bind_image(0, 1, src)
          .bind_image(0, 2, dst)
          .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(level_extent, curr_mip))
          .dispatch_invocations(level_extent.width, level_extent.height, level_extent.depth);
      }

      cmd_list.image_barrier(hpb, vuk::eComputeSampled, vuk::eComputeRW);

      return std::make_tuple(page_table, hpb);
    }
  );

  std::tie(context.page_table_attachment, context.hpb_attachment) = downsample_hpb_pass(
    std::move(context.page_table_attachment),
    std::move(context.hpb_attachment)
  );

  if (self.prepared_frame.use_mesh_shaders) {
    context.vsm_meshlet_instance_count_buffer = std::move(vsm_meshlet_instance_count_buffer);
    context.draw_cmd_buffer = std::move(cull_meshlets_cmd_buffer);
    return;
  }

  auto cull_triangles_cmd_buffer = render_context->scratch_buffer<vuk::DispatchIndirectCommand>(
    {.x = 0, .y = 1, .z = 1}
  );
  context.draw_cmd_buffer = render_context->scratch_buffer<vuk::DrawIndexedIndirectCommand>({.instanceCount = 1});

  auto cull_meshlets_pass = vuk::make_pass(
    "rmvsm pointspot cull meshlets",
    [ps_ctx](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eIndirectRead) dispatch_cmd,
      VUK_BA(vuk::eComputeRead) meshes,
      VUK_BA(vuk::eComputeRead) mesh_instances,
      VUK_BA(vuk::eComputeRead) transforms,
      VUK_BA(vuk::eComputeRead) views,
      VUK_BA(vuk::eComputeRead) vsm_meshlet_instances,
      VUK_BA(vuk::eComputeRead) vsm_meshlet_instance_count,
      VUK_IA(vuk::eComputeSampled) hpb,
      VUK_BA(vuk::eComputeWrite) visible_indices,
      VUK_BA(vuk::eComputeRW) cull_triangles_cmd
    ) {
      bind_vsm_pointspot_spec_constants(cmd_list.bind_compute_pipeline("rmvsm_pointspot_cull_meshlets"))
        .bind_buffer(0, 0, meshes)
        .bind_buffer(0, 1, mesh_instances)
        .bind_buffer(0, 2, transforms)
        .bind_buffer(0, 3, views)
        .bind_buffer(0, 4, vsm_meshlet_instances)
        .bind_buffer(0, 5, vsm_meshlet_instance_count)
        .bind_image(0, 6, hpb)
        .bind_buffer(0, 7, visible_indices)
        .bind_buffer(0, 8, cull_triangles_cmd)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, ps_ctx)
        .dispatch_indirect(dispatch_cmd);

      return std::make_tuple(
        meshes,
        mesh_instances,
        transforms,
        views,
        vsm_meshlet_instances,
        vsm_meshlet_instance_count,
        hpb,
        visible_indices,
        cull_triangles_cmd
      );
    }
  );

  std::tie(
    self.prepared_frame.meshes_buffer,
    self.prepared_frame.mesh_instances_buffer,
    self.prepared_frame.transforms_world_buffer,
    context.views_buffer,
    context.vsm_meshlet_instances_buffer,
    vsm_meshlet_instance_count_buffer,
    context.hpb_attachment,
    context.visible_indices_buffer,
    cull_triangles_cmd_buffer
  ) =
    cull_meshlets_pass(
      std::move(cull_meshlets_cmd_buffer),
      std::move(self.prepared_frame.meshes_buffer),
      std::move(self.prepared_frame.mesh_instances_buffer),
      std::move(self.prepared_frame.transforms_world_buffer),
      std::move(context.views_buffer),
      std::move(context.vsm_meshlet_instances_buffer),
      std::move(vsm_meshlet_instance_count_buffer),
      std::move(context.hpb_attachment),
      std::move(context.visible_indices_buffer),
      std::move(cull_triangles_cmd_buffer)
    );

  auto cull_triangles_pass = vuk::make_pass(
    "rmvsm pointspot cull triangles",
    [ps_ctx](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eIndirectRead) dispatch_cmd,
      VUK_BA(vuk::eComputeRead) meshes,
      VUK_BA(vuk::eComputeRead) mesh_instances,
      VUK_BA(vuk::eComputeRead) transforms,
      VUK_BA(vuk::eComputeRead) views,
      VUK_BA(vuk::eComputeRead) vsm_meshlet_instances,
      VUK_BA(vuk::eComputeRead) visible_indices,
      VUK_BA(vuk::eComputeWrite) reordered_indices,
      VUK_BA(vuk::eComputeRW) draw_cmd
    ) {
      cmd_list //
        .bind_compute_pipeline("rmvsm_pointspot_cull_triangles")
        .bind_buffer(0, 0, meshes)
        .bind_buffer(0, 1, mesh_instances)
        .bind_buffer(0, 2, transforms)
        .bind_buffer(0, 3, views)
        .bind_buffer(0, 4, vsm_meshlet_instances)
        .bind_buffer(0, 5, visible_indices)
        .bind_buffer(0, 6, reordered_indices)
        .bind_buffer(0, 7, draw_cmd)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, ps_ctx)
        .dispatch_indirect(dispatch_cmd);

      return std::make_tuple(
        meshes,
        mesh_instances,
        transforms,
        views,
        vsm_meshlet_instances,
        visible_indices,
        reordered_indices,
        draw_cmd
      );
    }
  );

  std::tie(
    self.prepared_frame.meshes_buffer,
    self.prepared_frame.mesh_instances_buffer,
    self.prepared_frame.transforms_world_buffer,
    context.views_buffer,
    context.vsm_meshlet_instances_buffer,
    context.visible_indices_buffer,
    context.reordered_indices_buffer,
    context.draw_cmd_buffer
  ) =
    cull_triangles_pass(
      std::move(cull_triangles_cmd_buffer),
      std::move(self.prepared_frame.meshes_buffer),
      std::move(self.prepared_frame.mesh_instances_buffer),
      std::move(self.prepared_frame.transforms_world_buffer),
      std::move(context.views_buffer),
      std::move(context.vsm_meshlet_instances_buffer),
      std::move(context.visible_indices_buffer),
      std::move(context.reordered_indices_buffer),
      std::move(context.draw_cmd_buffer)
    );
}

} // namespace ox
