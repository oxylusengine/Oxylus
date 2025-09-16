#include "Memory/Stack.hpp"
#include "Render/RendererPipeline.hpp"
#include "Render/Utils/VukCommon.hpp"

namespace ox {
static constexpr auto sampler_min_clamp_reduction_mode = VkSamplerReductionModeCreateInfo{
  .sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO,
  .pNext = nullptr,
  .reductionMode = VK_SAMPLER_REDUCTION_MODE_MIN,
};
static constexpr auto hiz_sampler_info = vuk::SamplerCreateInfo{
  .pNext = &sampler_min_clamp_reduction_mode,
  .magFilter = vuk::Filter::eLinear,
  .minFilter = vuk::Filter::eLinear,
  .mipmapMode = vuk::SamplerMipmapMode::eNearest,
  .addressModeU = vuk::SamplerAddressMode::eClampToEdge,
  .addressModeV = vuk::SamplerAddressMode::eClampToEdge,
};

auto cull_meshes(
  GPU::CullFlags cull_flags,
  u32 mesh_instance_count,
  VkContext& ctx,
  vuk::Value<vuk::Buffer>& meshes_buffer,
  vuk::Value<vuk::Buffer>& mesh_instances_buffer,
  vuk::Value<vuk::Buffer>& meshlet_instances_buffer,
  vuk::Value<vuk::Buffer>& visible_meshlet_instances_count_buffer,
  vuk::Value<vuk::Buffer>& transforms_buffer,
  vuk::Value<vuk::ImageAttachment>& hiz_attachment,
  vuk::Value<vuk::Buffer>& camera_buffer
) -> vuk::Value<vuk::Buffer> {
  ZoneScoped;

  auto vis_cull_meshes_pass = vuk::make_pass(
    "vis cull meshes",
    [cull_flags, mesh_instance_count](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eComputeRead) camera,
      VUK_BA(vuk::eComputeRead) meshes,
      VUK_BA(vuk::eComputeRead) transforms,
      VUK_IA(vuk::eComputeSampled) hiz,
      VUK_BA(vuk::eComputeRW) mesh_instances,
      VUK_BA(vuk::eComputeRW) meshlet_instances,
      VUK_BA(vuk::eComputeRW) visible_meshlet_instances_count
    ) {
      cmd_list //
        .bind_compute_pipeline("cull_meshes")
        .bind_buffer(0, 0, camera)
        .bind_buffer(0, 1, meshes)
        .bind_buffer(0, 2, transforms)
        .bind_image(0, 3, hiz)
        .bind_sampler(0, 4, hiz_sampler_info)
        .bind_buffer(0, 5, mesh_instances)
        .bind_buffer(0, 6, meshlet_instances)
        .bind_buffer(0, 7, visible_meshlet_instances_count)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(mesh_instance_count, cull_flags))
        .dispatch_invocations(mesh_instance_count);

      return std::make_tuple(
        camera,
        meshes,
        transforms,
        hiz,
        mesh_instances,
        meshlet_instances,
        visible_meshlet_instances_count
      );
    }
  );

  std::tie(
    camera_buffer,
    meshes_buffer,
    transforms_buffer,
    hiz_attachment,
    mesh_instances_buffer,
    meshlet_instances_buffer,
    visible_meshlet_instances_count_buffer
  ) =
    vis_cull_meshes_pass(
      std::move(camera_buffer),
      std::move(meshes_buffer),
      std::move(transforms_buffer),
      std::move(hiz_attachment),
      std::move(mesh_instances_buffer),
      std::move(meshlet_instances_buffer),
      std::move(visible_meshlet_instances_count_buffer)
    );

  auto generate_cull_commands_pass = vuk::make_pass(
    "generate cull commands",
    [](
      vuk::CommandBuffer& cmd_list, //
      VUK_BA(vuk::eComputeRead) visible_meshlet_instances_count,
      VUK_BA(vuk::eComputeRW) cull_meshlets_cmd
    ) {
      cmd_list //
        .bind_compute_pipeline("generate_cull_commands")
        .bind_buffer(0, 0, visible_meshlet_instances_count)
        .bind_buffer(0, 1, cull_meshlets_cmd)
        .dispatch(1);

      return std::make_tuple(visible_meshlet_instances_count, cull_meshlets_cmd);
    }
  );

  auto cull_meshlets_cmd_buffer = ctx.scratch_buffer<vuk::DispatchIndirectCommand>({.x = 0, .y = 1, .z = 1});
  std::tie(visible_meshlet_instances_count_buffer, cull_meshlets_cmd_buffer) = generate_cull_commands_pass(
    std::move(visible_meshlet_instances_count_buffer),
    std::move(cull_meshlets_cmd_buffer)
  );

  return cull_meshlets_cmd_buffer;
}

