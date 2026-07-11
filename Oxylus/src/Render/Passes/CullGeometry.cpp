#include <vuk/runtime/CommandBuffer.hpp>

#include "Core/App.hpp"
#include "Memory/Stack.hpp"
#include "Render/RendererInstance.hpp"
#include "Render/Utils/VukCommon.hpp"

namespace ox {
auto RendererInstance::generate_hiz(this RendererInstance&, MainGeometryContext& context) -> void {
  ZoneScoped;

  auto hiz_generate_slow_pass = vuk::make_pass(
    "hiz generate slow",
    [](
      vuk::CommandBuffer& cmd_list, //
      VUK_IA(vuk::eComputeSampled) src,
      VUK_IA(vuk::eComputeRW) dst
    ) {
      auto extent = dst->extent;
      auto mip_count = dst->level_count;

      cmd_list.bind_compute_pipeline("hiz");

      for (auto i = 0_u32; i < mip_count; i++) {
        auto mip_width = std::max(1_u32, extent.width >> i);
        auto mip_height = std::max(1_u32, extent.height >> i);

        auto mip = dst->mip(i);
        if (i == 0) {
          cmd_list.bind_image(0, 0, src);
        } else {
          auto prev_mip = dst->mip(i - 1);
          cmd_list.image_barrier(prev_mip, vuk::eComputeWrite, vuk::eComputeSampled);
          cmd_list.bind_image(0, 0, prev_mip);
        }

        cmd_list.bind_image(0, 1, mip);
        cmd_list.push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(mip_width, mip_height));
        cmd_list.dispatch_invocations(mip_width, mip_height);
      }

      cmd_list.image_barrier(dst, vuk::eComputeSampled, vuk::eComputeRW);

      return std::make_tuple(src, dst);
    }
  );

  std::tie(context.depth_attachment, context.hiz_attachment) = hiz_generate_slow_pass(
    std::move(context.depth_attachment),
    std::move(context.hiz_attachment)
  );
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
      self.prepared_frame.transforms_buffer,
      self.prepared_frame.mesh_instances_buffer,
      self.prepared_frame.meshlet_instances_buffer,
      context.visibility_buffer,
      context.cull_meshlets_cmd_buffer
    ) =
      cull_meshes_pass(
        std::move(self.prepared_frame.meshes_buffer),
        std::move(self.prepared_frame.transforms_buffer),
        std::move(self.prepared_frame.mesh_instances_buffer),
        std::move(self.prepared_frame.meshlet_instances_buffer),
        std::move(context.visibility_buffer),
        std::move(context.cull_meshlets_cmd_buffer)
      );
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
      self.prepared_frame.transforms_buffer,
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
        std::move(self.prepared_frame.transforms_buffer),
        std::move(context.visibility_buffer),
        std::move(self.prepared_frame.visible_meshlet_instances_indices_buffer),
        std::move(self.prepared_frame.meshlet_instance_visibility_mask_buffer),
        std::move(cull_triangles_cmd_buffer)
      );
  } else if (context.use_hpb) {
    auto cull_meshlets_pass = vuk::make_pass(
      "rmvsm cull meshlets",
      [cull_camera, clipmap_index = context.vsm_layer_index, page_offset = context.vsm_page_offset](
        vuk::CommandBuffer& cmd_list,
        VUK_BA(vuk::eIndirectRead) dispatch_cmd,
        VUK_BA(vuk::eComputeRead) meshes,
        VUK_BA(vuk::eComputeRead) mesh_instances,
        VUK_BA(vuk::eComputeRead) meshlet_instances,
        VUK_BA(vuk::eComputeRead) transforms,
        VUK_BA(vuk::eComputeRW) visibility,
        VUK_BA(vuk::eComputeRW) visible_meshlet_instances_indices,
        VUK_BA(vuk::eComputeRW) cull_triangles_cmd,
        VUK_IA(vuk::eComputeSampled) hpb
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
          .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(cull_camera, clipmap_index, page_offset))
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
          hpb
        );
      }
    );

    std::tie(
      context.cull_meshlets_cmd_buffer,
      self.prepared_frame.meshes_buffer,
      self.prepared_frame.mesh_instances_buffer,
      self.prepared_frame.meshlet_instances_buffer,
      self.prepared_frame.transforms_buffer,
      context.visibility_buffer,
      self.prepared_frame.visible_meshlet_instances_indices_buffer,
      cull_triangles_cmd_buffer,
      context.hpb_attachment
    ) =
      cull_meshlets_pass(
        std::move(context.cull_meshlets_cmd_buffer),
        std::move(self.prepared_frame.meshes_buffer),
        std::move(self.prepared_frame.mesh_instances_buffer),
        std::move(self.prepared_frame.meshlet_instances_buffer),
        std::move(self.prepared_frame.transforms_buffer),
        std::move(context.visibility_buffer),
        std::move(self.prepared_frame.visible_meshlet_instances_indices_buffer),
        std::move(cull_triangles_cmd_buffer),
        std::move(context.hpb_attachment)
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
      self.prepared_frame.transforms_buffer,
      context.visibility_buffer,
      self.prepared_frame.visible_meshlet_instances_indices_buffer,
      cull_triangles_cmd_buffer
    ) =
      cull_meshlets_pass(
        std::move(context.cull_meshlets_cmd_buffer),
        std::move(self.prepared_frame.meshes_buffer),
        std::move(self.prepared_frame.mesh_instances_buffer),
        std::move(self.prepared_frame.meshlet_instances_buffer),
        std::move(self.prepared_frame.transforms_buffer),
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
    self.prepared_frame.transforms_buffer,
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
      std::move(self.prepared_frame.transforms_buffer),
      std::move(self.prepared_frame.visible_meshlet_instances_indices_buffer),
      std::move(self.prepared_frame.reordered_indices_buffer),
      std::move(context.draw_geometry_cmd_buffer)
    );
}

} // namespace ox
