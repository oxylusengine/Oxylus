#include <vuk/runtime/CommandBuffer.hpp>

#include "Core/App.hpp"
#include "Memory/Stack.hpp"
#include "Render/RendererInstance.hpp"
#include "Render/Utils/VukCommon.hpp"

namespace ox {
static constexpr auto sampler_min_clamp_reduction_mode = VkSamplerReductionModeCreateInfo{
  .sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO,
  .pNext = nullptr,
  .reductionMode = VK_SAMPLER_REDUCTION_MODE_MIN,
};

static constexpr auto hiz_sampler = vuk::SamplerCreateInfo{
  .pNext = &sampler_min_clamp_reduction_mode,
  .magFilter = vuk::Filter::eLinear,
  .minFilter = vuk::Filter::eLinear,
  .mipmapMode = vuk::SamplerMipmapMode::eNearest,
  .addressModeU = vuk::SamplerAddressMode::eClampToEdge,
  .addressModeV = vuk::SamplerAddressMode::eClampToEdge,
};

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

      cmd_list //
        .bind_compute_pipeline("hiz")
        .bind_sampler(0, 0, hiz_sampler);

      for (auto i = 0_u32; i < mip_count; i++) {
        auto mip_width = std::max(1_u32, extent.width >> i);
        auto mip_height = std::max(1_u32, extent.height >> i);

        auto mip = dst->mip(i);
        if (i == 0) {
          cmd_list.bind_image(0, 1, src);
        } else {
          auto prev_mip = dst->mip(i - 1);
          cmd_list.image_barrier(prev_mip, vuk::eComputeWrite, vuk::eComputeSampled);
          cmd_list.bind_image(0, 1, prev_mip);
        }

        cmd_list.bind_image(0, 2, mip);
        cmd_list.push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(mip_width, mip_height, i));
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

auto RendererInstance::cull_for_visbuffer(this RendererInstance& self, MainGeometryContext& context) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  if (!context.late) {
    // TODO: Slang is fucking up as usual when using this as push constants BDA
    // auto debug_drawer_buffer_ptr = context.debug_drawer_buffer.node ? context.debug_drawer_buffer->device_address :
    // 0;
    auto cull_meshes_pass = vuk::make_pass(
      "vis cull meshes",
      [mesh_instance_count = self.prepared_frame.mesh_instance_count](
        vuk::CommandBuffer& cmd_list,
        VUK_BA(vuk::eComputeUniformRead) camera,
        VUK_BA(vuk::eComputeRead) meshes,
        VUK_BA(vuk::eComputeRead) transforms,
        VUK_BA(vuk::eComputeRW) mesh_instances,
        VUK_BA(vuk::eComputeRW) meshlet_instances,
        VUK_BA(vuk::eComputeRW) visibility
      ) {
        cmd_list //
          .bind_compute_pipeline("vis_cull_meshes")
          .bind_buffer(0, 0, camera)
          .bind_buffer(0, 1, meshes)
          .bind_buffer(0, 2, transforms)
          .bind_buffer(0, 3, mesh_instances)
          .bind_buffer(0, 4, meshlet_instances)
          .bind_buffer(0, 5, visibility)
          .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, mesh_instance_count)
          .dispatch_invocations(mesh_instance_count);

        return std::make_tuple(camera, meshes, transforms, mesh_instances, meshlet_instances, visibility);
      }
    );

    context.visibility_buffer = self.renderer.vk_context->scratch_buffer<GPU::MeshletInstanceVisibility>({});
    std::tie(
      self.prepared_frame.camera_buffer,
      self.prepared_frame.meshes_buffer,
      self.prepared_frame.transforms_buffer,
      self.prepared_frame.mesh_instances_buffer,
      self.prepared_frame.meshlet_instances_buffer,
      context.visibility_buffer
    ) =
      cull_meshes_pass(
        std::move(self.prepared_frame.camera_buffer),
        std::move(self.prepared_frame.meshes_buffer),
        std::move(self.prepared_frame.transforms_buffer),
        std::move(self.prepared_frame.mesh_instances_buffer),
        std::move(self.prepared_frame.meshlet_instances_buffer),
        std::move(context.visibility_buffer)
      );

    auto generate_cull_commands_pass = vuk::make_pass(
      "vis generate cull commands",
      [](
        vuk::CommandBuffer& cmd_list, //
        VUK_BA(vuk::eComputeRead) visibility,
        VUK_BA(vuk::eComputeRW) cull_meshlets_cmd
      ) {
        cmd_list //
          .bind_compute_pipeline("vis_generate_cull_commands")
          .bind_buffer(0, 5, visibility)
          .bind_buffer(0, 6, cull_meshlets_cmd)
          .dispatch(1);

        return std::make_tuple(visibility, cull_meshlets_cmd);
      }
    );

