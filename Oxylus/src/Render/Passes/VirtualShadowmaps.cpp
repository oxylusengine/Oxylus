#include <vuk/runtime/CommandBuffer.hpp>
#include <vuk/vsl/Core.hpp>

#include "Memory/Stack.hpp"
#include "Render/RendererInstance.hpp"

namespace ox {
auto calculate_virtual_shadow_matrices(
  GPU::VSMContext& ctx,
  const glm::vec3& camera_position,
  const glm::vec3& light_dir,
  f32 min_shadow_dist,
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

    auto clipmap_near = -min_shadow_dist;
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
    .first_clipmap_width = 10.0f,
    .clipmap_selection_bias = 0.0f,
    .virtual_extent = RMVSMContext::DIRECTIONAL_IMAGE_SIZE,
  };

  GPU::VirtualClipmap directional_clipmaps[RMVSMContext::MAX_DIRECTIONAL_CLIPMAP_COUNT] = {};
  constexpr static auto directional_clipmaps_size_bytes = ox::count_of(directional_clipmaps) *
                                                          sizeof(GPU::VirtualClipmap);
  auto directional_clipmaps_buffer = vk->alloc_transient_buffer(
    vuk::MemoryUsage::eCPUtoGPU,
    directional_clipmaps_size_bytes
  );
  calculate_virtual_shadow_matrices(
    vsm_ctx,
    self.camera_data.position,
    self.directional_light.direction,
    context.min_shadow_dist,
    context.max_shadow_dist,
    directional_clipmaps
  );
  std::memcpy(directional_clipmaps_buffer->mapped_ptr, directional_clipmaps, directional_clipmaps_size_bytes);

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

  auto virtual_page_table_attachment = vuk::Value<vuk::ImageAttachment>{};
  auto physical_page_table_attachment = vuk::Value<vuk::ImageAttachment>{};
  if (context.sun_moved) {
    virtual_page_table_attachment = vuk::discard_ia("vsm virtual page table", self.vsm_virtual_page_table_attachment);
    virtual_page_table_attachment = vuk::clear_image(std::move(virtual_page_table_attachment), vuk::Black<u32>);
    physical_page_table_attachment = vuk::discard_ia(
      "vsm physical page table",
      self.vsm_physical_page_table_attachment
    );
    physical_page_table_attachment = vuk::clear_image(std::move(physical_page_table_attachment), vuk::Black<u32>);
  } else {
    virtual_page_table_attachment = vuk::acquire_ia(
      "vsm virtual page table",
      self.vsm_virtual_page_table_attachment,
      vuk::eComputeRW
    );
    physical_page_table_attachment = vuk::acquire_ia(
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

  virtual_page_table_attachment = reset_page_visibility_pass(std::move(virtual_page_table_attachment));

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

  virtual_page_table_attachment = invalidate_pages_pass(std::move(virtual_page_table_attachment));

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
    directional_clipmaps_buffer,
    context.depth_attachment,
    virtual_page_table_attachment,
    page_visibility_mask_buffer,
    page_allocator_buffer
  ) =
    mark_visible_pages_pass(
      std::move(self.prepared_frame.camera_buffer),
      std::move(directional_clipmaps_buffer),
      std::move(context.depth_attachment),
      std::move(virtual_page_table_attachment),
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

  virtual_page_table_attachment = free_invisible_pages_pass(std::move(virtual_page_table_attachment));

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

  std::tie(virtual_page_table_attachment, page_allocator_buffer) = allocate_pages_pass(
    std::move(page_allocator_buffer),
    std::move(page_visibility_mask_buffer),
    std::move(virtual_page_table_attachment)
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

  std::tie(virtual_page_table_attachment, page_allocator_buffer, clear_dirty_pages_cmd_buffer) = mark_dirty_pages_pass(
    std::move(virtual_page_table_attachment),
    std::move(page_allocator_buffer),
    std::move(clear_dirty_pages_cmd_buffer)
  );

  auto clear_dirty_pages_pass = vuk::make_pass(
    "vsm clear dirty pages",
    [vsm_ctx](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eIndirectRead) clear_cmd,
      VUK_BA(vuk::eComputeUniformRead) allocator,
      VUK_IA(vuk::eComputeRW) physical_page_table,
      VUK_IA(vuk::eComputeSampled) depth
    ) {
      cmd_list //
        .bind_compute_pipeline("rmvsm_clear_dirty_pages")
        .bind_buffer(0, 0, allocator)
        .bind_image(0, 1, physical_page_table)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, vsm_ctx)
        .dispatch_indirect(clear_cmd);

      return std::make_tuple(allocator, physical_page_table, depth);
    }
  );

  std::tie(page_allocator_buffer, physical_page_table_attachment, context.depth_attachment) = clear_dirty_pages_pass(
    std::move(clear_dirty_pages_cmd_buffer),
    std::move(page_allocator_buffer),
    std::move(physical_page_table_attachment),
    std::move(context.depth_attachment)
  );
}

} // namespace ox