auto cull_meshlets(
  bool late,
  GPU::CullFlags cull_flags,
  VkContext& ctx,
  vuk::Value<vuk::ImageAttachment>& hiz_attachment,
  vuk::Value<vuk::Buffer>& cull_meshlets_cmd_buffer,
  vuk::Value<vuk::Buffer>& visible_meshlet_instances_count_buffer,
  vuk::Value<vuk::Buffer>& visible_meshlet_instances_indices_buffer,
  vuk::Value<vuk::Buffer>& meshlet_instance_visibility_mask_buffer,
  vuk::Value<vuk::Buffer>& reordered_indices_buffer,
  vuk::Value<vuk::Buffer>& meshes_buffer,
  vuk::Value<vuk::Buffer>& mesh_instances_buffer,
  vuk::Value<vuk::Buffer>& meshlet_instances_buffer,
  vuk::Value<vuk::Buffer>& transforms_buffer,
  vuk::Value<vuk::Buffer>& camera_buffer
) -> vuk::Value<vuk::Buffer> {
  ZoneScoped;
  memory::ScopedStack stack;

  //  ── CULL MESHLETS ───────────────────────────────────────────────────
  auto vis_cull_meshlets_pass = vuk::make_pass(
    stack.format("vis cull meshlets {}", late ? "late" : "early"),
    [late, cull_flags](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eIndirectRead) dispatch_cmd,
      VUK_BA(vuk::eComputeRead) camera,
      VUK_BA(vuk::eComputeRead) meshlet_instances,
      VUK_BA(vuk::eComputeRead) mesh_instances,
      VUK_BA(vuk::eComputeRead) meshes,
      VUK_BA(vuk::eComputeRead) transforms,
      VUK_IA(vuk::eComputeSampled) hiz,
      VUK_BA(vuk::eComputeRW) visible_meshlet_instances_count,
      VUK_BA(vuk::eComputeRW) visible_meshlet_instances_indices,
      VUK_BA(vuk::eComputeRW) meshlet_instance_visibility_mask,
      VUK_BA(vuk::eComputeRW) cull_triangles_cmd
    ) {
      cmd_list //
        .bind_compute_pipeline("cull_meshlets")
        .bind_buffer(0, 0, camera)
        .bind_buffer(0, 1, meshlet_instances)
        .bind_buffer(0, 2, mesh_instances)
        .bind_buffer(0, 3, meshes)
        .bind_buffer(0, 4, transforms)
        .bind_image(0, 5, hiz)
        .bind_sampler(0, 6, hiz_sampler_info)
        .bind_buffer(0, 7, visible_meshlet_instances_count)
        .bind_buffer(0, 8, visible_meshlet_instances_indices)
        .bind_buffer(0, 9, meshlet_instance_visibility_mask)
        .bind_buffer(0, 10, cull_triangles_cmd)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, cull_flags)
        .specialize_constants(0, late)
        .dispatch_indirect(dispatch_cmd);

      return std::make_tuple(
        dispatch_cmd,
        camera,
        meshlet_instances,
        mesh_instances,
        meshes,
        transforms,
        hiz,
        visible_meshlet_instances_count,
        visible_meshlet_instances_indices,
        meshlet_instance_visibility_mask,
        cull_triangles_cmd
      );
    }
  );

  auto cull_triangles_cmd_buffer = ctx.scratch_buffer<vuk::DispatchIndirectCommand>({.x = 0, .y = 1, .z = 1});

  std::tie(
    cull_meshlets_cmd_buffer,
    camera_buffer,
    meshlet_instances_buffer,
    mesh_instances_buffer,
    meshes_buffer,
    transforms_buffer,
    hiz_attachment,
    visible_meshlet_instances_count_buffer,
    visible_meshlet_instances_indices_buffer,
    meshlet_instance_visibility_mask_buffer,
    cull_triangles_cmd_buffer
  ) =
    vis_cull_meshlets_pass(
      std::move(cull_meshlets_cmd_buffer),
      std::move(camera_buffer),
      std::move(meshlet_instances_buffer),
      std::move(mesh_instances_buffer),
      std::move(meshes_buffer),
      std::move(transforms_buffer),
      std::move(hiz_attachment),
      std::move(visible_meshlet_instances_count_buffer),
      std::move(visible_meshlet_instances_indices_buffer),
      std::move(meshlet_instance_visibility_mask_buffer),
      std::move(cull_triangles_cmd_buffer)
    );

  //  ── CULL TRIANGLES ──────────────────────────────────────────────────
  auto vis_cull_triangles_pass = vuk::make_pass(
    stack.format("vis cull triangles {}", late ? "late" : "early"),
    [late, cull_flags](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eIndirectRead) cull_triangles_cmd,
      VUK_BA(vuk::eComputeRead) camera,
      VUK_BA(vuk::eComputeRead) visible_meshlet_instances_count,
      VUK_BA(vuk::eComputeRead) visible_meshlet_instances_indices,
      VUK_BA(vuk::eComputeRead) meshlet_instances,
      VUK_BA(vuk::eComputeRead) mesh_instances,
      VUK_BA(vuk::eComputeRead) meshes,
      VUK_BA(vuk::eComputeRead) transforms,
      VUK_BA(vuk::eComputeRW) draw_indexed_cmd,
      VUK_BA(vuk::eComputeWrite) reordered_indices
    ) {
      cmd_list //
        .bind_compute_pipeline("cull_triangles")
        .bind_buffer(0, 0, camera)
        .bind_buffer(0, 1, visible_meshlet_instances_count)
        .bind_buffer(0, 2, visible_meshlet_instances_indices)
        .bind_buffer(0, 3, meshlet_instances)
        .bind_buffer(0, 4, mesh_instances)
        .bind_buffer(0, 5, meshes)
        .bind_buffer(0, 6, transforms)
        .bind_buffer(0, 7, draw_indexed_cmd)
        .bind_buffer(0, 8, reordered_indices)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, cull_flags)
        .specialize_constants(0, late)
        .dispatch_indirect(cull_triangles_cmd);

      return std::make_tuple(
        camera,
        visible_meshlet_instances_count,
        visible_meshlet_instances_indices,
        meshlet_instances,
        mesh_instances,
        meshes,
        transforms,
        draw_indexed_cmd,
        reordered_indices
      );
    }
  );

  auto draw_command_buffer = ctx.scratch_buffer<vuk::DrawIndexedIndirectCommand>({.instanceCount = 1});

  std::tie(
    camera_buffer,
    visible_meshlet_instances_count_buffer,
    visible_meshlet_instances_indices_buffer,
    meshlet_instances_buffer,
    mesh_instances_buffer,
    meshes_buffer,
    transforms_buffer,
    draw_command_buffer,
    reordered_indices_buffer
  ) =
    vis_cull_triangles_pass(
      std::move(cull_triangles_cmd_buffer),
      std::move(camera_buffer),
      std::move(visible_meshlet_instances_count_buffer),
      std::move(visible_meshlet_instances_indices_buffer),
      std::move(meshlet_instances_buffer),
      std::move(mesh_instances_buffer),
      std::move(meshes_buffer),
      std::move(transforms_buffer),
      std::move(draw_command_buffer),
      std::move(reordered_indices_buffer)
    );

  return draw_command_buffer;
}

