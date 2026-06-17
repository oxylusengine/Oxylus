#include <vuk/runtime/CommandBuffer.hpp>
#include <vuk/vsl/Core.hpp>

#include "Memory/Stack.hpp"
#include "Render/RendererInstance.hpp"

namespace ox {
auto calculate_virtual_shadow_matrices(
  GPU::VSMContext& ctx,
  const glm::vec3& camera_position,
  const glm::vec3& light_dir,
  f32 max_shadow_dist,
  std::span<GPU::VirtualClipmap> directional_light_clipmaps
) -> void {
  ZoneScoped;

  // * Stable matrices *
  // We want our directional light matrix to be "stable" aka positionless.
  // This is to not keep getting cache hits on the virtual pages each time
  // camera moves. Later, we will offset the resulting projection view mat
  // per each page.

  auto forward = -light_dir;
  auto up = glm::vec3(0.0f, 1.0f, 0.0f);
  if (1.0f - glm::abs(glm::dot(forward, up)) < 1e-5f) {
    up = glm::vec3(0.0f, 0.0f, 1.0f);
  }

  auto page_table_size = glm::vec2(RMVSMContext::DIRECTIONAL_PAGE_TABLE_SIZE);
  auto clipmap_view = glm::lookAtRH(glm::vec3(0.0f), forward, up);

  for (auto clipmap_index = 0; clipmap_index < ctx.clipmap_count; clipmap_index++) {
    auto& clipmap = directional_light_clipmaps[clipmap_index];
    auto clipmap_scale = static_cast<f32>(1 << clipmap_index);
    auto clipmap_extent = ctx.first_clipmap_width * clipmap_scale;

    auto clipmap_near = -max_shadow_dist;
    auto clipmap_far = max_shadow_dist;

    auto clipmap_projection = glm::orthoRH_ZO(
      -clipmap_extent, //
      clipmap_extent,
      -clipmap_extent,
      clipmap_extent,
      clipmap_near,
      clipmap_far
    );
    clipmap_projection[1][1] *= -1.0f;

    auto clip_position = clipmap_projection * clipmap_view * glm::vec4(camera_position, 1.0f);
    auto ndc_position = glm::vec2(clip_position) / clip_position.w;
    auto center_uv_position = ndc_position * 0.5f;
    auto page_offset = glm::ivec2(center_uv_position * page_table_size);
    auto page_shift = (glm::vec2(page_offset) / page_table_size) * 2.0f;
    auto shifted_projection = glm::translate(glm::mat4(1.0f), glm::vec3(-page_shift, 0.0f)) * clipmap_projection;
    auto final_clipmap_view = glm::inverse(clipmap_projection) * shifted_projection * clipmap_view;

    clipmap.projection_view_mat = clipmap_projection * final_clipmap_view;
    clipmap.page_offset = page_offset;
    clipmap.z_near = clipmap_near;
  }
}

auto RendererInstance::draw_virtual_shadowmap(this RendererInstance& self, RMVSMContext& context) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  auto& vk = self.renderer.vk_context;

  auto vsm_ctx = GPU::VSMContext{
    .page_size = RMVSMContext::PAGE_SIZE,
    .page_table_size = RMVSMContext::DIRECTIONAL_PAGE_TABLE_SIZE,
    .physcial_page_table_size = RMVSMContext::DIRECTIONAL_IMAGE_SIZE,
    .clipmap_count = RMVSMContext::MAX_DIRECTIONAL_CLIPMAP_COUNT,
    .depth_extent = glm::ivec2(context.depth_extent.width, context.depth_extent.height),
    .first_clipmap_width = self.first_clipmap_width,
    .clipmap_selection_bias = self.clipmap_selection_bias,
    .virtual_extent = RMVSMContext::DIRECTIONAL_IMAGE_SIZE,
    .z_length = context.max_shadow_dist * 2.0f,
    .directional_light_dir = self.directional_light.direction,
  };