    context.cull_meshlets_cmd_buffer = self.renderer.vk_context->scratch_buffer<vuk::DispatchIndirectCommand>(
      {.x = 0, .y = 1, .z = 1}
    );
    std::tie(context.visibility_buffer, context.cull_meshlets_cmd_buffer) = generate_cull_commands_pass(
      std::move(context.visibility_buffer),
      std::move(context.cull_meshlets_cmd_buffer)
    );
  }

  auto cull_meshlets_pass = vuk::make_pass(
    stack.format("vis cull meshlets {}", context.late ? "late" : "early"),
    [late = context.late, occlusion_cull_ = self.occlusion_cull](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eIndirectRead) dispatch_cmd,
      VUK_IA(vuk::eComputeSampled) hiz,
      VUK_BA(vuk::eComputeUniformRead) camera,
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
        .bind_compute_pipeline("vis_cull_meshlets")
        .bind_image(0, 0, hiz)
        .bind_sampler(0, 1, hiz_sampler)
        .bind_buffer(0, 2, camera)
        .bind_buffer(0, 3, meshes)
        .bind_buffer(0, 4, mesh_instances)
        .bind_buffer(0, 5, meshlet_instances)
        .bind_buffer(0, 6, transforms)
        .bind_buffer(0, 7, visibility)
        .bind_buffer(0, 8, visible_meshlet_instances_indices)
        .bind_buffer(0, 9, meshlet_instance_visibility_mask)
        .bind_buffer(0, 10, cull_triangles_cmd)
        .specialize_constants(0, late)
        .specialize_constants(1, occlusion_cull_)
        .dispatch_indirect(dispatch_cmd);

      return std::make_tuple(
        dispatch_cmd,
        hiz,
        camera,
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

  auto cull_triangles_cmd_buffer = self.renderer.vk_context->scratch_buffer<vuk::DispatchIndirectCommand>({.x = 0, .y = 1, .z = 1});

  std::tie(
    context.cull_meshlets_cmd_buffer,
    context.hiz_attachment,
    self.prepared_frame.camera_buffer,
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
      std::move(self.prepared_frame.camera_buffer),
      std::move(self.prepared_frame.meshes_buffer),
      std::move(self.prepared_frame.mesh_instances_buffer),
      std::move(self.prepared_frame.meshlet_instances_buffer),
      std::move(self.prepared_frame.transforms_buffer),
      std::move(context.visibility_buffer),
      std::move(self.prepared_frame.visible_meshlet_instances_indices_buffer),
      std::move(self.prepared_frame.meshlet_instance_visibility_mask_buffer),
      std::move(cull_triangles_cmd_buffer)
    );

  auto cull_triangles_pass = vuk::make_pass(
    stack.format("vis cull triangles {}", context.late ? "late" : "early"),
    [late = context.late](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eIndirectRead) cull_triangles_cmd,
      VUK_BA(vuk::eComputeUniformRead) camera,
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
        .bind_compute_pipeline("vis_cull_triangles")
        .bind_buffer(0, 0, camera)
        .bind_buffer(0, 1, meshes)
        .bind_buffer(0, 2, mesh_instances)
        .bind_buffer(0, 3, meshlet_instances)
        .bind_buffer(0, 4, visibility)
        .bind_buffer(0, 5, transforms)
        .bind_buffer(0, 6, visible_meshlet_instances_indices)
        .bind_buffer(0, 7, reordered_indices)
        .bind_buffer(0, 8, draw_indexed_cmd)
        .specialize_constants(0, late)
        .dispatch_indirect(cull_triangles_cmd);

      return std::make_tuple(
        camera,
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

  context.draw_geometry_cmd_buffer = self.renderer.vk_context->scratch_buffer<vuk::DrawIndexedIndirectCommand>({.instanceCount = 1});
  std::tie(
    self.prepared_frame.camera_buffer,
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
      std::move(self.prepared_frame.camera_buffer),
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

auto RendererInstance::cull_for_shadowmap(
  this RendererInstance& self, ShadowGeometryContext& context, glm::mat4& projection_view
) -> void {
  ZoneScoped;

  auto cull_meshes_pass = vuk::make_pass(
    "sm cull meshes",
    [mesh_instance_count = self.prepared_frame.mesh_instance_count, projection_view](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eComputeRead) meshes,
      VUK_BA(vuk::eComputeRead) transforms,
      VUK_BA(vuk::eComputeRW) mesh_instances,
      VUK_BA(vuk::eComputeRW) meshlet_instances,
      VUK_BA(vuk::eComputeRW) visibility
    ) {
      cmd_list //
        .bind_compute_pipeline("shadowmap_cull_meshes")
        .bind_buffer(0, 0, meshes)
        .bind_buffer(0, 1, transforms)
        .bind_buffer(0, 2, mesh_instances)
        .bind_buffer(0, 3, meshlet_instances)
        .bind_buffer(0, 4, visibility)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(projection_view, mesh_instance_count))
        .dispatch_invocations(mesh_instance_count);

      return std::make_tuple(meshes, transforms, mesh_instances, meshlet_instances, visibility);
    }
  );

  context.visibility_buffer = self.renderer.vk_context->scratch_buffer<u32>({});
  std::tie(
    self.prepared_frame.meshes_buffer,
    self.prepared_frame.transforms_buffer,
    self.prepared_frame.mesh_instances_buffer,
    self.prepared_frame.meshlet_instances_buffer,
    context.visibility_buffer
  ) =
    cull_meshes_pass(
      std::move(self.prepared_frame.meshes_buffer),
      std::move(self.prepared_frame.transforms_buffer),
      std::move(self.prepared_frame.mesh_instances_buffer),
      std::move(self.prepared_frame.meshlet_instances_buffer),
      std::move(context.visibility_buffer)
    );

  auto generate_cull_commands_pass = vuk::make_pass(
    "sm generate cull commands",
    [](
      vuk::CommandBuffer& cmd_list, //
      VUK_BA(vuk::eComputeRead) visibility,
      VUK_BA(vuk::eComputeRW) cull_meshlets_cmd
    ) {
      cmd_list //
        .bind_compute_pipeline("shadowmap_generate_cull_commands")
        .bind_buffer(0, 4, visibility)
        .bind_buffer(0, 5, cull_meshlets_cmd)
        .dispatch(1);

      return std::make_tuple(visibility, cull_meshlets_cmd);
    }
  );

  context.cull_meshlets_cmd_buffer = self.renderer.vk_context->scratch_buffer<vuk::DispatchIndirectCommand>({.x = 0, .y = 1, .z = 1});

  std::tie(context.visibility_buffer, context.cull_meshlets_cmd_buffer) = generate_cull_commands_pass(
    std::move(context.visibility_buffer),
    std::move(context.cull_meshlets_cmd_buffer)
  );

  auto cull_meshlets_pass = vuk::make_pass(
    "shadowmap cull meshlets",
    [projection_view](
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
        .bind_compute_pipeline("shadowmap_cull_meshlets")
        .bind_buffer(0, 0, meshes)
        .bind_buffer(0, 1, mesh_instances)
        .bind_buffer(0, 2, meshlet_instances)
        .bind_buffer(0, 3, transforms)
        .bind_buffer(0, 4, visibility)
        .bind_buffer(0, 5, visible_meshlet_instances_indices)
        .bind_buffer(0, 6, cull_triangles_cmd)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, projection_view)
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

  auto cull_triangles_cmd_buffer = self.renderer.vk_context->scratch_buffer<vuk::DispatchIndirectCommand>({.x = 0, .y = 1, .z = 1});

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

  auto cull_triangles_pass = vuk::make_pass(
    "sm cull triangles",
    [projection_view](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eIndirectRead) cull_triangles_cmd,
      VUK_BA(vuk::eComputeRead) meshes,
      VUK_BA(vuk::eComputeRead) mesh_instances,
      VUK_BA(vuk::eComputeRead) meshlet_instances,
      VUK_BA(vuk::eComputeRead) transforms,
      VUK_BA(vuk::eComputeRead) visible_meshlet_instances_indices,
      VUK_BA(vuk::eComputeRW) reordered_indices,
      VUK_BA(vuk::eComputeRW) draw_indexed_cmd
    ) {
      cmd_list //
        .bind_compute_pipeline("shadowmap_cull_triangles")
        .bind_buffer(0, 0, meshes)
        .bind_buffer(0, 1, mesh_instances)
        .bind_buffer(0, 2, meshlet_instances)
        .bind_buffer(0, 3, transforms)
        .bind_buffer(0, 4, visible_meshlet_instances_indices)
        .bind_buffer(0, 5, reordered_indices)
        .bind_buffer(0, 6, draw_indexed_cmd)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, projection_view)
        .dispatch_indirect(cull_triangles_cmd);

      return std::make_tuple(
        meshes,
        mesh_instances,
        meshlet_instances,
        transforms,
        visible_meshlet_instances_indices,
        reordered_indices,
        draw_indexed_cmd
      );
    }
  );

  context.draw_geometry_cmd_buffer = self.renderer.vk_context->scratch_buffer<vuk::DrawIndexedIndirectCommand>({.instanceCount = 1});
  std::tie(
    self.prepared_frame.meshes_buffer,
    self.prepared_frame.mesh_instances_buffer,
    self.prepared_frame.meshlet_instances_buffer,
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
      std::move(self.prepared_frame.transforms_buffer),
      std::move(self.prepared_frame.visible_meshlet_instances_indices_buffer),
      std::move(self.prepared_frame.reordered_indices_buffer),
      std::move(context.draw_geometry_cmd_buffer)
    );
}

} // namespace ox