auto clear_visbuffer(
  vuk::Value<vuk::ImageAttachment>& visbuffer_attachment, vuk::Value<vuk::ImageAttachment>& overdraw_attachment
) -> void {
  ZoneScoped;

  auto vis_clear_pass = vuk::make_pass(
    "vis_clear",
    [](
      vuk::CommandBuffer& cmd_list, //
      VUK_IA(vuk::eComputeWrite) visbuffer,
      VUK_IA(vuk::eComputeWrite) overdraw
    ) {
      cmd_list.bind_compute_pipeline("visbuffer_clear")
        .bind_image(0, 0, visbuffer)
        .bind_image(0, 1, overdraw)
        .push_constants(
          vuk::ShaderStageFlagBits::eCompute,
          0,
          PushConstants(glm::uvec2(visbuffer->extent.width, visbuffer->extent.height))
        )
        .dispatch_invocations_per_pixel(visbuffer);
      return std::make_tuple(visbuffer, overdraw);
    }
  );

  std::tie(visbuffer_attachment, overdraw_attachment) = vis_clear_pass(
    std::move(visbuffer_attachment),
    std::move(overdraw_attachment)
  );
}

auto draw_visbuffer(
  bool late,
  vuk::PersistentDescriptorSet& descriptor_set,
  vuk::Value<vuk::ImageAttachment>& depth_attachment,
  vuk::Value<vuk::ImageAttachment>& visbuffer_attachment,
  vuk::Value<vuk::ImageAttachment>& overdraw_attachment,
  vuk::Value<vuk::Buffer>& draw_command_buffer,
  vuk::Value<vuk::Buffer>& reordered_indices_buffer,
  vuk::Value<vuk::Buffer>& meshes_buffer,
  vuk::Value<vuk::Buffer>& mesh_instances_buffer,
  vuk::Value<vuk::Buffer>& meshlet_instances_buffer,
  vuk::Value<vuk::Buffer>& transforms_buffer,
  vuk::Value<vuk::Buffer>& materials_buffer,
  vuk::Value<vuk::Buffer>& camera_buffer
) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  auto vis_encode_pass = vuk::make_pass(
    stack.format("vis encode {}", late ? "late" : "early"),
    [&descriptor_set](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eIndirectRead) triangle_indirect,
      VUK_BA(vuk::eIndexRead) index_buffer,
      VUK_BA(vuk::eVertexRead) camera,
      VUK_BA(vuk::eVertexRead) meshlet_instances,
      VUK_BA(vuk::eVertexRead) mesh_instances,
      VUK_BA(vuk::eVertexRead) meshes,
      VUK_BA(vuk::eVertexRead) transforms,
      VUK_BA(vuk::eFragmentRead) materials,
      VUK_IA(vuk::eColorRW) visbuffer,
      VUK_IA(vuk::eDepthStencilRW) depth,
      VUK_IA(vuk::eFragmentRW) overdraw
    ) {
      cmd_list //
        .bind_graphics_pipeline("visbuffer_encode")
        .set_rasterization({.cullMode = vuk::CullModeFlagBits::eBack})
        .set_depth_stencil(
          {.depthTestEnable = true, .depthWriteEnable = true, .depthCompareOp = vuk::CompareOp::eGreaterOrEqual}
        )
        .set_color_blend(visbuffer, vuk::BlendPreset::eOff)
        .set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
        .set_viewport(0, vuk::Rect2D::framebuffer())
        .set_scissor(0, vuk::Rect2D::framebuffer())
        .bind_persistent(1, descriptor_set)
        .bind_buffer(0, 0, camera)
        .bind_buffer(0, 1, meshlet_instances)
        .bind_buffer(0, 2, mesh_instances)
        .bind_buffer(0, 3, meshes)
        .bind_buffer(0, 4, transforms)
        .bind_buffer(0, 5, materials)
        .bind_image(0, 6, overdraw)
        .bind_index_buffer(index_buffer, vuk::IndexType::eUint32)
        .draw_indexed_indirect(1, triangle_indirect);

      return std::make_tuple(
        index_buffer,
        camera,
        meshlet_instances,
        mesh_instances,
        meshes,
        transforms,
        materials,
        visbuffer,
        depth,
        overdraw
      );
    }
  );

  std::tie(
    reordered_indices_buffer,
    camera_buffer,
    meshlet_instances_buffer,
    mesh_instances_buffer,
    meshes_buffer,
    transforms_buffer,
    materials_buffer,
    visbuffer_attachment,
    depth_attachment,
    overdraw_attachment
  ) =
    vis_encode_pass(
      std::move(draw_command_buffer),
      std::move(reordered_indices_buffer),
      std::move(camera_buffer),
      std::move(meshlet_instances_buffer),
      std::move(mesh_instances_buffer),
      std::move(meshes_buffer),
      std::move(transforms_buffer),
      std::move(materials_buffer),
      std::move(visbuffer_attachment),
      std::move(depth_attachment),
      std::move(overdraw_attachment)
    );
}