  GPU::VirtualClipmap directional_clipmaps[RMVSMContext::MAX_DIRECTIONAL_CLIPMAP_COUNT] = {};
  constexpr static auto directional_clipmaps_size_bytes = ox::count_of(directional_clipmaps) *
                                                          sizeof(GPU::VirtualClipmap);
  context.directional_clipmaps_buffer = vk->alloc_transient_buffer(
    vuk::MemoryUsage::eCPUtoGPU,
    directional_clipmaps_size_bytes
  );
  calculate_virtual_shadow_matrices(
    vsm_ctx,
    self.camera_data.position,
    self.directional_light.direction,
    context.max_shadow_dist,
    directional_clipmaps
  );
  std::memcpy(context.directional_clipmaps_buffer->mapped_ptr, directional_clipmaps, directional_clipmaps_size_bytes);

  auto page_visibility_mask_buffer = vk->alloc_transient_buffer(
    vuk::MemoryUsage::eGPUonly,
    RMVSMContext::DIRECTIONAL_PAGE_MASK_COUNT * sizeof(u32)
  );
  auto allocation_requests_buffer = vk->alloc_transient_buffer(
    vuk::MemoryUsage::eGPUonly,
    RMVSMContext::DIRECTIONAL_MAX_PAGE_COUNT * sizeof(GPU::VSMAllocRequest)
  );
  auto dirty_physical_page_addresses_buffer = vk->alloc_transient_buffer(
    vuk::MemoryUsage::eGPUonly,
    RMVSMContext::DIRECTIONAL_MAX_PAGE_COUNT * sizeof(glm::uvec2)
  );
  auto page_allocator_buffer = vk->scratch_buffer<GPU::VSMPageAllocator>({
    .requests = allocation_requests_buffer->device_address,
    .dirty_physical_page_addresses = dirty_physical_page_addresses_buffer->device_address,
  });
  auto clear_dirty_pages_cmd_buffer = vk->scratch_buffer<vuk::DispatchIndirectCommand>({
    .x = RMVSMContext::PAGE_SIZE / 16,
    .y = RMVSMContext::PAGE_SIZE / 16,
  });

  if (context.sun_moved) {
    context.virtual_page_table_attachment = vuk::discard_ia(
      "vsm virtual page table",
      self.vsm_virtual_page_table_attachment
    );
    context.virtual_page_table_attachment = vuk::clear_image(
      std::move(context.virtual_page_table_attachment),
      vuk::Black<u32>
    );
    context.physical_page_table_attachment = vuk::discard_ia(
      "vsm physical page table",
      self.vsm_physical_page_table_attachment
    );
    context.physical_page_table_attachment = vuk::clear_image(
      std::move(context.physical_page_table_attachment),
      vuk::Black<u32>
    );
  } else {
    context.virtual_page_table_attachment = vuk::acquire_ia(
      "vsm virtual page table",
      self.vsm_virtual_page_table_attachment,
      vuk::eComputeRW
    );
    context.physical_page_table_attachment = vuk::acquire_ia(
      "vsm physical page table",
      self.vsm_physical_page_table_attachment,
      vuk::eFragmentSampled
    );
  }

