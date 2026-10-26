#include <ankerl/svector.h>
#include <vir/RenderGraph.hpp>
#include <vir/Types.hpp>

#include "Memory/Stack.hpp"
#include "Render/RenderContext.hpp"
#include "Render/RendererInstance.hpp"
#include "Render/Utils/PushConstants.hpp"
#include "Render/Utils/Samplers.hpp"
#include "Scene/Scene.hpp"
#include "Utils/Log.hpp"

namespace ox {
using DirectionalClipmaps = std::array<GPU::VirtualClipmap, RMVSMContext::MAX_DIRECTIONAL_CLIPMAP_COUNT>;
using PointSpotViews = std::array<GPU::VSMPointSpotView, RMVSMContext::POINT_SPOT_LAYER_COUNT>;
using ClipmapScrollDeltas = std::array<glm::ivec2, RMVSMContext::MAX_DIRECTIONAL_CLIPMAP_COUNT>;
using PointSpotMovedPush = PushConstants<glm::uvec2, glm::uvec2>;
using PointSpotMarkPush = PushConstants<GPU::VSMPointSpotContext, glm::vec3>;

constexpr static auto TRANSIENT_BUFFER_USAGE = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

// cube face order {+X, -X, +Y, -Y, +Z, -Z}, must match `vsm_point_face_index` in rmvsm.slang
constexpr static glm::vec3 VSM_POINT_FACE_DIRS[6] = {
  {1.0f, 0.0f, 0.0f},
  {-1.0f, 0.0f, 0.0f},
  {0.0f, 1.0f, 0.0f},
  {0.0f, -1.0f, 0.0f},
  {0.0f, 0.0f, 1.0f},
  {0.0f, 0.0f, -1.0f},
};
constexpr static glm::vec3 VSM_POINT_FACE_UPS[6] = {
  {0.0f, 0.0f, 1.0f},
  {0.0f, 0.0f, 1.0f},
  {0.0f, 0.0f, 1.0f},
  {0.0f, 0.0f, 1.0f},
  {0.0f, 1.0f, 0.0f},
  {0.0f, 1.0f, 0.0f},
};

auto calculate_pointspot_shadow_views(std::span<const GPU::Light> lights, std::span<GPU::VSMPointSpotView> views)
  -> void {
  ZoneScoped;

  for (auto light_index = 0_u32; light_index < lights.size(); light_index++) {
    const auto& light = lights[light_index];
    if (light.shadow_map_index < 0 || light.range <= 0.0f) {
      continue;
    }

    if (light.kind == GPU::LightKind::Point) {
      auto projection = glm::perspectiveRH_ZO(
        glm::radians(RMVSMContext::POINT_LIGHT_FOV_DEG),
        1.0f,
        RMVSMContext::POINT_LIGHT_NEAR,
        light.range
      );
      projection[1][1] *= -1.0f;

      for (auto face = 0_u32; face < 6; face++) {
        const auto view_mat = glm::lookAtRH(
          light.position,
          light.position + VSM_POINT_FACE_DIRS[face],
          VSM_POINT_FACE_UPS[face]
        );
        views[light.shadow_map_index * 6 + face] = GPU::VSMPointSpotView{
          .projection_view = projection * view_mat,
          .light_position = light.position,
          .range = light.range,
          .light_index = light_index,
          .z_near = RMVSMContext::POINT_LIGHT_NEAR,
          .texel_world_scale = glm::tan(glm::radians(RMVSMContext::POINT_LIGHT_FOV_DEG * 0.5f)),
        };
      }
    } else if (light.kind == GPU::LightKind::Spot) {
      const auto fov = glm::clamp(light.outer_cone_angle * 2.0f, glm::radians(1.0f), glm::radians(170.0f));
      auto projection = glm::perspectiveRH_ZO(fov, 1.0f, RMVSMContext::SPOT_LIGHT_NEAR, light.range);
      projection[1][1] *= -1.0f;

      auto up = glm::vec3(0.0f, 1.0f, 0.0f);
      if (1.0f - glm::abs(glm::dot(light.direction, up)) < 1e-5f) {
        up = glm::vec3(0.0f, 0.0f, 1.0f);
      }
      const auto view_mat = glm::lookAtRH(light.position, light.position + light.direction, up);

      views[RMVSMContext::POINT_SPOT_SPOT_LAYER_OFFSET + light.shadow_map_index] = GPU::VSMPointSpotView{
        .projection_view = projection * view_mat,
        .light_position = light.position,
        .range = light.range,
        .light_index = light_index,
        .z_near = RMVSMContext::SPOT_LIGHT_NEAR,
        .texel_world_scale = glm::tan(fov * 0.5f),
      };
    }
  }
}

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

auto RendererInstance::prepare_virtual_shadowmap(this RendererInstance& self) -> void {
  ZoneScoped;

  auto& params = self.frame_params;
  const auto& shape = params.shape;
  const auto depth_extent = glm::ivec2(params.render_size);

  params.vsm = GPU::VSMContext{
    .page_size = RMVSMContext::PAGE_SIZE,
    .page_table_size = RMVSMContext::DIRECTIONAL_PAGE_TABLE_SIZE,
    .physcial_page_table_size = RMVSMContext::PHYSICAL_PAGE_TABLE_SIZE,
    .clipmap_count = RMVSMContext::MAX_DIRECTIONAL_CLIPMAP_COUNT,
    .depth_extent = depth_extent,
    .first_clipmap_width = self.first_clipmap_width,
    .clipmap_selection_bias = self.clipmap_selection_bias,
    .virtual_extent = RMVSMContext::DIRECTIONAL_IMAGE_RESOLUTION,
    .z_length = RMVSMContext::MAX_SHADOW_DISTANCE * 2.0f,
    .directional_light_dir = self.directional_light.direction,
  };
  params.vsm_pointspot = GPU::VSMPointSpotContext{
    .layer_count = RMVSMContext::POINT_SPOT_LAYER_COUNT,
    .mesh_instance_count = self.frame_data.mesh_instance_count,
    .depth_extent = depth_extent,
    .mip_bias_min = 0,
    .shadow_point_light_count = self.frame_data.shadow_point_light_count,
    .shadow_spot_light_count = self.frame_data.shadow_spot_light_count,
  };
  params.vsm_sun_moved = self.sun_direction_changed;

  calculate_virtual_shadow_matrices(
    params.vsm,
    self.camera_data.position,
    self.directional_light.direction,
    RMVSMContext::MAX_SHADOW_DISTANCE,
    params.vsm_directional_clipmaps
  );

  const auto is_directional_running = shape.has_directional_shadows && shape.has_meshes;
  if (!(self.gpu_scene_flags & GPU::SceneFlags::HasDirectionalLight) || !is_directional_running) {
    self.directional_vsm_cache_valid = false;
  }

  params.vsm_clipmap_scroll_deltas = {};
  if (is_directional_running) {
    // preserve overlapping toroidal pages while invalidating the rows and columns exposed by a clipmap shift
    const auto is_mapping_changed = !self.directional_vsm_cache_valid ||
                                    self.previous_directional_clipmap_width != self.first_clipmap_width ||
                                    self.previous_directional_shadow_distance != RMVSMContext::MAX_SHADOW_DISTANCE;

    for (auto clipmap_index = 0_u32; clipmap_index < RMVSMContext::MAX_DIRECTIONAL_CLIPMAP_COUNT; clipmap_index++) {
      const auto& clipmap = params.vsm_directional_clipmaps[clipmap_index];
      if (is_mapping_changed && !params.vsm_sun_moved) {
        params.vsm_clipmap_scroll_deltas[clipmap_index] = glm::ivec2(RMVSMContext::DIRECTIONAL_PAGE_TABLE_SIZE);
      } else if (!params.vsm_sun_moved) {
        params.vsm_clipmap_scroll_deltas[clipmap_index] = clipmap.page_offset -
                                                          self.previous_directional_clipmap_offsets[clipmap_index];
      }

      self.previous_directional_clipmap_offsets[clipmap_index] = clipmap.page_offset;
    }

    self.previous_directional_clipmap_width = self.first_clipmap_width;
    self.previous_directional_shadow_distance = RMVSMContext::MAX_SHADOW_DISTANCE;
    self.directional_vsm_cache_valid = true;
  }

  params.vsm_pointspot_views = {};
  calculate_pointspot_shadow_views(self.scene.lights.slots_unsafe(), params.vsm_pointspot_views);
}

auto RendererInstance::bind_shadow_variables(
  this const RendererInstance& self, vir::Program& program, const ShadowVariables& variables
) -> void {
  ZoneScoped;

  const auto& params = self.frame_params;
  const auto& data = self.frame_data;
  program.set_bytes(variables.context, params.vsm);
  program.set_bytes(variables.pointspot_context, params.vsm_pointspot);
  program.set_bytes(variables.directional_clipmaps, params.vsm_directional_clipmaps);
  program.set_bytes(variables.pointspot_views, params.vsm_pointspot_views);
  program.set_bytes(variables.clipmap_scroll_deltas, params.vsm_clipmap_scroll_deltas);
  program.set(variables.is_sun_moved, params.vsm_sun_moved);

  if (program.has_variable(variables.clipmap_context)) {
    auto clipmap_context = params.vsm;
    clipmap_context.curr_clipmap_index = RMVSMContext::MAX_DIRECTIONAL_CLIPMAP_COUNT - 1;
    program.set_bytes(variables.clipmap_context, clipmap_context);
  }

  if (program.has_variable(variables.pointspot_moved_lights) || program.has_variable(variables.pointspot_mark_push)) {
    const auto moved_point = data.moved_point_light_mask;
    const auto moved_spot = data.moved_spot_light_mask;
    program.set_bytes(
      variables.pointspot_moved_lights,
      PointSpotMovedPush(
        glm::uvec2(static_cast<u32>(moved_point), static_cast<u32>(moved_point >> 32)),
        glm::uvec2(static_cast<u32>(moved_spot), static_cast<u32>(moved_spot >> 32))
      )
    );
    program.set_bytes(variables.pointspot_mark_push, PointSpotMarkPush(params.vsm_pointspot, self.light_grid_origin));
  }

  program.set(variables.pointspot_dirty_count, static_cast<u32>(data.dirty_mesh_instances.size()));
  program.set(variables.pointspot_removed_count, static_cast<u32>(data.removed_mesh_bounds.size()));
  program.set(variables.dirty_count, params.vsm_sun_moved ? 0 : static_cast<u32>(data.dirty_mesh_instances.size()));
  program.set(variables.removed_count, params.vsm_sun_moved ? 0 : static_cast<u32>(data.removed_mesh_bounds.size()));
}

auto RendererInstance::draw_virtual_shadowmap(
  this RendererInstance& self, vir::Module& module, FrameRecording& recording, RMVSMContext& context
) -> void {
  ZoneScoped;
  auto& prepared = recording.prepared;
  auto& values = recording.values;
  memory::ScopedStack stack;

  auto& render_context = *self.renderer.render_context;
  auto& variables = prepared.shadow;
  variables.context = module.declare_bytes_var("vsm context", sizeof(GPU::VSMContext));
  variables.pointspot_context = module.declare_bytes_var("vsm pointspot context", sizeof(GPU::VSMPointSpotContext));
  variables.clipmap_context = module.declare_bytes_var("vsm clipmap context", sizeof(GPU::VSMContext));
  variables.directional_clipmaps = module.declare_bytes_var("vsm directional clipmaps", sizeof(DirectionalClipmaps));
  variables.pointspot_views = module.declare_bytes_var("vsm pointspot views", sizeof(PointSpotViews));
  variables.clipmap_scroll_deltas = module.declare_bytes_var("vsm clipmap scroll deltas", sizeof(ClipmapScrollDeltas));
  variables.is_sun_moved = module.declare_bool_var("vsm sun moved", false);
  variables.pointspot_moved_lights = module.declare_bytes_var("vsm pointspot moved lights", sizeof(PointSpotMovedPush));
  variables.pointspot_mark_push = module.declare_bytes_var(
    "vsm pointspot mark visible push",
    sizeof(PointSpotMarkPush)
  );
  variables.pointspot_dirty_count = module.declare_u32_var("vsm pointspot dirty meshes", 0);
  variables.pointspot_removed_count = module.declare_u32_var("vsm pointspot removed bounds", 0);
  variables.dirty_count = module.declare_u32_var("vsm dirty meshes", 0);
  variables.removed_count = module.declare_u32_var("vsm removed bounds", 0);

  const auto& shape = recording.shape;
  const auto is_directional = shape.has_directional_shadows;
  const auto is_pointspot = shape.has_pointspot_shadows;
  const auto use_mesh_shaders = shape.use_mesh_shaders;
  const auto nearest_clamped = render_context.cached_sampler_handle(NearestSamplerClamped);

  const auto vsm_ctx = variables.context;
  const auto ps_ctx = variables.pointspot_context;

  context.directional_clipmaps_buffer = module.transient_buffer({
    .size = sizeof(DirectionalClipmaps),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "vsm directional clipmaps",
  });

  context.directional_clipmaps_buffer = module.update_buffer_from(
    context.directional_clipmaps_buffer,
    variables.directional_clipmaps
  );
  context.pointspot_views_buffer = module.transient_buffer({
    .size = sizeof(PointSpotViews),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "vsm pointspot views",
  });

  context.pointspot_views_buffer = module.update_buffer_from(context.pointspot_views_buffer, variables.pointspot_views);

  auto page_occupancy = module.transient_buffer({
    .size = RMVSMContext::PHYSICAL_PAGE_COUNT * sizeof(u32),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "vsm page occupancy",
  });
  page_occupancy = module.fill_buffer(page_occupancy, 0);
  context.pointspot_layer_dirty_mask_buffer = module.transient_buffer({
    .size = RMVSMContext::POINT_SPOT_LAYER_COUNT * sizeof(u32),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "vsm pointspot layer dirty mask",
  });
  context.pointspot_layer_dirty_mask_buffer = module.fill_buffer(context.pointspot_layer_dirty_mask_buffer, 0);

  const auto clipmap_dirty_flags_value = std::array<u32, RMVSMContext::MAX_DIRECTIONAL_CLIPMAP_COUNT>{};
  auto clipmap_dirty_flags = module.transient_buffer({
    .size = sizeof(clipmap_dirty_flags_value),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "vsm clipmap dirty flags",
  });
  clipmap_dirty_flags = module.update_buffer(
    clipmap_dirty_flags,
    std::span{reinterpret_cast<const u8*>(&clipmap_dirty_flags_value), sizeof(clipmap_dirty_flags_value)}
  );
  const auto clear_dirty_pages_command = VkDispatchIndirectCommand{
    .x = RMVSMContext::PAGE_SIZE / 16,
    .y = RMVSMContext::PAGE_SIZE / 16,
    .z = 0
  };
  auto clear_dirty_pages_cmd = module.transient_buffer({
    .size = sizeof(clear_dirty_pages_command),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "vsm clear dirty pages command",
  });
  clear_dirty_pages_cmd = module.update_buffer(
    clear_dirty_pages_cmd,
    std::span{reinterpret_cast<const u8*>(&clear_dirty_pages_command), sizeof(clear_dirty_pages_command)}
  );

  // the allocator points at buffers whose addresses are only known once the frame allocated them,
  // so it is written from inside the program. They are only ever reached through the allocator, which
  // carries the ordering for them
  auto allocation_requests = module.transient_buffer({
    .size = RMVSMContext::MAX_ALLOC_REQUEST_COUNT * sizeof(GPU::VSMAllocRequest),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "vsm allocation requests",
  });
  auto dirty_page_coords = module.transient_buffer({
    .size = RMVSMContext::PHYSICAL_PAGE_COUNT * sizeof(glm::uvec2),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "vsm dirty physical pages",
  });
  auto free_page_list = module.transient_buffer({
    .size = RMVSMContext::PHYSICAL_PAGE_COUNT * sizeof(u32),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "vsm free page list",
  });
  auto page_allocator = module.transient_buffer({
    .size = sizeof(GPU::VSMPageAllocator),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "vsm page allocator",
  });
  const auto write_allocator = module.declare_callback_var(
    "vsm write page allocator",
    [page_allocator, allocation_requests, dirty_page_coords, free_page_list](vir::Recorder& recorder) -> void {
      const auto allocator = GPU::VSMPageAllocator{
        .request_capacity = RMVSMContext::MAX_ALLOC_REQUEST_COUNT,
        .requests = recorder.buffer(allocation_requests).device_address,
        .dirty_physical_page_coords = recorder.buffer(dirty_page_coords).device_address,
        .free_page_list = recorder.buffer(free_page_list).device_address,
      };
      recorder.update_buffer(
        recorder.buffer(page_allocator),
        std::span(reinterpret_cast<const u8*>(&allocator), sizeof(allocator))
      );
    }
  );
  page_allocator = //
    module
      .begin_compute(
        {{page_allocator, vir::Access::Clear},
         {allocation_requests, vir::Access::CopyRead},
         {dirty_page_coords, vir::Access::CopyRead},
         {free_page_list, vir::Access::CopyRead}}
      )
      .with_name("vsm write page allocator")
      .record_from(write_allocator)
      .end_compute()[0];

  // the sun moving invalidates every cached directional page
  context.virtual_page_table_attachment = module.set_condition(
    variables.is_sun_moved,
    [&](vir::Module& arm) {
      return arm.clear(context.virtual_page_table_attachment, vir::ClearValue::rgba_u32(0, 0, 0, 1));
    },
    [&](vir::Module&) { return context.virtual_page_table_attachment; }
  );

  constexpr auto directional_layers = RMVSMContext::MAX_DIRECTIONAL_CLIPMAP_COUNT;
  if (is_directional) {
    auto scroll_deltas = module.transient_buffer({
      .size = sizeof(ClipmapScrollDeltas),
      .usage = TRANSIENT_BUFFER_USAGE,
      .location = vir::MemoryLocation::GpuOnly,
      .name = "vsm clipmap scroll deltas",
    });
    scroll_deltas = module.update_buffer_from(scroll_deltas, variables.clipmap_scroll_deltas);
    std::tie(context.virtual_page_table_attachment, context.directional_clipmaps_buffer, std::ignore) = std::tuple_cat(
      module
        .begin_compute(
          {{context.virtual_page_table_attachment, vir::Access::ComputeRW},
           {context.directional_clipmaps_buffer, vir::Access::ComputeRead},
           {scroll_deltas, vir::Access::ComputeRead}}
        )
        .with_name("vsm reset page visibility")
        .bind_compute_pipeline(render_context.pipeline("rmvsm_reset_page_visibility"))
        .bind_image(0, 0, context.virtual_page_table_attachment)
        .bind_buffer(0, 1, context.directional_clipmaps_buffer)
        .bind_buffer(0, 2, scroll_deltas)
        .push_constants_from(vsm_ctx)
        .dispatch_invocations_per_pixel(
          context.virtual_page_table_attachment,
          {1.0f, 1.0f, static_cast<f32>(directional_layers)}
        )
        .end_compute()
    );
  }

  if (is_pointspot) {
    {
      auto mips = std::array<vir::ValueID, RMVSMContext::POINT_SPOT_MIP_COUNT>{};
      auto accesses = std::array<vir::ResourceAccess, RMVSMContext::POINT_SPOT_MIP_COUNT>{};
      for (auto mip = 0_u32; mip < RMVSMContext::POINT_SPOT_MIP_COUNT; mip++) {
        mips[mip] = module.mip(context.pointspot_page_table_attachment, mip);
        accesses[mip] = {mips[mip], vir::Access::ComputeRW};
      }
      auto pass = bind_vsm_pointspot_spec_constants(
        module.begin_compute(accesses)
          .with_name("vsm pointspot reset page visibility")
          .bind_compute_pipeline(render_context.pipeline("rmvsm_pointspot_reset_page_visibility"))
      );
      for (auto mip = 0_u32; mip < RMVSMContext::POINT_SPOT_MIP_COUNT; mip++) {
        module.bind_image(0, mip, mips[mip]);
      }
      module.push_constants_from(variables.pointspot_moved_lights)
        .dispatch_invocations(
          RMVSMContext::POINT_SPOT_PAGE_TABLE_SIZE,
          RMVSMContext::POINT_SPOT_PAGE_TABLE_SIZE,
          RMVSMContext::POINT_SPOT_LAYER_COUNT
        );
      const auto results = pass.end_compute();
      context.pointspot_page_table_attachment = module.whole(results[RMVSMContext::POINT_SPOT_MIP_COUNT - 1]);
    }

    {
      auto mips = std::array<vir::ValueID, RMVSMContext::POINT_SPOT_MIP_COUNT>{};
      auto accesses = std::array<vir::ResourceAccess, RMVSMContext::POINT_SPOT_MIP_COUNT + 6>{};
      for (auto mip = 0_u32; mip < RMVSMContext::POINT_SPOT_MIP_COUNT; mip++) {
        mips[mip] = module.mip(context.pointspot_page_table_attachment, mip);
        accesses[mip] = {mips[mip], vir::Access::ComputeRW};
      }
      accesses[RMVSMContext::POINT_SPOT_MIP_COUNT] = {values.dirty_mesh_instances, vir::Access::ComputeRead};
      accesses[RMVSMContext::POINT_SPOT_MIP_COUNT + 1] = {values.mesh_instances, vir::Access::ComputeRead};
      accesses[RMVSMContext::POINT_SPOT_MIP_COUNT + 2] = {values.meshes, vir::Access::ComputeRead};
      accesses[RMVSMContext::POINT_SPOT_MIP_COUNT + 3] = {values.transforms_world, vir::Access::ComputeRead};
      accesses[RMVSMContext::POINT_SPOT_MIP_COUNT + 4] = {values.transforms_previous, vir::Access::ComputeRead};
      accesses[RMVSMContext::POINT_SPOT_MIP_COUNT + 5] = {context.pointspot_views_buffer, vir::Access::ComputeRead};
      auto pass = bind_vsm_pointspot_spec_constants(
        module.begin_compute(accesses)
          .with_name("vsm pointspot invalidate pages")
          .bind_compute_pipeline(render_context.pipeline("rmvsm_pointspot_invalidate_pages"))
      );
      for (auto mip = 0_u32; mip < RMVSMContext::POINT_SPOT_MIP_COUNT; mip++) {
        module.bind_image(0, mip, mips[mip]);
      }
      module.bind_buffer(0, 6, values.dirty_mesh_instances)
        .bind_buffer(0, 7, values.mesh_instances)
        .bind_buffer(0, 8, values.meshes)
        .bind_buffer(0, 9, values.transforms_world)
        .bind_buffer(0, 10, values.transforms_previous)
        .bind_buffer(0, 11, context.pointspot_views_buffer)
        .push_constants_from(ps_ctx)
        .dispatch_invocations(variables.pointspot_dirty_count, 16, RMVSMContext::POINT_SPOT_LAYER_COUNT);
      const auto results = pass.end_compute();
      context.pointspot_page_table_attachment = module.whole(results[RMVSMContext::POINT_SPOT_MIP_COUNT - 1]);
      values.dirty_mesh_instances = results[RMVSMContext::POINT_SPOT_MIP_COUNT];
      values.mesh_instances = results[RMVSMContext::POINT_SPOT_MIP_COUNT + 1];
      values.meshes = results[RMVSMContext::POINT_SPOT_MIP_COUNT + 2];
      values.transforms_world = results[RMVSMContext::POINT_SPOT_MIP_COUNT + 3];
      values.transforms_previous = results[RMVSMContext::POINT_SPOT_MIP_COUNT + 4];
      context.pointspot_views_buffer = results[RMVSMContext::POINT_SPOT_MIP_COUNT + 5];
    }

    {
      auto mips = std::array<vir::ValueID, RMVSMContext::POINT_SPOT_MIP_COUNT>{};
      auto accesses = std::array<vir::ResourceAccess, RMVSMContext::POINT_SPOT_MIP_COUNT + 2>{};
      for (auto mip = 0_u32; mip < RMVSMContext::POINT_SPOT_MIP_COUNT; mip++) {
        mips[mip] = module.mip(context.pointspot_page_table_attachment, mip);
        accesses[mip] = {mips[mip], vir::Access::ComputeRW};
      }
      accesses[RMVSMContext::POINT_SPOT_MIP_COUNT] = {values.removed_mesh_bounds, vir::Access::ComputeRead};
      accesses[RMVSMContext::POINT_SPOT_MIP_COUNT + 1] = {context.pointspot_views_buffer, vir::Access::ComputeRead};
      auto pass = bind_vsm_pointspot_spec_constants(
        module.begin_compute(accesses)
          .with_name("vsm pointspot invalidate removed")
          .bind_compute_pipeline(render_context.pipeline("rmvsm_pointspot_invalidate_pages_bounds"))
      );
      for (auto mip = 0_u32; mip < RMVSMContext::POINT_SPOT_MIP_COUNT; mip++) {
        module.bind_image(0, mip, mips[mip]);
      }
      module.bind_buffer(0, 11, context.pointspot_views_buffer)
        .bind_buffer(0, 12, values.removed_mesh_bounds)
        .push_constants_from(ps_ctx)
        .dispatch_invocations(variables.pointspot_removed_count, 16, RMVSMContext::POINT_SPOT_LAYER_COUNT);
      const auto results = pass.end_compute();
      context.pointspot_page_table_attachment = module.whole(results[RMVSMContext::POINT_SPOT_MIP_COUNT - 1]);
      values.removed_mesh_bounds = results[RMVSMContext::POINT_SPOT_MIP_COUNT];
      context.pointspot_views_buffer = results[RMVSMContext::POINT_SPOT_MIP_COUNT + 1];
    }
  }

  if (is_directional) {
    // a moved sun already cleared the whole table, so there is nothing left to invalidate
    std::tie(
      context.virtual_page_table_attachment,
      values.dirty_mesh_instances,
      values.mesh_instances,
      values.meshes,
      values.transforms_world,
      values.transforms_previous,
      context.directional_clipmaps_buffer
    ) = std::tuple_cat( //
      module
        .begin_compute(
          {{context.virtual_page_table_attachment, vir::Access::ComputeRW},
           {values.dirty_mesh_instances, vir::Access::ComputeRead},
           {values.mesh_instances, vir::Access::ComputeRead},
           {values.meshes, vir::Access::ComputeRead},
           {values.transforms_world, vir::Access::ComputeRead},
           {values.transforms_previous, vir::Access::ComputeRead},
           {context.directional_clipmaps_buffer, vir::Access::ComputeRead}}
        )
        .with_name("vsm invalidate pages")
        .bind_compute_pipeline(render_context.pipeline("rmvsm_invalidate_pages"))
        .bind_image(0, 0, context.virtual_page_table_attachment)
        .bind_buffer(0, 1, values.dirty_mesh_instances)
        .bind_buffer(0, 2, values.mesh_instances)
        .bind_buffer(0, 3, values.meshes)
        .bind_buffer(0, 4, values.transforms_world)
        .bind_buffer(0, 5, context.directional_clipmaps_buffer)
        .bind_buffer(0, 6, values.transforms_previous)
        .push_constants_from(vsm_ctx)
        .dispatch_invocations(variables.dirty_count, RMVSMContext::DIRECTIONAL_PAGE_TABLE_SIZE, directional_layers)
        .end_compute()
    );

    std::tie(context.virtual_page_table_attachment, values.removed_mesh_bounds, context.directional_clipmaps_buffer) =
      std::tuple_cat(
        module
          .begin_compute(
            {{context.virtual_page_table_attachment, vir::Access::ComputeRW},
             {values.removed_mesh_bounds, vir::Access::ComputeRead},
             {context.directional_clipmaps_buffer, vir::Access::ComputeRead}}
          )
          .with_name("vsm invalidate removed")
          .bind_compute_pipeline(render_context.pipeline("rmvsm_invalidate_pages_bounds"))
          .bind_image(0, 0, context.virtual_page_table_attachment)
          .bind_buffer(0, 5, context.directional_clipmaps_buffer)
          .bind_buffer(0, 7, values.removed_mesh_bounds)
          .push_constants_from(vsm_ctx)
          .dispatch_invocations(variables.removed_count, RMVSMContext::DIRECTIONAL_PAGE_TABLE_SIZE, directional_layers)
          .end_compute()
      );

    std::tie(
      values.camera,
      context.directional_clipmaps_buffer,
      context.depth_attachment,
      context.normal_attachment,
      context.virtual_page_table_attachment,
      page_occupancy,
      page_allocator
    ) = std::tuple_cat( //
      module
        .begin_compute(
          {{values.camera, vir::Access::ComputeUniformRead},
           {context.directional_clipmaps_buffer, vir::Access::ComputeRead},
           {context.depth_attachment, vir::Access::ComputeSampled},
           {context.normal_attachment, vir::Access::ComputeSampled},
           {context.virtual_page_table_attachment, vir::Access::ComputeRW},
           {page_occupancy, vir::Access::ComputeRW},
           {page_allocator, vir::Access::ComputeRW}}
        )
        .with_name("vsm mark visible pages")
        .bind_compute_pipeline(render_context.pipeline("rmvsm_mark_visible_pages"))
        .bind_buffer(0, 0, values.camera)
        .bind_buffer(0, 1, context.directional_clipmaps_buffer)
        .bind_image(0, 2, context.depth_attachment)
        .bind_image(0, 3, context.normal_attachment)
        .bind_image(0, 4, context.virtual_page_table_attachment)
        .bind_buffer(0, 5, page_occupancy)
        .bind_buffer(0, 6, page_allocator)
        .push_constants_from(vsm_ctx)
        .dispatch_invocations_per_pixel(context.depth_attachment)
        .end_compute()
    );
  }

  if (is_pointspot) {
    {
      auto mips = std::array<vir::ValueID, RMVSMContext::POINT_SPOT_MIP_COUNT>{};
      auto accesses = std::array<vir::ResourceAccess, RMVSMContext::POINT_SPOT_MIP_COUNT + 6>{};
      for (auto mip = 0_u32; mip < RMVSMContext::POINT_SPOT_MIP_COUNT; mip++) {
        mips[mip] = module.mip(context.pointspot_page_table_attachment, mip);
        accesses[mip] = {mips[mip], vir::Access::ComputeRW};
      }
      accesses[RMVSMContext::POINT_SPOT_MIP_COUNT] = {values.camera, vir::Access::ComputeUniformRead};
      accesses[RMVSMContext::POINT_SPOT_MIP_COUNT + 1] = {context.pointspot_views_buffer, vir::Access::ComputeRead};
      accesses[RMVSMContext::POINT_SPOT_MIP_COUNT + 2] = {context.light_grid_buffer, vir::Access::ComputeRead};
      accesses[RMVSMContext::POINT_SPOT_MIP_COUNT + 3] = {context.depth_attachment, vir::Access::ComputeSampled};
      accesses[RMVSMContext::POINT_SPOT_MIP_COUNT + 4] = {page_occupancy, vir::Access::ComputeRW};
      accesses[RMVSMContext::POINT_SPOT_MIP_COUNT + 5] = {page_allocator, vir::Access::ComputeRW};
      auto pass = bind_vsm_pointspot_spec_constants(
        module.begin_compute(accesses)
          .with_name("vsm pointspot mark visible pages")
          .bind_compute_pipeline(render_context.pipeline("rmvsm_mark_visible_pages_pointspot"))
      );
      for (auto mip = 0_u32; mip < RMVSMContext::POINT_SPOT_MIP_COUNT; mip++) {
        module.bind_image(0, 4 + mip, mips[mip]);
      }
      module.bind_buffer(0, 0, values.camera)
        .bind_buffer(0, 1, context.pointspot_views_buffer)
        .bind_buffer(0, 2, context.light_grid_buffer)
        .bind_image(0, 3, context.depth_attachment)
        .bind_buffer(0, 10, page_occupancy)
        .bind_buffer(0, 11, page_allocator)
        .push_constants_from(variables.pointspot_mark_push)
        .dispatch_invocations_per_pixel(context.depth_attachment);
      const auto results = pass.end_compute();
      context.pointspot_page_table_attachment = module.whole(results[RMVSMContext::POINT_SPOT_MIP_COUNT - 1]);
      values.camera = results[RMVSMContext::POINT_SPOT_MIP_COUNT];
      context.pointspot_views_buffer = results[RMVSMContext::POINT_SPOT_MIP_COUNT + 1];
      context.light_grid_buffer = results[RMVSMContext::POINT_SPOT_MIP_COUNT + 2];
      context.depth_attachment = results[RMVSMContext::POINT_SPOT_MIP_COUNT + 3];
      page_occupancy = results[RMVSMContext::POINT_SPOT_MIP_COUNT + 4];
      page_allocator = results[RMVSMContext::POINT_SPOT_MIP_COUNT + 5];
    }

    const auto coarse_mip = module.mip(context.pointspot_page_table_attachment, RMVSMContext::POINT_SPOT_MIP_COUNT - 1);
    auto pass = //
      module
        .begin_compute(
          {{context.pointspot_views_buffer, vir::Access::ComputeRead},
           {coarse_mip, vir::Access::ComputeRW},
           {page_occupancy, vir::Access::ComputeRW},
           {page_allocator, vir::Access::ComputeRW}}
        )
        .with_name("vsm pointspot mark coarse pages")
        .bind_compute_pipeline(render_context.pipeline("rmvsm_pointspot_mark_coarse_pages"));
    bind_vsm_pointspot_spec_constants(pass)
      .bind_buffer(0, 0, context.pointspot_views_buffer)
      .bind_image(0, 1, coarse_mip)
      .bind_buffer(0, 2, page_occupancy)
      .bind_buffer(0, 3, page_allocator)
      .dispatch_invocations(RMVSMContext::POINT_SPOT_LAYER_COUNT, 1, 1);
    const auto coarse = pass.end_compute();
    context.pointspot_views_buffer = coarse[0];
    context.pointspot_page_table_attachment = module.whole(coarse[1]);
    page_occupancy = coarse[2];
    page_allocator = coarse[3];
  }

  if (is_directional) {
    context.virtual_page_table_attachment = //
      module.begin_compute({{context.virtual_page_table_attachment, vir::Access::ComputeRW}})
        .with_name("vsm free invisible pages")
        .bind_compute_pipeline(render_context.pipeline("rmvsm_free_invisible_pages"))
        .bind_image(0, 0, context.virtual_page_table_attachment)
        .push_constants_from(vsm_ctx)
        .dispatch_invocations_per_pixel(
          context.virtual_page_table_attachment,
          {1.0f, 1.0f, static_cast<f32>(directional_layers)}
        )
        .end_compute()[0];
  }

  if (is_pointspot) {
    {
      auto mips = std::array<vir::ValueID, RMVSMContext::POINT_SPOT_MIP_COUNT>{};
      auto accesses = std::array<vir::ResourceAccess, RMVSMContext::POINT_SPOT_MIP_COUNT>{};
      for (auto mip = 0_u32; mip < RMVSMContext::POINT_SPOT_MIP_COUNT; mip++) {
        mips[mip] = module.mip(context.pointspot_page_table_attachment, mip);
        accesses[mip] = {mips[mip], vir::Access::ComputeRW};
      }
      auto pass = bind_vsm_pointspot_spec_constants(
        module.begin_compute(accesses)
          .with_name("vsm pointspot free invisible pages")
          .bind_compute_pipeline(render_context.pipeline("rmvsm_pointspot_free_invisible_pages"))
      );
      for (auto mip = 0_u32; mip < RMVSMContext::POINT_SPOT_MIP_COUNT; mip++) {
        module.bind_image(0, mip, mips[mip]);
      }
      module.dispatch_invocations(
        RMVSMContext::POINT_SPOT_PAGE_TABLE_SIZE,
        RMVSMContext::POINT_SPOT_PAGE_TABLE_SIZE,
        RMVSMContext::POINT_SPOT_LAYER_COUNT
      );
      const auto results = pass.end_compute();
      context.pointspot_page_table_attachment = module.whole(results[RMVSMContext::POINT_SPOT_MIP_COUNT - 1]);
    }
  }

  std::tie(page_allocator, page_occupancy) = std::tuple_cat(
    module.begin_compute({{page_allocator, vir::Access::ComputeRW}, {page_occupancy, vir::Access::ComputeRead}})
      .with_name("vsm build free page list")
      .bind_compute_pipeline(render_context.pipeline("rmvsm_build_free_page_list"))
      .bind_buffer(0, 0, page_allocator)
      .bind_buffer(0, 1, page_occupancy)
      .push_constants(RMVSMContext::PHYSICAL_PAGE_COUNT)
      .dispatch_invocations(RMVSMContext::PHYSICAL_PAGE_COUNT, 1, 1)
      .end_compute()
  );

  {
    auto mips = std::array<vir::ValueID, RMVSMContext::POINT_SPOT_MIP_COUNT>{};
    auto accesses = std::array<vir::ResourceAccess, 2 + RMVSMContext::POINT_SPOT_MIP_COUNT>{};
    accesses[0] = {page_allocator, vir::Access::ComputeRW};
    accesses[1] = {context.virtual_page_table_attachment, vir::Access::ComputeRW};
    for (auto mip = 0_u32; mip < RMVSMContext::POINT_SPOT_MIP_COUNT; mip++) {
      mips[mip] = module.mip(context.pointspot_page_table_attachment, mip);
      accesses[2 + mip] = {mips[mip], vir::Access::ComputeRW};
    }
    auto pass = //
      module.begin_compute(accesses)
        .with_name("vsm allocate pages")
        .bind_compute_pipeline(render_context.pipeline("rmvsm_allocate_pages"))
        .bind_buffer(0, 0, page_allocator)
        .bind_image(0, 1, context.virtual_page_table_attachment);
    for (auto mip = 0_u32; mip < RMVSMContext::POINT_SPOT_MIP_COUNT; mip++) {
      module.bind_image(0, 2 + mip, mips[mip]);
    }

    module.dispatch_invocations(RMVSMContext::MAX_ALLOC_REQUEST_COUNT, 1, 1);
    const auto results = pass.end_compute();
    page_allocator = results[0];
    context.virtual_page_table_attachment = results[1];
    context.pointspot_page_table_attachment = module.whole(results.back());
  }

  auto hpb = vir::ValueID::Invalid;
  if (is_directional) {
    // one occupancy pyramid per clipmap layer, built one level per pass so every level reads the one
    // before it with the barrier the graph puts between them
    const auto hpb_levels = 1 + static_cast<u32>(std::log2(RMVSMContext::DIRECTIONAL_PAGE_TABLE_SIZE));
    hpb = module.transient_image({
      .extent = {RMVSMContext::DIRECTIONAL_PAGE_TABLE_SIZE, RMVSMContext::DIRECTIONAL_PAGE_TABLE_SIZE, 1},
      .format = vir::Format::R8Uint,
      .view_type = vir::ImageViewType::e2DArray,
      .mip_levels = hpb_levels,
      .array_layers = RMVSMContext::MAX_DIRECTIONAL_CLIPMAP_COUNT,
      .name = "vsm hpb",
    });
    for (auto i = 0_u32; i < hpb_levels; i++) {
      const auto level_extent = vir::Extent3D{
        std::max(1u, RMVSMContext::DIRECTIONAL_PAGE_TABLE_SIZE >> i),
        std::max(1u, RMVSMContext::DIRECTIONAL_PAGE_TABLE_SIZE >> i),
        directional_layers,
      };
      const auto src = module.mip(hpb, i == 0 ? 0 : i - 1);
      const auto dst = module.mip(hpb, i);
      const auto accesses = std::array<vir::ResourceAccess, 3>{{
        {context.virtual_page_table_attachment, vir::Access::ComputeSampled},
        {dst, vir::Access::ComputeRW},
        {src, vir::Access::ComputeSampled},
      }};
      module.begin_compute(std::span(accesses).first(i > 0 ? 3 : 2))
        .with_name("vsm downsample hpb")
        .bind_compute_pipeline(render_context.pipeline("rmvsm_downsample_hpb"))
        .specialize_constant(0, i == 0)
        .bind_image(0, 0, context.virtual_page_table_attachment)
        .bind_image(0, 1, i == 0 ? dst : src)
        .bind_image(0, 2, dst)
        .push_constants(level_extent)
        .dispatch_invocations(level_extent.width, level_extent.height, RMVSMContext::MAX_DIRECTIONAL_CLIPMAP_COUNT);
      const auto results = module.end_compute_values();
      context.virtual_page_table_attachment = results[0];
      hpb = module.whole(results[1]);
    }

    std::tie(
      context.virtual_page_table_attachment,
      page_allocator,
      clear_dirty_pages_cmd,
      clipmap_dirty_flags
    ) = std::tuple_cat( //
      module
        .begin_compute(
          {{context.virtual_page_table_attachment, vir::Access::ComputeSampled},
           {page_allocator, vir::Access::ComputeRW},
           {clear_dirty_pages_cmd, vir::Access::ComputeRW},
           {clipmap_dirty_flags, vir::Access::ComputeRW}}
        )
        .with_name("vsm mark dirty pages")
        .bind_compute_pipeline(render_context.pipeline("rmvsm_mark_dirty_pages"))
        .bind_image(0, 0, context.virtual_page_table_attachment)
        .bind_buffer(0, 1, clear_dirty_pages_cmd)
        .bind_buffer(0, 2, page_allocator)
        .bind_buffer(0, 3, clipmap_dirty_flags)
        .push_constants_from(vsm_ctx)
        .dispatch_invocations_per_pixel(
          context.virtual_page_table_attachment,
          {1.0f, 1.0f, static_cast<f32>(directional_layers)}
        )
        .end_compute()
    );
  }

  if (is_pointspot) {
    auto pass = //
      module
        .begin_compute(
          {{context.pointspot_page_table_attachment, vir::Access::ComputeSampled},
           {clear_dirty_pages_cmd, vir::Access::ComputeRW},
           {page_allocator, vir::Access::ComputeRW},
           {context.pointspot_layer_dirty_mask_buffer, vir::Access::ComputeRW}}
        )
        .with_name("vsm pointspot mark dirty pages")
        .bind_compute_pipeline(render_context.pipeline("rmvsm_pointspot_mark_dirty_pages"));
    bind_vsm_pointspot_spec_constants(pass)
      .bind_image(0, 0, context.pointspot_page_table_attachment)
      .bind_buffer(0, 1, clear_dirty_pages_cmd)
      .bind_buffer(0, 2, page_allocator)
      .bind_buffer(0, 3, context.pointspot_layer_dirty_mask_buffer)
      .push_constants_from(ps_ctx)
      .dispatch_invocations(
        RMVSMContext::POINT_SPOT_PAGE_TABLE_SIZE,
        RMVSMContext::POINT_SPOT_PAGE_TABLE_SIZE,
        RMVSMContext::POINT_SPOT_LAYER_COUNT
      );
    std::tie(
      context.pointspot_page_table_attachment,
      clear_dirty_pages_cmd,
      page_allocator,
      context.pointspot_layer_dirty_mask_buffer
    ) = std::tuple_cat(pass.end_compute());
  }

  std::tie(std::ignore, page_allocator, context.physical_page_table_attachment) = std::tuple_cat(
    module
      .begin_compute(
        {{clear_dirty_pages_cmd, vir::Access::IndirectRead},
         {page_allocator, vir::Access::ComputeUniformRead},
         {context.physical_page_table_attachment, vir::Access::ComputeRW}}
      )
      .with_name("vsm clear dirty pages")
      .bind_compute_pipeline(render_context.pipeline("rmvsm_clear_dirty_pages"))
      .bind_buffer(0, 0, page_allocator)
      .bind_image(0, 1, context.physical_page_table_attachment)
      .push_constants_from(vsm_ctx)
      .dispatch_indirect(clear_dirty_pages_cmd)
      .end_compute()
  );

  const auto physical_extent = vir::Extent2D{
    RMVSMContext::DIRECTIONAL_IMAGE_RESOLUTION,
    RMVSMContext::DIRECTIONAL_IMAGE_RESOLUTION,
  };

  if (is_directional) {
    auto cull_context = CullGeometryContext{
      .use_hiz = false,
      .use_hpb = true,
      .init_cull_meshes = true,
      .cull_flags = GPU::CullFlag::TestFrustum | GPU::CullFlag::ShadowCastersOnly,
      .vsm_clipmaps_buffer = context.directional_clipmaps_buffer,
      .vsm_clipmap_dirty_flags_buffer = clipmap_dirty_flags,
      .hpb_attachment = hpb,
    };
    cull_geometry(module, render_context, recording, cull_context);
    context.directional_clipmaps_buffer = cull_context.vsm_clipmaps_buffer;
    clipmap_dirty_flags = cull_context.vsm_clipmap_dirty_flags_buffer;
    hpb = cull_context.hpb_attachment;

    const auto clipmap_vsm_ctx = variables.clipmap_context;
    const auto command_stride = use_mesh_shaders ? sizeof(VkDrawMeshTasksIndirectCommandEXT)
                                                 : sizeof(VkDrawIndexedIndirectCommand);
    auto draw_commands = module.transient_buffer({
      .size = RMVSMContext::MAX_DIRECTIONAL_CLIPMAP_COUNT * command_stride,
      .usage = TRANSIENT_BUFFER_USAGE,
      .location = vir::MemoryLocation::GpuOnly,
      .name = "vsm draw commands",
    });
    draw_commands = module.fill_buffer(draw_commands, 0);
    const auto draw_count_value = 0_u32;
    auto draw_count = module.transient_buffer({
      .size = sizeof(draw_count_value),
      .usage = TRANSIENT_BUFFER_USAGE,
      .location = vir::MemoryLocation::GpuOnly,
      .name = "vsm draw count",
    });
    draw_count = module.update_buffer(
      draw_count,
      std::span{reinterpret_cast<const u8*>(&draw_count_value), sizeof(draw_count_value)}
    );
    const auto draw_clipmaps_value = std::array<u32, RMVSMContext::MAX_DIRECTIONAL_CLIPMAP_COUNT>{};
    auto draw_clipmaps = module.transient_buffer({
      .size = sizeof(draw_clipmaps_value),
      .usage = TRANSIENT_BUFFER_USAGE,
      .location = vir::MemoryLocation::GpuOnly,
      .name = "vsm draw clipmaps",
    });
    draw_clipmaps = module.update_buffer(
      draw_clipmaps,
      std::span{reinterpret_cast<const u8*>(&draw_clipmaps_value), sizeof(draw_clipmaps_value)}
    );

    std::tie(std::ignore, clipmap_dirty_flags, draw_commands, draw_count, draw_clipmaps) = std::tuple_cat(
      module
        .begin_compute(
          {{cull_context.draw_geometry_cmd_buffer, vir::Access::ComputeRead},
           {clipmap_dirty_flags, vir::Access::ComputeRead},
           {draw_commands, vir::Access::ComputeRW},
           {draw_count, vir::Access::ComputeRW},
           {draw_clipmaps, vir::Access::ComputeRW}}
        )
        .with_name(use_mesh_shaders ? "vsm build mesh draw commands" : "vsm build indexed draw commands")
        .bind_compute_pipeline(render_context.pipeline("rmvsm_build_draw_commands"))
        .bind_buffer(0, 0, cull_context.draw_geometry_cmd_buffer)
        .bind_buffer(0, 1, clipmap_dirty_flags)
        .bind_buffer(0, 2, draw_commands)
        .bind_buffer(0, 3, draw_count)
        .bind_buffer(0, 4, draw_clipmaps)
        .push_constants(
          glm::uvec2(RMVSMContext::MAX_DIRECTIONAL_CLIPMAP_COUNT, static_cast<u32>(command_stride / sizeof(u32)))
        )
        .dispatch(1, 1, 1)
        .end_compute()
    );

    if (use_mesh_shaders) {
      static constexpr auto geometry_access = vir::Access::TaskRead | vir::Access::MeshRead;
      std::tie(
        std::ignore,
        std::ignore,
        std::ignore,
        values.meshes,
        values.mesh_instances,
        values.meshlet_instances,
        values.transforms_world,
        std::ignore,
        values.materials,
        context.directional_clipmaps_buffer,
        std::ignore,
        context.virtual_page_table_attachment,
        context.physical_page_table_attachment
      ) = std::tuple_cat( //
        module
          .begin_rendering_area(
            {{draw_commands, vir::Access::IndirectRead},
             {draw_count, vir::Access::IndirectRead},
             {draw_clipmaps, geometry_access},
             {values.meshes, geometry_access},
             {values.mesh_instances, geometry_access},
             {values.meshlet_instances, geometry_access},
             {values.transforms_world, geometry_access},
             {cull_context.visibility_buffer, geometry_access},
             {values.materials, vir::Access::FragmentRead},
             {context.directional_clipmaps_buffer, geometry_access | vir::Access::FragmentRead},
             {hpb, vir::Access::TaskSampled | vir::Access::MeshSampled},
             {context.virtual_page_table_attachment, vir::Access::FragmentSampled},
             {context.physical_page_table_attachment, vir::Access::FragmentRW}},
            physical_extent
          )
          .with_name("vsm draw dirty clipmaps ms")
          .bind_graphics_pipeline(render_context.pipeline("rmvsm_draw_physical_pages_ms"))
          .set_viewport(0, vir::Rect2D::framebuffer())
          .set_scissor(0, vir::Rect2D::framebuffer())
          .bind_buffer(0, 0, values.meshes)
          .bind_buffer(0, 1, values.mesh_instances)
          .bind_buffer(0, 2, values.meshlet_instances)
          .bind_buffer(0, 3, values.transforms_world)
          .bind_buffer(0, 4, values.materials)
          .bind_buffer(0, 5, context.directional_clipmaps_buffer)
          .bind_image(0, 6, context.virtual_page_table_attachment)
          .bind_image(0, 7, context.physical_page_table_attachment, vir::Format::R32Uint)
          .bind_buffer(0, 8, cull_context.visibility_buffer)
          .bind_image(0, 9, hpb)
          .bind_sampler(0, 10, nearest_clamped)
          .bind_buffer(0, 11, draw_clipmaps)
          .push_constants_from(clipmap_vsm_ctx)
          .draw_mesh_tasks_indirect_count(draw_commands, 0, draw_count, 0, RMVSMContext::MAX_DIRECTIONAL_CLIPMAP_COUNT)
          .end_rendering()
      );
    } else {
      std::tie(
        std::ignore,
        std::ignore,
        std::ignore,
        values.reordered_indices,
        values.meshes,
        values.mesh_instances,
        values.meshlet_instances,
        values.transforms_world,
        values.materials,
        context.directional_clipmaps_buffer,
        context.virtual_page_table_attachment,
        context.physical_page_table_attachment
      ) =
        std::tuple_cat(
          module
            .begin_rendering_area(
              {{draw_commands, vir::Access::IndirectRead},
               {draw_count, vir::Access::IndirectRead},
               {draw_clipmaps, vir::Access::VertexRead},
               {values.reordered_indices, vir::Access::IndexRead},
               {values.meshes, vir::Access::VertexRead},
               {values.mesh_instances, vir::Access::VertexRead},
               {values.meshlet_instances, vir::Access::VertexRead},
               {values.transforms_world, vir::Access::VertexRead},
               {values.materials, vir::Access::FragmentRead},
               {context.directional_clipmaps_buffer, vir::Access::VertexRead | vir::Access::FragmentRead},
               {context.virtual_page_table_attachment, vir::Access::FragmentSampled},
               {context.physical_page_table_attachment, vir::Access::FragmentRW}},
              physical_extent
            )
            .with_name("vsm draw dirty clipmaps")
            .bind_graphics_pipeline(render_context.pipeline("rmvsm_draw_physical_pages"))
            .set_viewport(0, vir::Rect2D::framebuffer())
            .set_scissor(0, vir::Rect2D::framebuffer())
            .bind_buffer(0, 0, values.meshes)
            .bind_buffer(0, 1, values.mesh_instances)
            .bind_buffer(0, 2, values.meshlet_instances)
            .bind_buffer(0, 3, values.transforms_world)
            .bind_buffer(0, 4, values.materials)
            .bind_buffer(0, 5, context.directional_clipmaps_buffer)
            .bind_image(0, 6, context.virtual_page_table_attachment)
            .bind_image(0, 7, context.physical_page_table_attachment, vir::Format::R32Uint)
            .bind_buffer(0, 8, draw_clipmaps)
            .bind_index_buffer(values.reordered_indices, VK_INDEX_TYPE_UINT32)
            .push_constants_from(clipmap_vsm_ctx)
            .draw_indexed_indirect_count(draw_commands, 0, draw_count, 0, RMVSMContext::MAX_DIRECTIONAL_CLIPMAP_COUNT)
            .end_rendering()
        );
    }
  }

  if (is_pointspot) {
    const auto pointspot_hpb = module.transient_image({
      .extent = {RMVSMContext::POINT_SPOT_PAGE_TABLE_SIZE, RMVSMContext::POINT_SPOT_PAGE_TABLE_SIZE, 1},
      .format = vir::Format::R8Uint,
      .view_type = vir::ImageViewType::e2DArray,
      .mip_levels = RMVSMContext::POINT_SPOT_HPB_LEVEL_COUNT,
      .array_layers = RMVSMContext::POINT_SPOT_LAYER_COUNT,
      .name = "vsm pointspot hpb",
    });
    auto cull_context = CullGeometryPointSpotContext{
      .views_buffer = context.pointspot_views_buffer,
      .layer_dirty_mask_buffer = context.pointspot_layer_dirty_mask_buffer,
      .page_table_attachment = context.pointspot_page_table_attachment,
      .hpb_attachment = pointspot_hpb,
    };

    for (auto mip = 0_u32; mip < RMVSMContext::POINT_SPOT_MIP_COUNT; mip++) {
      cull_context.init = mip == 0;
      cull_context.curr_mip = mip;
      cull_geometry_pointspot(module, render_context, recording, cull_context);

      const auto mip_ps_ctx = prepared.cull.pointspot_contexts[mip];
      const auto mip_extent = static_cast<u32>(RMVSMContext::POINT_SPOT_IMAGE_RESOLUTION) >> mip;
      if (use_mesh_shaders) {
        static constexpr auto geometry_access = vir::Access::TaskRead | vir::Access::MeshRead;
        auto pass = //
          module
            .begin_rendering_area(
              {{cull_context.draw_cmd_buffer, vir::Access::IndirectRead},
               {cull_context.vsm_meshlet_instance_count_buffer, geometry_access},
               {values.meshes, geometry_access},
               {values.mesh_instances, geometry_access},
               {values.transforms_world, geometry_access},
               {values.materials, vir::Access::FragmentRead},
               {cull_context.views_buffer, geometry_access},
               {cull_context.vsm_meshlet_instances_buffer, geometry_access},
               {cull_context.hpb_attachment, vir::Access::TaskSampled | vir::Access::MeshSampled},
               {cull_context.page_table_attachment, vir::Access::FragmentSampled},
               {context.physical_page_table_attachment, vir::Access::FragmentRW}},
              {mip_extent, mip_extent}
            )
            .with_name(stack.format("vsm pointspot draw mip {} ms", mip))
            .bind_graphics_pipeline(render_context.pipeline("rmvsm_pointspot_draw_pages_ms"));
        bind_vsm_pointspot_spec_constants(pass)
          .set_viewport(0, vir::Rect2D::framebuffer())
          .set_scissor(0, vir::Rect2D::framebuffer())
          .bind_buffer(0, 0, values.meshes)
          .bind_buffer(0, 1, values.mesh_instances)
          .bind_buffer(0, 2, values.transforms_world)
          .bind_buffer(0, 3, values.materials)
          .bind_buffer(0, 4, cull_context.views_buffer)
          .bind_buffer(0, 5, cull_context.vsm_meshlet_instances_buffer)
          .bind_buffer(0, 6, cull_context.vsm_meshlet_instance_count_buffer)
          .bind_image(0, 7, cull_context.hpb_attachment)
          .bind_image(0, 8, cull_context.page_table_attachment)
          .bind_image(0, 9, context.physical_page_table_attachment, vir::Format::R32Uint)
          .push_constants_from(mip_ps_ctx)
          .draw_mesh_tasks_indirect(cull_context.draw_cmd_buffer, 1);
        std::tie(
          cull_context.draw_cmd_buffer,
          cull_context.vsm_meshlet_instance_count_buffer,
          values.meshes,
          values.mesh_instances,
          values.transforms_world,
          values.materials,
          cull_context.views_buffer,
          cull_context.vsm_meshlet_instances_buffer,
          cull_context.hpb_attachment,
          cull_context.page_table_attachment,
          context.physical_page_table_attachment
        ) = std::tuple_cat(pass.end_rendering());
      } else {
        auto pass = //
          module
            .begin_rendering_area(
              {{cull_context.draw_cmd_buffer, vir::Access::IndirectRead},
               {cull_context.reordered_indices_buffer, vir::Access::IndexRead},
               {values.meshes, vir::Access::VertexRead},
               {values.mesh_instances, vir::Access::VertexRead},
               {values.transforms_world, vir::Access::VertexRead},
               {values.materials, vir::Access::FragmentRead},
               {cull_context.views_buffer, vir::Access::VertexRead},
               {cull_context.vsm_meshlet_instances_buffer, vir::Access::VertexRead},
               {cull_context.page_table_attachment, vir::Access::FragmentSampled},
               {context.physical_page_table_attachment, vir::Access::FragmentRW}},
              {mip_extent, mip_extent}
            )
            .with_name(stack.format("vsm pointspot draw mip {}", mip))
            .bind_graphics_pipeline(render_context.pipeline("rmvsm_pointspot_draw_pages"));
        bind_vsm_pointspot_spec_constants(pass)
          .set_viewport(0, vir::Rect2D::framebuffer())
          .set_scissor(0, vir::Rect2D::framebuffer())
          .bind_buffer(0, 0, values.meshes)
          .bind_buffer(0, 1, values.mesh_instances)
          .bind_buffer(0, 2, values.transforms_world)
          .bind_buffer(0, 3, values.materials)
          .bind_buffer(0, 4, cull_context.views_buffer)
          .bind_buffer(0, 5, cull_context.vsm_meshlet_instances_buffer)
          .bind_image(0, 6, cull_context.page_table_attachment)
          .bind_image(0, 7, context.physical_page_table_attachment, vir::Format::R32Uint)
          .bind_index_buffer(cull_context.reordered_indices_buffer, VK_INDEX_TYPE_UINT32)
          .push_constants_from(mip_ps_ctx)
          .draw_indexed_indirect(cull_context.draw_cmd_buffer, 1);
        std::tie(
          cull_context.draw_cmd_buffer,
          cull_context.reordered_indices_buffer,
          values.meshes,
          values.mesh_instances,
          values.transforms_world,
          values.materials,
          cull_context.views_buffer,
          cull_context.vsm_meshlet_instances_buffer,
          cull_context.page_table_attachment,
          context.physical_page_table_attachment
        ) = std::tuple_cat(pass.end_rendering());
      }
    }

    context.pointspot_views_buffer = cull_context.views_buffer;
    context.pointspot_layer_dirty_mask_buffer = cull_context.layer_dirty_mask_buffer;
    context.pointspot_page_table_attachment = cull_context.page_table_attachment;
  }
}
} // namespace ox