auto draw_visbuffer_depth_only(
  bool late,
  vuk::Value<vuk::ImageAttachment>& depth_attachment,
  vuk::Value<vuk::Buffer>& draw_command_buffer,
  vuk::Value<vuk::Buffer>& reordered_indices_buffer,
  vuk::Value<vuk::Buffer>& meshes_buffer,
  vuk::Value<vuk::Buffer>& mesh_instances_buffer,
  vuk::Value<vuk::Buffer>& meshlet_instances_buffer,
  vuk::Value<vuk::Buffer>& transforms_buffer,
  vuk::Value<vuk::Buffer>& camera_buffer
) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  auto vis_encode_depth_pass = vuk::make_pass(
    stack.format("vis encode depth {}", late ? "late" : "early"),
    [](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eIndirectRead) triangle_indirect,
      VUK_BA(vuk::eIndexRead) index_buffer,
      VUK_BA(vuk::eVertexRead) camera,
      VUK_BA(vuk::eVertexRead) meshlet_instances,
      VUK_BA(vuk::eVertexRead) mesh_instances,
      VUK_BA(vuk::eVertexRead) meshes,
      VUK_BA(vuk::eVertexRead) transforms,
      VUK_IA(vuk::eDepthStencilRW) depth
    ) {
      cmd_list //
        .bind_graphics_pipeline("visbuffer_encode_depth")
        .set_rasterization({.cullMode = vuk::CullModeFlagBits::eBack})
        .set_depth_stencil(
          {.depthTestEnable = true, .depthWriteEnable = true, .depthCompareOp = vuk::CompareOp::eGreaterOrEqual}
        )
        .set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
        .set_viewport(0, vuk::Rect2D::framebuffer())
        .set_scissor(0, vuk::Rect2D::framebuffer())
        .bind_buffer(0, 0, camera)
        .bind_buffer(0, 1, meshlet_instances)
        .bind_buffer(0, 2, mesh_instances)
        .bind_buffer(0, 3, meshes)
        .bind_buffer(0, 4, transforms)
        .bind_index_buffer(index_buffer, vuk::IndexType::eUint32)
        .draw_indexed_indirect(1, triangle_indirect);

      return std::make_tuple(index_buffer, camera, meshlet_instances, mesh_instances, meshes, transforms, depth);
    }
  );

  std::tie(
    reordered_indices_buffer,
    camera_buffer,
    meshlet_instances_buffer,
    mesh_instances_buffer,
    meshes_buffer,
    transforms_buffer,
    depth_attachment
  ) =
    vis_encode_depth_pass(
      std::move(draw_command_buffer),
      std::move(reordered_indices_buffer),
      std::move(camera_buffer),
      std::move(meshlet_instances_buffer),
      std::move(mesh_instances_buffer),
      std::move(meshes_buffer),
      std::move(transforms_buffer),
      std::move(depth_attachment)
    );
}

