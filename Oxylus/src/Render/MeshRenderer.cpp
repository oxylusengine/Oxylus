#include "Render/MeshRenderer.hpp"

#include "Core/App.hpp"
#include "Memory/Stack.hpp"
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

MeshRenderer::MeshRenderer(VkContext& vk_context, vuk::PersistentDescriptorSet& bindless_set)
    : vk_context_(vk_context),
      bindless_set_(bindless_set) {}

auto MeshRenderer::set_extent(this MeshRenderer& self, vuk::Extent3D extent) -> MeshRenderer& {
  self.extent_ = extent;
  return self;
}

auto MeshRenderer::set_pass_name(this MeshRenderer& self, const std::string& name) -> MeshRenderer& {
  self.pass_name_ = name;
  return self;
}

auto MeshRenderer::set_cull_flags(this MeshRenderer& self, GPU::CullFlags flags) -> MeshRenderer& {
  self.cull_flags_ = flags;
  return self;
}

auto MeshRenderer::disable_hiz(this MeshRenderer& self) -> MeshRenderer& {
  self.enable_hiz = false;
  return self;
}

auto MeshRenderer::render(MeshRenderContext& ctx)
  -> std::tuple<vuk::Value<vuk::ImageAttachment>, vuk::Value<vuk::ImageAttachment>> {
  ZoneScoped;
  memory::ScopedStack stack;

  auto visbuffer_attachment = vuk::declare_ia(
    stack.format("visbuffer_{}", pass_name_),
    {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
     .extent = extent_,
     .format = vuk::Format::eR32Uint,
     .sample_count = vuk::Samples::e1,
     .level_count = 1,
     .layer_count = 1}
  );

  auto overdraw_attachment = vuk::declare_ia(
    stack.format("overdraw_{}", pass_name_),
    {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage,
     .extent = extent_,
     .format = vuk::Format::eR32Uint,
     .sample_count = vuk::Samples::e1,
     .level_count = 1,
     .layer_count = 1}
  );

  auto vis_clear_pass = vuk::make_pass(
    stack.format("vis_clear_{}", pass_name_),
    [](vuk::CommandBuffer& cmd_list, VUK_IA(vuk::eComputeWrite) visbuffer, VUK_IA(vuk::eComputeWrite) overdraw) {
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

  auto cull_meshlets_cmd_buffer = cull_meshes(ctx);

  auto early_draw_visbuffer_cmd_buffer = cull_meshlets(ctx, false, cull_meshlets_cmd_buffer);
  draw_visbuffer(ctx, false, bindless_set_, visbuffer_attachment, overdraw_attachment, early_draw_visbuffer_cmd_buffer);

  if (enable_hiz) {
    generate_hiz(ctx);
  }

  auto late_draw_visbuffer_cmd_buffer = cull_meshlets(ctx, true, cull_meshlets_cmd_buffer);
  draw_visbuffer(ctx, true, bindless_set_, visbuffer_attachment, overdraw_attachment, late_draw_visbuffer_cmd_buffer);

  return std::make_tuple(visbuffer_attachment, overdraw_attachment);
}

vuk::Value<vuk::Buffer> MeshRenderer::cull_meshes(MeshRenderContext& ctx) {
  ZoneScoped;

  memory::ScopedStack stack;

  auto vis_cull_meshes_pass = vuk::make_pass(
    stack.format("vis_cull_meshes_{}", pass_name_),
    [cull_flags = cull_flags_, mesh_instance_count = ctx.mesh_instance_count](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eComputeRead) camera,
      VUK_BA(vuk::eComputeRead) meshes,
      VUK_BA(vuk::eComputeRead) transforms,
      VUK_IA(vuk::eComputeSampled) hiz,
      VUK_BA(vuk::eComputeRW) mesh_instances,
      VUK_BA(vuk::eComputeRW) meshlet_instances,
      VUK_BA(vuk::eComputeRW) visible_meshlet_instances_count
    ) {
      cmd_list.bind_compute_pipeline("cull_meshes")
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
    ctx.camera_buffer,
    ctx.meshes_buffer,
    ctx.transforms_buffer,
    ctx.hiz_attachment,
    ctx.mesh_instances_buffer,
    ctx.meshlet_instances_buffer,
    ctx.visible_meshlet_instances_count_buffer
  ) =
    vis_cull_meshes_pass(
      std::move(ctx.camera_buffer),
      std::move(ctx.meshes_buffer),
      std::move(ctx.transforms_buffer),
      std::move(ctx.hiz_attachment),
      std::move(ctx.mesh_instances_buffer),
      std::move(ctx.meshlet_instances_buffer),
      std::move(ctx.visible_meshlet_instances_count_buffer)
    );

  auto generate_cull_commands_pass = vuk::make_pass(
    stack.format("generate_cull_commands_{}", pass_name_),
    [](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eComputeRead) visible_meshlet_instances_count,
      VUK_BA(vuk::eComputeRW) cull_meshlets_cmd
    ) {
      cmd_list.bind_compute_pipeline("generate_cull_commands")
        .bind_buffer(0, 0, visible_meshlet_instances_count)
        .bind_buffer(0, 1, cull_meshlets_cmd)
        .dispatch(1);

      return std::make_tuple(visible_meshlet_instances_count, cull_meshlets_cmd);
    }
  );

  auto cull_meshlets_cmd_buffer = vk_context_.scratch_buffer<vuk::DispatchIndirectCommand>({.x = 0, .y = 1, .z = 1});
  std::tie(ctx.visible_meshlet_instances_count_buffer, cull_meshlets_cmd_buffer) = generate_cull_commands_pass(
    std::move(ctx.visible_meshlet_instances_count_buffer),
    std::move(cull_meshlets_cmd_buffer)
  );

  return cull_meshlets_cmd_buffer;
}

auto MeshRenderer::cull_meshlets(MeshRenderContext& ctx, bool late, vuk::Value<vuk::Buffer>& cull_meshlets_cmd_buffer)
  -> vuk::Value<vuk::Buffer> {
  ZoneScoped;
  memory::ScopedStack stack;

  //  --- CULL MESHLETS ---
  auto vis_cull_meshlets_pass = vuk::make_pass(
    stack.format("vis_cull_meshlets_{}_{}", pass_name_, late ? "late" : "early"),
    [late, cull_flags = cull_flags_](
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

  auto cull_triangles_cmd_buffer = vk_context_.scratch_buffer<vuk::DispatchIndirectCommand>({.x = 0, .y = 1, .z = 1});

  std::tie(
    cull_meshlets_cmd_buffer,
    ctx.camera_buffer,
    ctx.meshlet_instances_buffer,
    ctx.mesh_instances_buffer,
    ctx.meshes_buffer,
    ctx.transforms_buffer,
    ctx.hiz_attachment,
    ctx.visible_meshlet_instances_count_buffer,
    ctx.visible_meshlet_instances_indices_buffer,
    ctx.meshlet_instance_visibility_mask_buffer,
    cull_triangles_cmd_buffer
  ) =
    vis_cull_meshlets_pass(
      std::move(cull_meshlets_cmd_buffer),
      std::move(ctx.camera_buffer),
      std::move(ctx.meshlet_instances_buffer),
      std::move(ctx.mesh_instances_buffer),
      std::move(ctx.meshes_buffer),
      std::move(ctx.transforms_buffer),
      std::move(ctx.hiz_attachment),
      std::move(ctx.visible_meshlet_instances_count_buffer),
      std::move(ctx.visible_meshlet_instances_indices_buffer),
      std::move(ctx.meshlet_instance_visibility_mask_buffer),
      std::move(cull_triangles_cmd_buffer)
    );

  //  --- CULL TRIANGLES ---
  auto vis_cull_triangles_pass = vuk::make_pass(
    stack.format("vis_cull_triangles_{}_{}", pass_name_, late ? "late" : "early"),
    [late, cull_flags = cull_flags_](
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

  auto draw_command_buffer = vk_context_.scratch_buffer<vuk::DrawIndexedIndirectCommand>({.instanceCount = 1});

  std::tie(
    ctx.camera_buffer,
    ctx.visible_meshlet_instances_count_buffer,
    ctx.visible_meshlet_instances_indices_buffer,
    ctx.meshlet_instances_buffer,
    ctx.mesh_instances_buffer,
    ctx.meshes_buffer,
    ctx.transforms_buffer,
    draw_command_buffer,
    ctx.reordered_indices_buffer
  ) =
    vis_cull_triangles_pass(
      std::move(cull_triangles_cmd_buffer),
      std::move(ctx.camera_buffer),
      std::move(ctx.visible_meshlet_instances_count_buffer),
      std::move(ctx.visible_meshlet_instances_indices_buffer),
      std::move(ctx.meshlet_instances_buffer),
      std::move(ctx.mesh_instances_buffer),
      std::move(ctx.meshes_buffer),
      std::move(ctx.transforms_buffer),
      std::move(draw_command_buffer),
      std::move(ctx.reordered_indices_buffer)
    );

  return draw_command_buffer;
}

void MeshRenderer::draw_visbuffer(
  MeshRenderContext& ctx,
  bool late,
  vuk::PersistentDescriptorSet& descriptor_set,
  vuk::Value<vuk::ImageAttachment>& visbuffer_attachment,
  vuk::Value<vuk::ImageAttachment>& overdraw_attachment,
  vuk::Value<vuk::Buffer>& draw_command_buffer
) {
  ZoneScoped;
  memory::ScopedStack stack;

  auto vis_encode_pass = vuk::make_pass(
    stack.format("vis_encode_{}_{}", pass_name_, late ? "late" : "early"),
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
    ctx.reordered_indices_buffer,
    ctx.camera_buffer,
    ctx.meshlet_instances_buffer,
    ctx.mesh_instances_buffer,
    ctx.meshes_buffer,
    ctx.transforms_buffer,
    ctx.materials_buffer,
    visbuffer_attachment,
    ctx.depth_attachment,
    overdraw_attachment
  ) =
    vis_encode_pass(
      std::move(draw_command_buffer),
      std::move(ctx.reordered_indices_buffer),
      std::move(ctx.camera_buffer),
      std::move(ctx.meshlet_instances_buffer),
      std::move(ctx.mesh_instances_buffer),
      std::move(ctx.meshes_buffer),
      std::move(ctx.transforms_buffer),
      std::move(ctx.materials_buffer),
      std::move(visbuffer_attachment),
      std::move(ctx.depth_attachment),
      std::move(overdraw_attachment)
    );
}

void MeshRenderer::generate_hiz(MeshRenderContext& ctx) {
  ZoneScoped;

  memory::ScopedStack stack;

  auto hiz_generate_pass = vuk::make_pass(
    stack.format("hiz_generate_{}", pass_name_),
    [](vuk::CommandBuffer& cmd_list, VUK_IA(vuk::eComputeSampled) src, VUK_IA(vuk::eComputeRW) dst) {
      auto extent = dst->extent;
      auto mip_count = dst->level_count;

      cmd_list.bind_compute_pipeline("hiz").bind_sampler(0, 0, hiz_sampler_info);

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

  std::tie(ctx.depth_attachment, ctx.hiz_attachment) = hiz_generate_pass(
    std::move(ctx.depth_attachment),
    std::move(ctx.hiz_attachment)
  );
}
} // namespace ox