  auto reset_page_visibility_pass = vuk::make_pass(
    "vsm reset page visibility",
    [vsm_ctx](vuk::CommandBuffer& cmd_list, VUK_IA(vuk::eComputeRW) page_table) {
      cmd_list //
        .bind_compute_pipeline("rmvsm_reset_page_visibility")
        .bind_image(0, 0, page_table)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, vsm_ctx)
        .dispatch_invocations_per_pixel(page_table, 1.0f, 1.0f, static_cast<f32>(page_table->layer_count));

      return page_table;
    }
  );

  context.virtual_page_table_attachment = reset_page_visibility_pass(std::move(context.virtual_page_table_attachment));

  auto invalidate_pages_pass = vuk::make_pass(
    "vsm invalidate pages",
    [vsm_ctx](vuk::CommandBuffer& cmd_list, VUK_IA(vuk::eComputeRW) page_table) {
      cmd_list //
        .bind_compute_pipeline("rmvsm_invalidate_pages")
        .bind_image(0, 0, page_table)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, vsm_ctx)
        .dispatch_invocations_per_pixel(page_table, 1.0f, 1.0f, static_cast<f32>(page_table->layer_count));

      return page_table;
    }
  );

  context.virtual_page_table_attachment = invalidate_pages_pass(std::move(context.virtual_page_table_attachment));

  auto mark_visible_pages_pass = vuk::make_pass(
    "vsm mark visible pages",
    [vsm_ctx](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eComputeUniformRead) camera,
      VUK_BA(vuk::eComputeRead) clipmaps,
      VUK_IA(vuk::eComputeSampled) depth,
      VUK_IA(vuk::eComputeRW) page_table,
      VUK_BA(vuk::eComputeRW | vuk::eTransferRW) page_visibility_mask,
      VUK_BA(vuk::eComputeRW) allocator
    ) {
      cmd_list //
        .fill_buffer(page_visibility_mask, 0_u32)
        .bind_compute_pipeline("rmvsm_mark_visible_pages")
        .bind_buffer(0, 0, camera)
        .bind_buffer(0, 1, clipmaps)
        .bind_image(0, 2, depth)
        .bind_image(0, 3, page_table)
        .bind_buffer(0, 4, page_visibility_mask)
        .bind_buffer(0, 5, allocator)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, vsm_ctx)
        .dispatch_invocations_per_pixel(depth);

      return std::make_tuple(camera, clipmaps, depth, page_table, page_visibility_mask, allocator);
    }
  );

  std::tie(
    self.prepared_frame.camera_buffer,
    context.directional_clipmaps_buffer,
    context.depth_attachment,
    context.virtual_page_table_attachment,
    page_visibility_mask_buffer,
    page_allocator_buffer
  ) =
    mark_visible_pages_pass(
      std::move(self.prepared_frame.camera_buffer),
      std::move(context.directional_clipmaps_buffer),
      std::move(context.depth_attachment),
      std::move(context.virtual_page_table_attachment),
      std::move(page_visibility_mask_buffer),
      std::move(page_allocator_buffer)
    );

  auto free_invisible_pages_pass = vuk::make_pass(
    "vsm free invisible pages",
    [vsm_ctx](vuk::CommandBuffer& cmd_list, VUK_IA(vuk::eComputeRW) page_table) {
      cmd_list //
        .bind_compute_pipeline("rmvsm_free_invisible_pages")
        .bind_image(0, 0, page_table)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, vsm_ctx)
        .dispatch_invocations_per_pixel(page_table, 1.0f, 1.0f, static_cast<f32>(page_table->layer_count));

      return page_table;
    }
  );

  context.virtual_page_table_attachment = free_invisible_pages_pass(std::move(context.virtual_page_table_attachment));

  auto allocate_pages_pass = vuk::make_pass(
    "vsm allocate pages",
    [](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eComputeUniformRead) allocator,
      VUK_BA(vuk::eComputeRW) page_visibility_mask,
      VUK_IA(vuk::eComputeRW) page_table
    ) {
      cmd_list.bind_compute_pipeline("rmvsm_allocate_pages")
        .bind_buffer(0, 0, allocator)
        .bind_buffer(0, 1, page_visibility_mask)
        .bind_image(0, 2, page_table)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, RMVSMContext::DIRECTIONAL_PAGE_MASK_COUNT)
        .dispatch(1);

      return std::make_tuple(page_table, allocator);
    }
  );

  std::tie(context.virtual_page_table_attachment, page_allocator_buffer) = allocate_pages_pass(
    std::move(page_allocator_buffer),
    std::move(page_visibility_mask_buffer),
    std::move(context.virtual_page_table_attachment)
  );

  auto mark_dirty_pages_pass = vuk::make_pass(
    "vsm mark dirty pages",
    [vsm_ctx](
      vuk::CommandBuffer& cmd_list,
      VUK_IA(vuk::eComputeSampled) page_table,
      VUK_BA(vuk::eComputeRW) allocator,
      VUK_BA(vuk::eComputeRW) clear_cmd
    ) {
      cmd_list //
        .bind_compute_pipeline("rmvsm_mark_dirty_pages")
        .bind_image(0, 0, page_table)
        .bind_buffer(0, 1, clear_cmd)
        .bind_buffer(0, 2, allocator)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, vsm_ctx)
        .dispatch_invocations_per_pixel(page_table, 1.0f, 1.0f, static_cast<f32>(page_table->layer_count));

      return std::make_tuple(page_table, allocator, clear_cmd);
    }
  );

  std::tie(context.virtual_page_table_attachment, page_allocator_buffer, clear_dirty_pages_cmd_buffer) =
    mark_dirty_pages_pass(
      std::move(context.virtual_page_table_attachment),
      std::move(page_allocator_buffer),
      std::move(clear_dirty_pages_cmd_buffer)
    );

  auto clear_dirty_pages_pass = vuk::make_pass(
    "vsm clear dirty pages",
    [vsm_ctx](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eIndirectRead) clear_cmd,
      VUK_BA(vuk::eComputeUniformRead) allocator,
      VUK_IA(vuk::eComputeRW) physical_page_table
    ) {
      cmd_list //
        .bind_compute_pipeline("rmvsm_clear_dirty_pages")
        .bind_buffer(0, 0, allocator)
        .bind_image(0, 1, physical_page_table)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, vsm_ctx)
        .dispatch_indirect(clear_cmd);

      return std::make_tuple(allocator, physical_page_table);
    }
  );

  std::tie(page_allocator_buffer, context.physical_page_table_attachment) = clear_dirty_pages_pass(
    std::move(clear_dirty_pages_cmd_buffer),
    std::move(page_allocator_buffer),
    std::move(context.physical_page_table_attachment)
  );

  auto geometry_context = ShadowGeometryContext{};

  auto clipmap_camera = GPU::CullCamera{
    .position = self.camera_data.position,
    .acceptable_lod_error = self.camera_data.acceptable_lod_error,
    .resolution = self.camera_data.resolution,
    .near_clip = self.camera_data.near_clip,
    .mesh_instance_count = self.prepared_frame.mesh_instance_count,
  };

  // CullGeometryContext is hoisted outside the clipmap loop so the visibility /
  // dispatch-command buffers allocated by the first iteration's `cull_meshes`
  // pre-pass persist across subsequent iterations.
  //
  // We iterate from the largest clipmap (highest index) down to the smallest.
  // Clipmap 0 covers the smallest area around the camera; if we ran `cull_meshes`
  // against it, any mesh outside that tiny frustum would be marked invisible and
  // stay invisible for every subsequent clipmap. The largest clipmap's frustum
  // contains all the others, so culling against it yields a superset of every
  // clipmap's visible meshes. Per-clipmap `cull_meshlets` then refines against
  // each clipmap's own (tighter) frustum.
  auto cull_geometry_context = CullGeometryContext{
    .use_hiz = false,
  };

  auto physical_depth_attachment = vuk::declare_ia(
    "vsm depth",
    {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eDepthStencilAttachment,
     .extent =
       {.width = RMVSMContext::DIRECTIONAL_IMAGE_SIZE, .height = RMVSMContext::DIRECTIONAL_IMAGE_SIZE, .depth = 1},
     .format = vuk::Format::eD32Sfloat,
     .sample_count = vuk::Samples::e1,
     .level_count = 1,
     .layer_count = 1}
  );

  for (auto reverse_index = 0_u32; reverse_index < RMVSMContext::MAX_DIRECTIONAL_CLIPMAP_COUNT; reverse_index++) {
    const auto clipmap_index = RMVSMContext::MAX_DIRECTIONAL_CLIPMAP_COUNT - 1 - reverse_index;
    const auto& clipmap = directional_clipmaps[clipmap_index];
    clipmap_camera.projection_view = clipmap.projection_view_mat;
    clipmap_camera.near_clip = clipmap.z_near;

    vsm_ctx.curr_clipmap_index = clipmap_index;

    cull_geometry_context.cull_camera = clipmap_camera;
    cull_geometry_context.init_cull_meshes = (reverse_index == 0);
    self.cull_geometry(cull_geometry_context);
    geometry_context.draw_geometry_cmd_buffer = std::move(cull_geometry_context.draw_geometry_cmd_buffer);

    auto draw_physical_pages_pass = vuk::make_pass(
      stack.format("vsm draw clipmap {}", clipmap_index),
      [vsm_ctx, vsm_physical_pages_u32_view = *self.vsm_physical_page_table_u32_view](
        vuk::CommandBuffer& cmd_list,
        VUK_BA(vuk::eIndirectRead) triangle_indirect,
        VUK_BA(vuk::eIndexRead) index_buffer,
        VUK_BA(vuk::eVertexRead) meshes,
        VUK_BA(vuk::eVertexRead) mesh_instances,
        VUK_BA(vuk::eVertexRead) meshlet_instances,
        VUK_BA(vuk::eVertexRead) transforms,
        VUK_BA(vuk::eVertexRead | vuk::eFragmentRead) clipmaps,
        VUK_IA(vuk::eFragmentSampled) page_tables,
        VUK_IA(vuk::eFragmentRW) physical_pages,
        VUK_IA(vuk::eDepthStencilRW) dummy_depth
      ) {
        auto viewport_rect = vuk::Rect2D{
          .offset = {.x = 0, .y = 0},
          .extent = {.width = RMVSMContext::DIRECTIONAL_IMAGE_SIZE, .height = RMVSMContext::DIRECTIONAL_IMAGE_SIZE},
          ._relative = {},
        };
        cmd_list //
          .bind_graphics_pipeline("rmvsm_draw_physical_pages")
          .set_rasterization({.cullMode = vuk::CullModeFlagBits::eNone})
          .set_depth_stencil({.depthWriteEnable = false, .depthCompareOp = vuk::CompareOp::eNever})
          .set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
          .set_viewport(0, viewport_rect)
          .set_scissor(0, vuk::Rect2D::framebuffer())
          .bind_buffer(0, 0, meshes)
          .bind_buffer(0, 1, mesh_instances)
          .bind_buffer(0, 2, meshlet_instances)
          .bind_buffer(0, 3, transforms)
          .bind_buffer(0, 4, clipmaps)
          .bind_image(0, 5, page_tables)
          .bind_image(0, 6, vsm_physical_pages_u32_view, vuk::ImageLayout::eGeneral)
          .bind_index_buffer(index_buffer, vuk::IndexType::eUint32)
          .push_constants(vuk::ShaderStageFlagBits::eVertex | vuk::ShaderStageFlagBits::eFragment, 0, vsm_ctx)
          .draw_indexed_indirect(1, triangle_indirect);

        return std::make_tuple(
          index_buffer, //
          meshes,
          mesh_instances,
          meshlet_instances,
          transforms,
          clipmaps,
          page_tables,
          physical_pages,
          dummy_depth
        );
      }
    );

    std::tie(
      self.prepared_frame.reordered_indices_buffer,
      self.prepared_frame.meshes_buffer,
      self.prepared_frame.mesh_instances_buffer,
      self.prepared_frame.meshlet_instances_buffer,
      self.prepared_frame.transforms_buffer,
      context.directional_clipmaps_buffer,
      context.virtual_page_table_attachment,
      context.physical_page_table_attachment,
      physical_depth_attachment
    ) =
      draw_physical_pages_pass(
        std::move(geometry_context.draw_geometry_cmd_buffer),
        std::move(self.prepared_frame.reordered_indices_buffer),
        std::move(self.prepared_frame.meshes_buffer),
        std::move(self.prepared_frame.mesh_instances_buffer),
        std::move(self.prepared_frame.meshlet_instances_buffer),
        std::move(self.prepared_frame.transforms_buffer),
        std::move(context.directional_clipmaps_buffer),
        std::move(context.virtual_page_table_attachment),
        std::move(context.physical_page_table_attachment),
        std::move(physical_depth_attachment)
      );
  }
}