auto generate_hiz(vuk::Value<vuk::ImageAttachment>& hiz_attachment, vuk::Value<vuk::ImageAttachment>& depth_attachment)
  -> void {
  ZoneScoped;

  auto hiz_generate_pass = vuk::make_pass(
    "hiz generate",
    [](
      vuk::CommandBuffer& cmd_list, //
      VUK_IA(vuk::eComputeSampled) src,
      VUK_IA(vuk::eComputeRW) dst
    ) {
      auto extent = dst->extent;
      auto mip_count = dst->level_count;

      cmd_list //
        .bind_compute_pipeline("hiz")
        .bind_sampler(0, 0, hiz_sampler_info);

      for (auto i = 0_u32; i < mip_count; i++) {
        auto mip_width = std::max(1_u32, extent.width >> i);
        auto mip_height = std::max(1_u32, extent.height >> i);

        if (i == 0) {
          cmd_list.bind_image(0, 1, src);
        } else {
          auto prev_mip = dst->mip(i - 1);
          cmd_list.image_barrier(prev_mip, vuk::eComputeRW, vuk::eComputeSampled);
          cmd_list.bind_image(0, 1, prev_mip);
        }

        auto cur_mip = dst->mip(i);
        cmd_list.bind_image(0, 2, cur_mip);
        cmd_list.push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(mip_width, mip_height, i));
        cmd_list.dispatch_invocations(mip_width, mip_height);
      }

      cmd_list.image_barrier(dst, vuk::eComputeSampled, vuk::eComputeRW);

      return std::make_tuple(src, dst);
    }
  );

  std::tie(depth_attachment, hiz_attachment) = hiz_generate_pass(
    std::move(depth_attachment),
    std::move(hiz_attachment)
  );
}
} // namespace ox