auto RendererInstance::resolve_shadowmap(this RendererInstance& self, ShadowResolveContext& context) -> void {
  ZoneScoped;

  auto vsm_ctx = GPU::VSMContext{
    .page_size = RMVSMContext::PAGE_SIZE,
    .page_table_size = RMVSMContext::DIRECTIONAL_PAGE_TABLE_SIZE,
    .physcial_page_table_size = RMVSMContext::DIRECTIONAL_IMAGE_SIZE,
    .clipmap_count = RMVSMContext::MAX_DIRECTIONAL_CLIPMAP_COUNT,
    .first_clipmap_width = self.first_clipmap_width,
    .clipmap_selection_bias = self.clipmap_selection_bias,
    .virtual_extent = RMVSMContext::DIRECTIONAL_IMAGE_SIZE,
    .z_length = context.max_shadow_dist * 2.0f,
    .directional_light_dir = self.directional_light.direction,
  };

  auto resolve_pass = vuk::make_pass(
    "resolve shadows",
    [vsm_ctx](
      vuk::CommandBuffer& cmd_list,
      VUK_IA(vuk::eColorRW) resolved,
      VUK_BA(vuk::eFragmentUniformRead) camera,
      VUK_IA(vuk::eFragmentSampled) depth,
      VUK_IA(vuk::eFragmentSampled) normals,
      VUK_BA(vuk::eFragmentRead) clipmaps,
      VUK_IA(vuk::eFragmentSampled) page_tables,
      VUK_IA(vuk::eFragmentSampled) physical_pages
    ) {
      cmd_list //
        .bind_graphics_pipeline("resolve_shadowmaps")
        .set_color_blend(resolved, vuk::BlendPreset::eOff)
        .set_rasterization({.cullMode = vuk::CullModeFlagBits::eNone})
        .set_depth_stencil({.depthWriteEnable = false, .depthCompareOp = vuk::CompareOp::eNever})
        .set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
        .set_viewport(0, vuk::Rect2D::framebuffer())
        .set_scissor(0, vuk::Rect2D::framebuffer())
        .bind_buffer(0, 0, camera)
        .bind_image(0, 1, depth)
        .bind_image(0, 2, normals)
        .bind_buffer(2, 0, clipmaps)
        .bind_image(2, 1, page_tables)
        .bind_image(2, 2, physical_pages)
        .push_constants(vuk::ShaderStageFlagBits::eFragment, 0, vsm_ctx)
        .draw(3, 1, 0, 0);

      return std::make_tuple(resolved, camera, depth, normals);
    }
  );

  std::tie(
    context.resolved_shadows_attachment,
    self.prepared_frame.camera_buffer,
    context.depth_attachment,
    context.normal_attachment
  ) =
    resolve_pass(
      std::move(context.resolved_shadows_attachment),
      std::move(self.prepared_frame.camera_buffer),
      std::move(context.depth_attachment),
      std::move(context.normal_attachment),
      std::move(context.directional_clipmaps_buffer),
      std::move(context.virtual_page_table_attachment),
      std::move(context.physical_page_table_attachment)
    );
}

} // namespace ox
