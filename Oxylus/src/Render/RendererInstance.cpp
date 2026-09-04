#include "Render/RendererInstance.hpp"

#include <algorithm>
#include <ankerl/svector.h>
#include <vuk/runtime/CommandBuffer.hpp>

#include "Asset/AssetManager.hpp"
#include "Asset/Model.hpp"
#include "Asset/Texture.hpp"
#include "Core/App.hpp"
#include "Core/Enum.hpp"
#include "Memory/Stack.hpp"
#include "Render/DebugRenderer.hpp"
#include "Render/RenderContext.hpp"
#include "Render/Utils/VukCommon.hpp"
#include "Scene/SceneGPU.hpp"
#include "Utils/Log.hpp"

namespace ox {
struct PendingLight {
  GPU::Light light = {};
  u64 entity_id = 0;
  bool cast_shadows = false;
};

struct ShadowSlotCandidate {
  u32 light_index = 0;
  f32 priority = 0.0f;
  u64 entity_id = 0;
};

struct ShadowSlotStats {
  u32 reassigned = 0;
  u32 invalidated = 0;
};

// prevent slot churn near frustum and priority boundaries
constexpr static f32 SHADOW_SLOT_HYSTERESIS = 1.25f;

static auto atmosphere_lut_inputs_equal(const GPU::Atmosphere& a, const GPU::Atmosphere& b) -> bool {
  return a.rayleigh_scatter == b.rayleigh_scatter &&           //
         a.rayleigh_density == b.rayleigh_density &&           //
         a.mie_scatter == b.mie_scatter &&                     //
         a.mie_density == b.mie_density &&                     //
         a.mie_extinction == b.mie_extinction &&               //
         a.mie_asymmetry == b.mie_asymmetry &&                 //
         a.mie_haze_amount == b.mie_haze_amount &&             //
         a.mie_haze_scale_height == b.mie_haze_scale_height && //
         a.ozone_absorption == b.ozone_absorption &&           //
         a.ozone_height == b.ozone_height &&                   //
         a.ozone_thickness == b.ozone_thickness &&             //
         a.terrain_albedo == b.terrain_albedo &&               //
         a.planet_radius == b.planet_radius &&                 //
         a.atmos_radius == b.atmos_radius;
}

static auto shadow_light_priority(const GPU::Light& light) -> f32 {
  if (light.range <= 0.0f) {
    return -1.0f;
  }

  // VSM layers are also sampled by world-space DDGI probes, so their ownership must not depend on the camera
  return light.range * glm::sqrt(glm::max(light.intensity, 0.0f));
}

static auto assign_shadow_slots(
  std::span<PendingLight> lights,
  GPU::LightKind kind,
  std::span<ShadowSlotState> slots,
  u32& slot_high_water,
  u64& moved_mask
) -> ShadowSlotStats {
  ZoneScoped;
  memory::ScopedStack stack;

  auto stats = ShadowSlotStats{};

  // only uploaded lights may receive shadow slots
  const auto uploaded_light_count = glm::min(lights.size(), static_cast<usize>(GPU::MAX_LIGHTS));

  auto candidates = stack.alloc<ShadowSlotCandidate>(uploaded_light_count);
  auto candidate_count = 0_sz;
  for (auto light_index = 0_u32; light_index < uploaded_light_count; light_index++) {
    const auto& pending = lights[light_index];
    if (!pending.cast_shadows || pending.light.kind != kind) {
      continue;
    }

    const auto priority = shadow_light_priority(pending.light);
    if (priority <= 0.0f) {
      continue;
    }

    candidates[candidate_count++] = ShadowSlotCandidate{
      .light_index = light_index,
      .priority = priority,
      .entity_id = pending.entity_id,
    };
  }

  auto selected = candidates.subspan(0, candidate_count);
  std::ranges::sort(selected, [](const ShadowSlotCandidate& lhs, const ShadowSlotCandidate& rhs) {
    if (lhs.priority != rhs.priority) {
      return lhs.priority > rhs.priority;
    }
    return lhs.entity_id < rhs.entity_id;
  });
  selected = selected.subspan(0, glm::min(selected.size(), slots.size()));

  // preserve existing assignments before filling vacant slots
  auto assigned_slots = stack.alloc<i32>(selected.size());
  auto claimed_mask = 0_u64;

  for (auto i = 0_sz; i < selected.size(); i++) {
    assigned_slots[i] = -1;
    const auto entity_id = lights[selected[i].light_index].entity_id;
    for (auto slot_index = 0_u32; slot_index < slots.size(); slot_index++) {
      if (slots[slot_index].entity_id == entity_id) {
        assigned_slots[i] = static_cast<i32>(slot_index);
        claimed_mask |= 1_u64 << slot_index;
        break;
      }
    }
  }

  auto next_free_slot = 0_u32;
  for (auto i = 0_sz; i < selected.size(); i++) {
    if (assigned_slots[i] >= 0) {
      continue;
    }

    while (next_free_slot < slots.size() && (claimed_mask & (1_u64 << next_free_slot)) != 0) {
      next_free_slot++;
    }

    assigned_slots[i] = static_cast<i32>(next_free_slot);
    claimed_mask |= 1_u64 << next_free_slot;
  }

  for (auto i = 0_sz; i < selected.size(); i++) {
    auto& pending = lights[selected[i].light_index];
    const auto slot_index = static_cast<u32>(assigned_slots[i]);
    auto& slot = slots[slot_index];

    const auto occupant_changed = slot.entity_id != pending.entity_id;
    const auto view_changed = slot.position != pending.light.position ||   //
                              slot.direction != pending.light.direction || //
                              slot.range != pending.light.range ||         //
                              slot.outer_cone_angle != pending.light.outer_cone_angle;
    if (occupant_changed || view_changed) {
      moved_mask |= 1_u64 << slot_index;
      stats.reassigned += static_cast<u32>(occupant_changed);
      stats.invalidated += static_cast<u32>(!occupant_changed);
    }

    slot = ShadowSlotState{
      .entity_id = pending.entity_id,
      .position = pending.light.position,
      .direction = pending.light.direction,
      .range = pending.light.range,
      .outer_cone_angle = pending.light.outer_cone_angle,
    };

    pending.light.shadow_map_index = static_cast<i32>(slot_index);
    slot_high_water = glm::max(slot_high_water, slot_index + 1);
  }

  // clearing vacated state invalidates it for future occupants
  for (auto slot_index = 0_u32; slot_index < slots.size(); slot_index++) {
    if ((claimed_mask & (1_u64 << slot_index)) == 0) {
      slots[slot_index] = {};
    }
  }

  return stats;
}

auto bind_vsm_pointspot_spec_constants(vuk::CommandBuffer& cmd) -> vuk::CommandBuffer& {
  return cmd.specialize_constants(50, RMVSMContext::POINT_SPOT_IMAGE_RESOLUTION)
    .specialize_constants(51, RMVSMContext::POINT_SPOT_PAGE_TABLE_SIZE)
    .specialize_constants(52, RMVSMContext::POINT_SPOT_MIP_COUNT)
    .specialize_constants(53, RMVSMContext::PAGE_SIZE)
    .specialize_constants(54, RMVSMContext::PHYSICAL_PAGE_TABLE_SIZE);
}

template <typename T>
auto update_projected_transform_buffer(
  auto& render_context,
  std::span<GPU::Transforms> gpu_transforms,
  std::span<GPU::TransformID> dirty_transform_ids,
  vuk::Unique<vuk::Buffer>& buffer,
  vuk::Value<vuk::Buffer>& prepared_buffer,
  auto projection,
  std::string_view buffer_name,
  std::string_view pass_name
) -> void {
  memory::ScopedStack stack;

  constexpr auto full_rebuild_dirty_threshold = 0.4;

  const auto element_count = gpu_transforms.size();
  constexpr auto element_size = sizeof(T);
  const auto rebuild_needed = !buffer || buffer->size < gpu_transforms.size_bytes();

  buffer = render_context.resize_buffer(std::move(buffer), vuk::MemoryUsage::eGPUonly, gpu_transforms.size_bytes());
  if (dirty_transform_ids.empty()) {
    if (buffer) {
      prepared_buffer = vuk::acquire_buf(buffer_name, *buffer, vuk::Access::eMemoryRead);
    }

    return;
  }

  auto unique_indices = stack.alloc<u32>(dirty_transform_ids.size());
  for (const auto& [unique_index, dirty_id] : std::views::zip(unique_indices, dirty_transform_ids)) {
    unique_index = SlotMap_decode_id(dirty_id).index;
  }
  std::sort(unique_indices.begin(), unique_indices.end());
  const auto unique_end = std::unique(unique_indices.begin(), unique_indices.end());
  unique_indices = unique_indices.first(static_cast<usize>(unique_end - unique_indices.begin()));

  if (rebuild_needed || static_cast<f64>(unique_indices.size()) >= element_count * full_rebuild_dirty_threshold) {
    memory::ScopedStack staging_stack;

    auto staging = staging_stack.alloc<T>(element_count);
    for (auto i = 0_sz; i < element_count; ++i) {
      staging[i] = projection(gpu_transforms[i]);
    }
    prepared_buffer = render_context.upload_staging(staging, *buffer);

    return;
  }

  const auto dirty_count = unique_indices.size();
  const auto dirty_size_bytes = dirty_count * element_size;

  auto upload_buffer = render_context.alloc_transient_buffer(vuk::MemoryUsage::eCPUtoGPU, dirty_size_bytes);
  auto* dst_ptr = reinterpret_cast<T*>(upload_buffer->mapped_ptr);
  for (usize i = 0; i < dirty_count; ++i) {
    dst_ptr[i] = projection(gpu_transforms[unique_indices[i]]);
  }

  struct CopyRange {
    usize src_offset = 0;
    usize dst_offset = 0;
    usize size_bytes = 0;
  };

  auto ranges = std::vector<CopyRange>{};
  ranges.reserve(dirty_count);
  for (auto i = 0_sz; i < dirty_count;) {
    const auto start_index = unique_indices[i];
    auto run_length = 1_sz;
    while (i + run_length < dirty_count && unique_indices[i + run_length] == start_index + run_length) {
      ++run_length;
    }
    ranges.push_back({i * element_size, start_index * element_size, run_length * element_size});
    i += run_length;
  }

  auto update_pass = vuk::make_pass(
    pass_name,
    [copy_ranges = std::move(ranges)](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::Access::eTransferRead) src_buffer,
      VUK_BA(vuk::Access::eTransferWrite) dst_buffer
    ) {
      for (const auto& r : copy_ranges) {
        const auto src_subrange = src_buffer->subrange(r.src_offset, r.size_bytes);
        const auto dst_subrange = dst_buffer->subrange(r.dst_offset, r.size_bytes);
        cmd_list.copy_buffer(src_subrange, dst_subrange);
      }
      return dst_buffer;
    }
  );

  auto buffer_handle = vuk::acquire_buf(buffer_name, *buffer, vuk::Access::eMemoryRead);
  prepared_buffer = update_pass(std::move(upload_buffer), std::move(buffer_handle));
}

RendererInstance::RendererInstance(Scene& owner_scene, Renderer& parent_renderer)
    : scene(owner_scene),
      renderer(parent_renderer) {

  auto& render_context = App::get_rendercontext();
  auto& allocator = render_context.superframe_allocator;
  render_queue_2d.init();

  lights_buffer = render_context.allocate_buffer_super(
    vuk::MemoryUsage::eGPUonly,
    GPU::MAX_LIGHTS * sizeof(GPU::Light)
  );
  transforms_world_buffer = render_context.allocate_buffer_super(
    vuk::MemoryUsage::eGPUonly,
    sizeof(GPU::TransformWorld)
  );
  transforms_previous_buffer = render_context.allocate_buffer_super(
    vuk::MemoryUsage::eGPUonly,
    sizeof(GPU::TransformPrevious)
  );

  constexpr usize stage_count = static_cast<usize>(RenderStage::Count);
  before_callbacks.resize(stage_count);
  after_callbacks.resize(stage_count);

  sky_transmittance_lut = Texture::create({
    .format = vuk::Format::eR16G16B16A16Sfloat,
    .extent = vuk::Extent3D{.width = 256u, .height = 64u, .depth = 1u},
    .usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage,
  });
  OX_ASSERT(sky_transmittance_lut);

  sky_multiscatter_lut = Texture::create({
    .format = vuk::Format::eR16G16B16A16Sfloat,
    .extent = vuk::Extent3D{.width = 32u, .height = 32u, .depth = 1u},
    .usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage,
  });
  OX_ASSERT(sky_multiscatter_lut);

  constexpr auto HILBERT_NOISE_LUT_WIDTH = 64_u32;
  auto hilbert_index = [](u32 pos_x, u32 pos_y) -> u16 {
    auto index = 0_u32;
    for (auto cur_level = HILBERT_NOISE_LUT_WIDTH / 2; cur_level > 0_u32; cur_level /= 2_u32) {
      auto region_x = static_cast<u32>((pos_x & cur_level) > 0_u32);
      auto region_y = static_cast<u32>((pos_y & cur_level) > 0_u32);
      index += cur_level * cur_level * ((3_u32 * region_x) ^ region_y);
      if (region_y == 0_u32) {
        if (region_x == 1_u32) {
          pos_x = (HILBERT_NOISE_LUT_WIDTH - 1_u32) - pos_x;
          pos_y = (HILBERT_NOISE_LUT_WIDTH - 1_u32) - pos_y;
        }

        auto temp_pos_x = pos_x;
        pos_x = pos_y;
        pos_y = temp_pos_x;
      }
    }

    return static_cast<u16>(index);
  };

  u16 hilbert_noise[HILBERT_NOISE_LUT_WIDTH * HILBERT_NOISE_LUT_WIDTH] = {};
  for (auto y = 0_u32; y < HILBERT_NOISE_LUT_WIDTH; y++) {
    for (auto x = 0_u32; x < HILBERT_NOISE_LUT_WIDTH; x++) {
      hilbert_noise[y * HILBERT_NOISE_LUT_WIDTH + x] = hilbert_index(x, y);
    }
  }

  hilbert_noise_lut = Texture::create({
    .format = vuk::Format::eR16Uint,
    .extent = vuk::Extent3D{.width = HILBERT_NOISE_LUT_WIDTH, .height = HILBERT_NOISE_LUT_WIDTH, .depth = 1u},
    .usage = vuk::ImageUsageFlagBits::eSampled,
  });
  OX_ASSERT(hilbert_noise_lut);
  hilbert_noise_lut.upload({reinterpret_cast<u8*>(hilbert_noise), sizeof(hilbert_noise)}, vuk::eFragmentSampled);

  sky_cubemap = Texture::create({
    .format = vuk::Format::eR16G16B16A16Sfloat,
    .extent = vuk::Extent3D{.width = 32u, .height = 32u, .depth = 1u},
    .layer_count = 6u,
    .image_flags = vuk::ImageCreateFlagBits::eCubeCompatible,
    .usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage,
    .view_type = vuk::ImageViewType::eCube,
  });
  OX_ASSERT(sky_cubemap);

  auto sky_cubemap_init = sky_cubemap.discard("sky_cubemap_init");

  sky_cubemap_init = vuk::clear_image(std::move(sky_cubemap_init), vuk::Black<float>);
  sky_cubemap_init = sky_cubemap_init.as_released(vuk::eFragmentSampled);
  render_context.wait_on(std::move(sky_cubemap_init));

  auto transmittance_lut_attachment = vuk::clear_image(
    sky_transmittance_lut.discard("sky_transmittance_lut_init"),
    vuk::Black<f32>
  );
  auto multiscatter_lut_attachment = vuk::clear_image(
    sky_multiscatter_lut.discard("sky_multiscatter_lut_init"),
    vuk::Black<f32>
  );
  transmittance_lut_attachment = std::move(transmittance_lut_attachment).as_released(vuk::eFragmentSampled);
  multiscatter_lut_attachment = std::move(multiscatter_lut_attachment).as_released(vuk::eFragmentSampled);
  render_context.wait_on(std::move(transmittance_lut_attachment));
  render_context.wait_on(std::move(multiscatter_lut_attachment));

  vsm_virtual_page_table = Texture::create({
    .format = vuk::Format::eR32Uint,
    .extent =
      {
        .width = RMVSMContext::DIRECTIONAL_PAGE_TABLE_SIZE,
        .height = RMVSMContext::DIRECTIONAL_PAGE_TABLE_SIZE,
        .depth = 1,
      },
    .layer_count = RMVSMContext::MAX_DIRECTIONAL_CLIPMAP_COUNT,
    .level_count = 1,
    .usage = vuk::ImageUsageFlagBits::eStorage | vuk::ImageUsageFlagBits::eSampled,
    .view_type = vuk::ImageViewType::e2DArray,

  });

  auto virtual_page_table_attachment = vsm_virtual_page_table.discard("vsm virtual page table");
  virtual_page_table_attachment = vuk::clear_image(std::move(virtual_page_table_attachment), vuk::Black<u32>);
  virtual_page_table_attachment = std::move(virtual_page_table_attachment).as_released(vuk::eFragmentSampled);
  render_context.wait_on(std::move(virtual_page_table_attachment));

  vsm_pointspot_virtual_page_table = Texture::create({
    .format = vuk::Format::eR32Uint,
    .extent =
      {
        .width = RMVSMContext::POINT_SPOT_PAGE_TABLE_SIZE,
        .height = RMVSMContext::POINT_SPOT_PAGE_TABLE_SIZE,
        .depth = 1,
      },
    .layer_count = RMVSMContext::POINT_SPOT_LAYER_COUNT,
    .level_count = RMVSMContext::POINT_SPOT_MIP_COUNT,
    .usage = vuk::ImageUsageFlagBits::eStorage | vuk::ImageUsageFlagBits::eSampled,
    .view_type = vuk::ImageViewType::e2DArray,
  });

  auto pointspot_page_table_attachment = vsm_pointspot_virtual_page_table.discard("vsm pointspot page table");
  pointspot_page_table_attachment = vuk::clear_image(std::move(pointspot_page_table_attachment), vuk::Black<u32>);
  pointspot_page_table_attachment = std::move(pointspot_page_table_attachment).as_released(vuk::eFragmentSampled);
  render_context.wait_on(std::move(pointspot_page_table_attachment));

  vsm_physical_page_table_attachment = vuk::ImageAttachment{
    .image_flags = vuk::ImageCreateFlagBits::eMutableFormat,
    .usage = vuk::ImageUsageFlagBits::eStorage | vuk::ImageUsageFlagBits::eSampled |
             vuk::ImageUsageFlagBits::eTransferDst,
    .extent =
      {.width = RMVSMContext::PHYSICAL_PAGE_TABLE_SIZE, .height = RMVSMContext::PHYSICAL_PAGE_TABLE_SIZE, .depth = 1},
    .format = vuk::Format::eR32Sfloat,
    .sample_count = vuk::Samples::e1,
    .view_type = vuk::ImageViewType::e2D,
    .base_level = 0,
    .level_count = 1,
    .base_layer = 0,
    .layer_count = 1,
  };
  vsm_physical_page_table = *vuk::allocate_image(*allocator, vsm_physical_page_table_attachment);
  vsm_physical_page_table_attachment.image = *vsm_physical_page_table;
  vsm_physical_page_table_f32_view = *vuk::allocate_image_view(*allocator, vsm_physical_page_table_attachment);
  vsm_physical_page_table_attachment.image_view = *vsm_physical_page_table_f32_view;
  auto vsm_physical_page_table_u32_attachment = vsm_physical_page_table_attachment;
  vsm_physical_page_table_u32_attachment.format = vuk::Format::eR32Uint;
  vsm_physical_page_table_u32_view = *vuk::allocate_image_view(*allocator, vsm_physical_page_table_u32_attachment);
  render_context.wait_on(
    vuk::clear_image(vuk::discard_ia("vsm physical page table", vsm_physical_page_table_attachment), vuk::Black<f32>)
      .as_released(vuk::eFragmentSampled)
  );

  rebuild_execution_order();
}

RendererInstance::~RendererInstance() {}

auto RendererInstance::add_stage_callback(this RendererInstance& self, RenderStageCallback callback) -> void {
  ZoneScoped;
  self.stage_callbacks.emplace_back(std::move(callback));
  self.rebuild_execution_order();
}

auto RendererInstance::clear_stages(this RendererInstance& self) -> void {
  ZoneScoped;
  self.stage_callbacks.clear();
}

auto RendererInstance::rebuild_execution_order(this RendererInstance& self) -> void {
  ZoneScoped;

  constexpr usize stage_count = static_cast<usize>(RenderStage::Count);

  for (auto& vec : self.before_callbacks) {
    vec.clear();
  }
  for (auto& vec : self.after_callbacks) {
    vec.clear();
  }

  std::sort(
    self.stage_callbacks.begin(),
    self.stage_callbacks.end(),
    [](const RenderStageCallback& a, const RenderStageCallback& b) noexcept {
      return a.dependency.order < b.dependency.order;
    }
  );

  for (usize i = 0; i < self.stage_callbacks.size(); ++i) {
    const auto& callback = self.stage_callbacks[i];
    const usize stage_index = static_cast<usize>(callback.dependency.target_stage);

    if (stage_index >= stage_count) [[unlikely]] {
      continue;
    }

    if (!callback.callback) [[unlikely]] {
      continue;
    }

    if (callback.dependency.position == StagePosition::Before) {
      self.before_callbacks[stage_index].emplace_back(i);
    } else if (callback.dependency.position == StagePosition::After) {
      self.after_callbacks[stage_index].emplace_back(i);
    }
  }

  for (auto& vec : self.before_callbacks) {
    vec.shrink_to_fit();
  }
  for (auto& vec : self.after_callbacks) {
    vec.shrink_to_fit();
  }
}

auto RendererInstance::execute_stages_before(
  this const RendererInstance& self, RenderStage stage, RenderStageContext& ctx
) -> void {
  ZoneScoped;

  const usize stage_index = static_cast<usize>(stage);
  constexpr usize stage_count = static_cast<usize>(RenderStage::Count);

  if (stage_index >= stage_count) [[unlikely]] {
    return;
  }

  if (stage_index >= self.before_callbacks.size()) [[unlikely]] {
    return;
  }

  const auto& callbacks = self.before_callbacks[stage_index];

  for (const usize callback_idx : callbacks) {
    if (callback_idx >= self.stage_callbacks.size()) [[unlikely]] {
      continue;
    }

    const auto& callback = self.stage_callbacks[callback_idx];

    if (callback.callback) [[likely]] {
      callback.callback(ctx);
    }
  }
}

auto RendererInstance::execute_stages_after(
  this const RendererInstance& self, RenderStage stage, RenderStageContext& ctx
) -> void {
  ZoneScoped;

  const usize stage_index = static_cast<usize>(stage);
  constexpr usize stage_count = static_cast<usize>(RenderStage::Count);

  if (stage_index >= stage_count) [[unlikely]] {
    return;
  }

  if (stage_index >= self.after_callbacks.size()) [[unlikely]] {
    return;
  }

  const auto& callbacks = self.after_callbacks[stage_index];

  for (const usize callback_idx : callbacks) {
    if (callback_idx >= self.stage_callbacks.size()) [[unlikely]] {
      continue;
    }

    const auto& callback = self.stage_callbacks[callback_idx];

    if (callback.callback) [[likely]] {
      callback.callback(ctx);
    }
  }
}
auto RendererInstance::add_stage_before(
  this RendererInstance& self,
  RenderStage stage,
  const std::string& name,
  std::function<void(RenderStageContext&)> callback,
  int order
) -> void {
  StageDependency dep{.target_stage = stage, .position = StagePosition::Before, .order = order};
  self.add_stage_callback(RenderStageCallback{.callback = std::move(callback), .dependency = dep, .name = name});
}

auto RendererInstance::add_stage_after(
  this RendererInstance& self,
  RenderStage stage,
  const std::string& name,
  std::function<void(RenderStageContext&)> callback,
  int order
) -> void {
  StageDependency dep{.target_stage = stage, .position = StagePosition::After, .order = order};
  self.add_stage_callback(RenderStageCallback{.callback = std::move(callback), .dependency = dep, .name = name});
}

auto RendererInstance::render(
  this RendererInstance& self,
  vuk::Value<vuk::ImageAttachment>&& dst_attachment,
  glm::ivec2 viewport_origin,
  glm::ivec2 viewport_size,
  glm::ivec2 surface_size,
  const RendererCVar& cvar
) -> vuk::Value<vuk::ImageAttachment> {
  ZoneScoped;

  self.viewport_origin_ = viewport_origin;
  self.viewport_size_ = viewport_size;
  self.surface_size_ = surface_size;

  OX_ASSERT(self.update_ran_this_frame);
  self.update_ran_this_frame = false;

  OX_DEFER(&) {
    self.clear_stages();
    self.shared_resources.clear();
    self.prepared_frame = {};
  };

  const auto dst_extent = dst_attachment->extent;

  OX_CHECK_GT(dst_extent.width, 0u);
  OX_CHECK_GT(dst_extent.height, 0u);

  auto& bindless_set = self.renderer.render_context->get_descriptor_set();

  self.camera_data.resolution = {dst_extent.width, dst_extent.height};
  self.prepared_frame.camera_buffer = self.renderer.render_context->scratch_buffer(self.camera_data);

  self.render_queue_2d.update();
  self.render_queue_2d.sort();
  auto vertex_buffer_2d = self.renderer.render_context->scratch_buffer_span(
    std::span(self.render_queue_2d.sprite_data)
  );

  const auto scene_has_atmosphere = self.gpu_scene_flags & GPU::SceneFlags::HasAtmosphere;
  const auto scene_has_directional_light = self.gpu_scene_flags & GPU::SceneFlags::HasDirectionalLight;

  if (cvar.cvar_bloom_enable.as_bool())
    self.gpu_scene_flags |= GPU::SceneFlags::HasBloom;
  if (cvar.cvar_fxaa_enable.as_bool())
    self.gpu_scene_flags |= GPU::SceneFlags::HasFXAA;
  if (cvar.cvar_vbgtao_enable.as_bool())
    self.gpu_scene_flags |= GPU::SceneFlags::HasGTAO;
  if (cvar.cvar_contact_shadows_enabled.as_bool())
    self.gpu_scene_flags |= GPU::SceneFlags::HasContactShadows;
  if (cvar.cvar_transparent_background.as_bool())
    self.gpu_scene_flags |= GPU::SceneFlags::TransparentBackground;
  if (cvar.cvar_particles_enable.as_bool())
    self.gpu_scene_flags |= GPU::SceneFlags::HasParticles;
  if (cvar.cvar_particle_sort.as_bool())
    self.gpu_scene_flags |= GPU::SceneFlags::HasParticleSorting;

  const auto debug_view = static_cast<GPU::DebugView>(cvar.cvar_debug_view.get());
  const f32 debug_heatmap_scale = 5.0;
  const auto debugging = debug_view != GPU::DebugView::None && cvar.cvar_enable_debug_renderer.as_bool();
  const auto draw_overdraw = debugging && debug_view == GPU::DebugView::Overdraw;
  const auto debug_uses_visbuffer = debugging && debug_view != GPU::DebugView::DDGIProbes &&
                                    debug_view != GPU::DebugView::RMVSM && debug_view != GPU::DebugView::RMVSMPointSpot;

  const auto transparent_background = static_cast<bool>(self.gpu_scene_flags & GPU::SceneFlags::TransparentBackground);
  const auto hdr_format = transparent_background ? vuk::Format::eR16G16B16A16Sfloat
                                                 : vuk::Format::eB10G11R11UfloatPack32;

  auto final_attachment = vuk::declare_ia(
    "final_attachment",
    {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
     .extent = dst_extent,
     .format = hdr_format,
     .sample_count = vuk::Samples::e1,
     .level_count = 1,
     .layer_count = 1}
  );
  final_attachment = vuk::clear_image(
    std::move(final_attachment),
    transparent_background ? vuk::Transparent<f32> : vuk::Black<f32>
  );

  auto depth_attachment = vuk::declare_ia(
    "depth_image",
    {.usage = vuk::ImageUsageFlagBits::eDepthStencilAttachment | vuk::ImageUsageFlagBits::eSampled,
     .extent = dst_extent,
     .format = vuk::Format::eD32Sfloat,
     .sample_count = vuk::SampleCountFlagBits::e1,
     .level_count = 1,
     .layer_count = 1}
  );
  depth_attachment = vuk::clear_image(std::move(depth_attachment), vuk::DepthZero);

  auto hiz_extent = vuk::Extent3D{
    .width = std::bit_ceil((depth_attachment->extent.width + 1) >> 1),
    .height = std::bit_ceil((depth_attachment->extent.height + 1) >> 1),
    .depth = 1,
  };

  auto hiz_attachment = vuk::declare_ia(
    "hiz",
    {.usage = vuk::ImageUsageFlagBits::eStorage | vuk::ImageUsageFlagBits::eSampled,
     .extent = hiz_extent,
     .format = vuk::Format::eR32Sfloat,
     .sample_count = vuk::SampleCountFlagBits::e1,
     .level_count = std::min(Texture::calculate_mip_count(hiz_extent), 13u),
     .layer_count = 1}
  );
  hiz_attachment = vuk::clear_image(std::move(hiz_attachment), vuk::DepthZero);

  auto sky_transmittance_lut_attachment = self.sky_transmittance_lut.acquire(
    "sky transmittance lut",
    vuk::eFragmentSampled
  );
  auto sky_multiscatter_lut_attachment = self.sky_multiscatter_lut.acquire(
    "sky multiscatter lut",
    vuk::eFragmentSampled
  );

  auto sky_view_lut_attachment = vuk::declare_ia(
    "sky_view_lut",
    {.image_type = vuk::ImageType::e2D,
     .usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage,
     .extent = self.sky_view_lut_extent,
     .format = vuk::Format::eR16G16B16A16Sfloat,
     .sample_count = vuk::Samples::e1,
     .view_type = vuk::ImageViewType::e2D,
     .level_count = 1,
     .layer_count = 1}
  );
  sky_view_lut_attachment = vuk::clear_image(std::move(sky_view_lut_attachment), vuk::Black<f32>);

  auto sky_aerial_perspective_attachment = vuk::declare_ia(
    "sky aerial perspective",
    {.image_type = vuk::ImageType::e3D,
     .usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage,
     .extent = self.sky_aerial_perspective_lut_extent,
     .sample_count = vuk::Samples::e1,
     .view_type = vuk::ImageViewType::e3D,
     .level_count = 1,
     .layer_count = 1}
  );
  sky_aerial_perspective_attachment.same_format_as(sky_view_lut_attachment);
  sky_aerial_perspective_attachment = vuk::clear_image(std::move(sky_aerial_perspective_attachment), vuk::Black<f32>);

  auto sky_cubemap_attachment = self.sky_cubemap.acquire("sky cubemap", vuk::eFragmentSampled);

  auto hilbert_noise_lut_attachment = self.hilbert_noise_lut.acquire("hilbert noise", vuk::eFragmentSampled);

  auto visbuffer_attachment = vuk::declare_ia(
    "visbuffer",
    {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage |
              vuk::ImageUsageFlagBits::eColorAttachment,
     .format = vuk::Format::eR32Uint,
     .sample_count = vuk::SampleCountFlagBits::e1}
  );
  visbuffer_attachment.same_shape_as(final_attachment);

  auto visbuffer_attachment_2d = vuk::declare_ia(
    "visbuffer_2d",
    {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
     .format = vuk::Format::eR32Uint,
     .sample_count = vuk::SampleCountFlagBits::e1}
  );
  visbuffer_attachment_2d.same_shape_as(final_attachment);
  visbuffer_attachment_2d = vuk::clear_image(
    visbuffer_attachment_2d,
    vuk::ClearColor{~0_u32, ~0_u32, ~0_u32, ~0_u32}
  ); // Clear to invalid transform id

  auto overdraw_attachment = vuk::declare_ia(
    "overdraw",
    {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage,
     .extent = draw_overdraw ? dst_extent : vuk::Extent3D{1, 1, 1},
     .format = vuk::Format::eR32Uint,
     .sample_count = vuk::SampleCountFlagBits::e1,
     .level_count = 1,
     .layer_count = 1}
  );

  // before anything reads vertex positions: the culling passes, both visbuffer encoders and every
  // shadow pass all fetch through the same pointers this rewrites
  self.skin_vertices();

  auto vis_clear_pass = vuk::make_pass(
    "vis clear",
    [draw_overdraw](
      vuk::CommandBuffer& cmd_list, //
      VUK_IA(vuk::eComputeWrite) visbuffer,
      VUK_IA(vuk::eComputeWrite) overdraw
    ) {
      cmd_list //
        .bind_compute_pipeline("visbuffer_clear")
        .specialize_constants(0, draw_overdraw)
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

  auto shadows_attachment = vuk::declare_ia(
    "shadows",
    {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage |
              vuk::ImageUsageFlagBits::eColorAttachment,
     .format = vuk::Format::eR8G8Unorm,
     .sample_count = vuk::SampleCountFlagBits::e1}
  );
  shadows_attachment.same_shape_as(final_attachment);
  shadows_attachment = vuk::clear_image(std::move(shadows_attachment), vuk::White<f32>);

  auto albedo_attachment = vuk::declare_ia(
    "albedo",
    {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
     .format = vuk::Format::eR8G8B8A8Srgb,
     .sample_count = vuk::Samples::e1}
  );
  albedo_attachment.same_shape_as(visbuffer_attachment);
  albedo_attachment = vuk::clear_image(std::move(albedo_attachment), vuk::Black<f32>);

  auto normal_attachment = vuk::declare_ia(
    "normal",
    {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
     .format = vuk::Format::eR8G8B8A8Snorm,
     .sample_count = vuk::Samples::e1}
  );
  normal_attachment.same_shape_as(visbuffer_attachment);
  normal_attachment = vuk::clear_image(std::move(normal_attachment), vuk::Black<f32>);

  auto emissive_attachment = vuk::declare_ia(
    "emissive",
    {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
     .format = vuk::Format::eB10G11R11UfloatPack32,
     .sample_count = vuk::Samples::e1}
  );
  emissive_attachment.same_shape_as(visbuffer_attachment);
  emissive_attachment = vuk::clear_image(std::move(emissive_attachment), vuk::Black<f32>);

  auto metallic_roughness_occlusion_attachment = vuk::declare_ia(
    "metallic roughness occlusion",
    {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
     .format = vuk::Format::eR8G8Unorm,
     .sample_count = vuk::Samples::e1}
  );
  metallic_roughness_occlusion_attachment.same_shape_as(visbuffer_attachment);
  metallic_roughness_occlusion_attachment = vuk::clear_image(
    std::move(metallic_roughness_occlusion_attachment),
    vuk::Black<f32>
  );

  auto vbgtao_occlusion_attachment = vuk::declare_ia(
    "vbgtao occlusion",
    {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage,
     .format = vuk::Format::eR8Unorm,
     .sample_count = vuk::Samples::e1,
     .view_type = vuk::ImageViewType::e2D,
     .level_count = 1,
     .layer_count = 1}
  );
  vbgtao_occlusion_attachment.same_extent_as(depth_attachment);
  vbgtao_occlusion_attachment = vuk::clear_image(std::move(vbgtao_occlusion_attachment), vuk::White<f32>);

  auto vbgtao_depth_differences_attachment = vuk::Value<vuk::ImageAttachment>{};
  auto rmvsm_virtual_page_table_attachment = vuk::Value<vuk::ImageAttachment>{};
  auto rmvsm_virtual_clipmaps_buffer = vuk::Value<vuk::Buffer>{};
  auto pointspot_views_for_lighting = vuk::Value<vuk::Buffer>{};
  auto pointspot_page_table_for_lighting = vuk::Value<vuk::ImageAttachment>{};
  auto vsm_physical_pages_for_lighting = vuk::Value<vuk::ImageAttachment>{};
  auto vsm_chain_ran = false;

  auto light_grid_context = LightGridContext{};
  self.build_light_grid(light_grid_context);

  const auto* terrain = self.scene.terrain != nullptr && self.scene.terrain->is_baked() ? self.scene.terrain.get()
                                                                                        : nullptr;

  // This needs to be finished before any RT passes begin executing
  // should probably move this entire scope to somewhere else, shit_in_a_kettle.gif
  auto& frame_render_context = *self.renderer.render_context;
  if (
    frame_render_context.use_ray_tracing() && self.tlas_has_consumer(cvar) &&
    self.prepared_frame.mesh_instance_count > 0 && self.prepared_frame.blas_addresses_buffer.node != nullptr &&
    self.prepared_frame.skinned_blas_addresses_buffer.node != nullptr &&
    self.prepared_frame.mesh_instances_buffer.node != nullptr
  ) {
    auto skinned_blas_buffer = build_skinned_blases(
      frame_render_context,
      self.skinned_blas_pool,
      SkinnedBLASBudget{
        .rebuild_primitive_budget = static_cast<u32>(ox::max(0, cvar.cvar_rt_skinned_rebuild_budget.get())),
        .max_refits_before_rebuild = static_cast<u32>(ox::max(0, cvar.cvar_rt_skinned_max_refits.get())),
      },
      std::move(self.prepared_frame.skinned_vertices_buffer)
    );

    auto tlas_value = build_scene_tlas(
      frame_render_context,
      self.scene_tlas,
      TLASBuildInfo{
        .instance_count = self.prepared_frame.mesh_instance_count,
        .skinned_instance_count = self.prepared_frame.skinned_instance_count,
        .mesh_instances_buffer = self.prepared_frame.mesh_instances_buffer,
        .transforms_buffer = self.prepared_frame.transforms_world_buffer,
        .blas_addresses_buffer = self.prepared_frame.blas_addresses_buffer,
        .skinned_blas_addresses_buffer = self.prepared_frame.skinned_blas_addresses_buffer,
        .skinned_blas_buffer = std::move(skinned_blas_buffer),
      }
    );

    if (tlas_value.node != nullptr) {
      // build_scene_tlas waits on the subgraph the builds were threaded into, so by here they have
      // run: only now may an entry count as built and start publishing its address
      self.skinned_blas_pool.commit_builds();
      self.shared_resources.buffer_resources["tlas"] = std::move(tlas_value);
    }
  }

  // --- 3D Pass ---
  if (self.prepared_frame.mesh_instance_count > 0 || terrain != nullptr) {
    auto main_geometry_context = MainGeometryContext{
      .draw_overdraw = draw_overdraw,
      .bindless_set = &bindless_set,
      .depth_attachment = std::move(depth_attachment),
      .hiz_attachment = std::move(hiz_attachment),
      .visbuffer_attachment = std::move(visbuffer_attachment),
      .overdraw_attachment = std::move(overdraw_attachment),
      .albedo_attachment = std::move(albedo_attachment),
      .normal_attachment = std::move(normal_attachment),
      .emissive_attachment = std::move(emissive_attachment),
      .metallic_roughness_occlusion_attachment = std::move(metallic_roughness_occlusion_attachment),
    };

    auto cull_camera = GPU::CullCamera{
      .projection_view = self.camera_data.projection_view,
      .position = self.camera_data.position,
      .acceptable_lod_error = self.camera_data.acceptable_lod_error,
      .resolution = self.camera_data.resolution,
      .near_clip = self.camera_data.near_clip,
      .mesh_instance_count = self.prepared_frame.mesh_instance_count,
    };
    const auto has_meshes = self.prepared_frame.mesh_instance_count > 0;

    // The CullGeometryContext is hoisted outside both passes so the visibility /
    // dispatch-command buffers allocated by the early pass's `cull_meshes`
    // pre-pass persist into the late pass (which runs with `init_cull_meshes = false`).
    auto cull_geometry_context = CullGeometryContext{
      .use_hiz = true,
      .init_cull_meshes = true,
      .cull_camera = cull_camera,
    };

    auto terrain_context = TerrainContext{.terrain = terrain};
    auto terrain_brush_context = TerrainBrushContext{.terrain = terrain};
    if (terrain != nullptr) {
      terrain_context.terrain_buffer = self.build_terrain_buffer(*terrain);
      terrain_context.visible_patches_buffer = self.renderer.render_context->alloc_transient_buffer(
        vuk::MemoryUsage::eGPUonly,
        static_cast<usize>(terrain->patch_count.x) * terrain->patch_count.y * sizeof(u32)
      );
      terrain_context.patch_visibility_mask_buffer = std::move(
        self.prepared_frame.terrain_patch_visibility_mask_buffer
      );

      terrain_brush_context.maps = TerrainMaps{
        .heightmap = terrain->heightmap.acquire("terrain heightmap", vuk::eComputeSampled),
        .normalmap = terrain->normalmap.acquire("terrain normalmap", vuk::eFragmentSampled),
        .splatmap = terrain->splatmap.acquire("terrain splatmap", vuk::eFragmentSampled),
        .patch_minmax = terrain->patch_minmax.acquire("terrain patch minmax", vuk::eComputeSampled),
      };

      if (terrain->brush.active && terrain->brush.painting) {
        terrain_brush_context.maps.ridgemap = terrain->ridgemap.acquire("terrain ridgemap", vuk::eFragmentSampled);
        terrain_brush_context.maps.height_edit = terrain->height_edit.acquire("terrain height edit", vuk::eComputeRW);
        terrain_brush_context.maps.splat_edit = terrain->splat_edit.acquire("terrain splat edit", vuk::eComputeRW);

        self.scene.terrain->edits_dirty = true;
      }

      if (terrain->brush.active) {
        self.apply_terrain_brush(terrain_brush_context);
      } else {
        terrain_brush_context.hit_buffer = self.renderer.render_context->scratch_buffer(GPU::TerrainBrushHit{});
      }

      self.scene.terrain->brush.active = false;
      self.scene.terrain->brush.painting = false;

      terrain_context.heightmap_attachment = std::move(terrain_brush_context.maps.heightmap);
      terrain_context.patch_minmax_attachment = std::move(terrain_brush_context.maps.patch_minmax);
    }

    const auto run_geometry_pass = [&](bool late) {
      if (late) {
        cull_geometry_context.cull_flags |= GPU::CullFlag::LatePass;
        cull_geometry_context.init_cull_meshes = false;
        cull_geometry_context.cull_camera = cull_camera;
      }

      if (has_meshes) {
        cull_geometry_context.hiz_attachment = std::move(main_geometry_context.hiz_attachment);
        self.cull_geometry(cull_geometry_context);
        main_geometry_context.hiz_attachment = std::move(cull_geometry_context.hiz_attachment);
        main_geometry_context.draw_geometry_cmd_buffer = std::move(cull_geometry_context.draw_geometry_cmd_buffer);
        main_geometry_context.cull_flags = cull_geometry_context.cull_flags;
        main_geometry_context.cull_camera = cull_geometry_context.cull_camera;
        main_geometry_context.visibility_buffer = std::move(cull_geometry_context.visibility_buffer);
        self.draw_for_visbuffer(main_geometry_context);
        cull_geometry_context.visibility_buffer = std::move(main_geometry_context.visibility_buffer);
        if (self.prepared_frame.use_mesh_shaders) {
          cull_geometry_context.cull_meshlets_cmd_buffer = std::move(main_geometry_context.draw_geometry_cmd_buffer);
        }
      }

      if (terrain != nullptr) {
        terrain_context.cull_flags = GPU::CullFlag::TestFrustum | GPU::CullFlag::TestOcclusion;
        if (late) {
          terrain_context.cull_flags |= GPU::CullFlag::LatePass;
        }
        terrain_context.cull_camera = cull_camera;
        terrain_context.hiz_attachment = std::move(main_geometry_context.hiz_attachment);
        self.cull_terrain(terrain_context);
        main_geometry_context.hiz_attachment = std::move(terrain_context.hiz_attachment);

        terrain_context.visbuffer_attachment = std::move(main_geometry_context.visbuffer_attachment);
        terrain_context.depth_attachment = std::move(main_geometry_context.depth_attachment);
        self.draw_terrain_for_visbuffer(terrain_context);
        main_geometry_context.visbuffer_attachment = std::move(terrain_context.visbuffer_attachment);
        main_geometry_context.depth_attachment = std::move(terrain_context.depth_attachment);
      }
    };

    run_geometry_pass(false);
    self.generate_hiz(main_geometry_context);
    run_geometry_pass(true);

    if (terrain != nullptr) {
      self.prepared_frame.terrain_patch_visibility_mask_buffer = std::move(
        terrain_context.patch_visibility_mask_buffer
      );
    }

    if (has_meshes || terrain != nullptr) {
      {
        auto meshlet_instances_buffer = has_meshes ? std::move(self.prepared_frame.meshlet_instances_buffer)
                                                   : self.renderer.render_context->alloc_transient_buffer(
                                                       vuk::MemoryUsage::eGPUonly,
                                                       sizeof(GPU::MeshletInstance)
                                                     );
        auto mesh_instances_buffer = has_meshes ? std::move(self.prepared_frame.mesh_instances_buffer)
                                                : self.renderer.render_context->alloc_transient_buffer(
                                                    vuk::MemoryUsage::eGPUonly,
                                                    sizeof(GPU::MeshInstance)
                                                  );

        RenderStageContext
          ctx(self, self.shared_resources, RenderStage::VisBufferEncode, *self.renderer.render_context);
        ctx.set_viewport_size(viewport_size)
          .set_image_resource("visbuffer_attachment", std::move(main_geometry_context.visbuffer_attachment))
          .set_image_resource("depth_attachment", std::move(main_geometry_context.depth_attachment))
          .set_buffer_resource("meshlet_instances_buffer", std::move(meshlet_instances_buffer))
          .set_buffer_resource("mesh_instances_buffer", std::move(mesh_instances_buffer));

        self.execute_stages_after(RenderStage::VisBufferEncode, ctx);

        main_geometry_context.visbuffer_attachment = ctx.get_image_resource("visbuffer_attachment");
        main_geometry_context.depth_attachment = ctx.get_image_resource("depth_attachment");
        if (has_meshes) {
          self.prepared_frame.meshlet_instances_buffer = ctx.get_buffer_resource("meshlet_instances_buffer");
          self.prepared_frame.mesh_instances_buffer = ctx.get_buffer_resource("mesh_instances_buffer");
        }
      }

      if (has_meshes) {
        self.decode_visbuffer(main_geometry_context);
      }
    }

    if (terrain != nullptr) {
      auto terrain_decode_context = TerrainDecodeContext{
        .bindless_set = &bindless_set,
        .terrain_buffer = std::move(terrain_context.terrain_buffer),
        .brush_hit_buffer = std::move(terrain_brush_context.hit_buffer),
        .normalmap_attachment = std::move(terrain_brush_context.maps.normalmap),
        .splatmap_attachment = std::move(terrain_brush_context.maps.splatmap),
        .visbuffer_attachment = std::move(main_geometry_context.visbuffer_attachment),
        .depth_attachment = std::move(main_geometry_context.depth_attachment),
        .albedo_attachment = std::move(main_geometry_context.albedo_attachment),
        .normal_attachment = std::move(main_geometry_context.normal_attachment),
        .emissive_attachment = std::move(main_geometry_context.emissive_attachment),
        .metallic_roughness_occlusion_attachment = std::move(
          main_geometry_context.metallic_roughness_occlusion_attachment
        ),
      };
      self.decode_terrain(terrain_decode_context);

      main_geometry_context.visbuffer_attachment = std::move(terrain_decode_context.visbuffer_attachment);
      main_geometry_context.depth_attachment = std::move(terrain_decode_context.depth_attachment);
      main_geometry_context.albedo_attachment = std::move(terrain_decode_context.albedo_attachment);
      main_geometry_context.normal_attachment = std::move(terrain_decode_context.normal_attachment);
      main_geometry_context.emissive_attachment = std::move(terrain_decode_context.emissive_attachment);
      main_geometry_context.metallic_roughness_occlusion_attachment = std::move(
        terrain_decode_context.metallic_roughness_occlusion_attachment
      );
    }

    visbuffer_attachment = std::move(main_geometry_context.visbuffer_attachment);
    depth_attachment = std::move(main_geometry_context.depth_attachment);
    overdraw_attachment = std::move(main_geometry_context.overdraw_attachment);
    albedo_attachment = std::move(main_geometry_context.albedo_attachment);
    normal_attachment = std::move(main_geometry_context.normal_attachment);
    emissive_attachment = std::move(main_geometry_context.emissive_attachment);
    metallic_roughness_occlusion_attachment = std::move(main_geometry_context.metallic_roughness_occlusion_attachment);

    const auto has_shadow_pointspot = self.prepared_frame.shadow_point_light_count +
                                        self.prepared_frame.shadow_spot_light_count >
                                      0;
    if (
      !(self.gpu_scene_flags & GPU::SceneFlags::HasDirectionalLight) || !self.directional_light_cast_shadows ||
      !has_meshes
    ) {
      self.directional_vsm_cache_valid = false;
    }
    if ((self.directional_light_cast_shadows || has_shadow_pointspot) && has_meshes) {
      auto rmvsm_context = RMVSMContext{
        .bindless_set = &bindless_set,
        .sun_moved = self.sun_direction_changed,
        .depth_extent = dst_extent,
        .depth_attachment = std::move(depth_attachment),
        .normal_attachment = std::move(normal_attachment),
        .light_grid_origin = light_grid_context.grid_origin,
        .light_grid_buffer = std::move(light_grid_context.light_grid_buffer),
      };
      self.draw_virtual_shadowmap(rmvsm_context);
      depth_attachment = std::move(rmvsm_context.depth_attachment);
      normal_attachment = std::move(rmvsm_context.normal_attachment);
      light_grid_context.light_grid_buffer = std::move(rmvsm_context.light_grid_buffer);

      vsm_chain_ran = true;
      pointspot_views_for_lighting = std::move(rmvsm_context.pointspot_views_buffer);
      pointspot_page_table_for_lighting = std::move(rmvsm_context.pointspot_page_table_attachment);

      if (self.directional_light_cast_shadows) {
        auto shadow_resolve_context = ShadowResolveContext{
          .directional_clipmaps_buffer = std::move(rmvsm_context.directional_clipmaps_buffer),
          .depth_attachment = std::move(depth_attachment),
          .normal_attachment = std::move(normal_attachment),
          .virtual_page_table_attachment = std::move(rmvsm_context.virtual_page_table_attachment),
          .physical_page_table_attachment = std::move(rmvsm_context.physical_page_table_attachment),
          .shadows_attachment = std::move(shadows_attachment),
        };
        self.resolve_shadowmap(shadow_resolve_context);
        depth_attachment = std::move(shadow_resolve_context.depth_attachment);
        normal_attachment = std::move(shadow_resolve_context.normal_attachment);
        shadows_attachment = std::move(shadow_resolve_context.shadows_attachment);
        rmvsm_virtual_page_table_attachment = std::move(shadow_resolve_context.virtual_page_table_attachment);
        rmvsm_virtual_clipmaps_buffer = std::move(shadow_resolve_context.directional_clipmaps_buffer);
        vsm_physical_pages_for_lighting = std::move(shadow_resolve_context.physical_page_table_attachment);
      } else {
        vsm_physical_pages_for_lighting = std::move(rmvsm_context.physical_page_table_attachment);
        rmvsm_virtual_page_table_attachment = std::move(rmvsm_context.virtual_page_table_attachment);
      }
    }

    auto contact_shadows_pass = vuk::make_pass(
      "contact_shadows",
      [sun_dir = self.directional_light.direction, &cvar](
        vuk::CommandBuffer& cmd_list,
        VUK_IA(vuk::eComputeRW) result,
        VUK_IA(vuk::eComputeSampled) src_depth,
        VUK_BA(vuk::eComputeRead) camera
      ) {
        const u32 steps = static_cast<u32>(cvar.cvar_contact_shadows_steps.get());
        const f32 thickness = cvar.cvar_contact_shadows_thickness.get();
        const f32 length = cvar.cvar_contact_shadows_length.get();

        cmd_list //
          .bind_compute_pipeline("contact_shadows")
          .bind_image(0, 0, src_depth)
          .bind_image(0, 1, result)
          .bind_buffer(0, 2, camera)
          .bind_sampler(0, 3, vuk::NearestSamplerClamped)
          .bind_sampler(0, 4, vuk::LinearSamplerClamped)
          .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(sun_dir, steps, thickness, length))
          .dispatch_invocations_per_pixel(result);

        return std::make_tuple(result, src_depth, camera);
      }
    );

    std::tie(shadows_attachment, depth_attachment, self.prepared_frame.camera_buffer) = contact_shadows_pass(
      std::move(shadows_attachment),
      std::move(depth_attachment),
      std::move(self.prepared_frame.camera_buffer)
    );
  }

  if (scene_has_atmosphere && scene_has_directional_light) {
    auto atmos_context = AtmosphereContext{
      .sky_transmittance_lut_attachment = sky_transmittance_lut_attachment,
      .sky_multiscatter_lut_attachment = sky_multiscatter_lut_attachment,
      .sky_view_lut_attachment = std::move(sky_view_lut_attachment),
      .sky_cubemap_attachment = std::move(sky_cubemap_attachment),
      .sky_aerial_perspective_lut_attachment = std::move(sky_aerial_perspective_attachment),
    };
    self.draw_atmosphere(atmos_context);

    sky_transmittance_lut_attachment = std::move(atmos_context.sky_transmittance_lut_attachment);
    sky_multiscatter_lut_attachment = std::move(atmos_context.sky_multiscatter_lut_attachment);
    sky_view_lut_attachment = std::move(atmos_context.sky_view_lut_attachment);
    sky_cubemap_attachment = std::move(atmos_context.sky_cubemap_attachment);
    sky_aerial_perspective_attachment = std::move(atmos_context.sky_aerial_perspective_lut_attachment);
  }

  const auto tlas_it = self.shared_resources.buffer_resources.find("tlas");
  const auto use_rtao = cvar.cvar_rtao_enable.as_bool() && tlas_it != self.shared_resources.buffer_resources.end();
  if (use_rtao) {
    // RTAO feeds the same attachment the GTAO path writes, so PBR needs the flag either way.
    self.gpu_scene_flags |= GPU::SceneFlags::HasGTAO;

    auto rtao_context = RTAOContext{
      .tlas = &self.scene_tlas,
      .ray_count = static_cast<u32>(std::max(cvar.cvar_rtao_ray_count.get(), 1)),
      .radius = cvar.cvar_rtao_radius.get(),
      .power = cvar.cvar_rtao_power.get(),
      .frame_index = static_cast<u32>(self.renderer.render_context->num_frames),
      .tlas_buffer = std::move(tlas_it->second),
      .normal_attachment = std::move(normal_attachment),
      .depth_attachment = std::move(depth_attachment),
      .ambient_occlusion_attachment = std::move(vbgtao_occlusion_attachment),
    };
    self.generate_rtao(rtao_context);

    tlas_it->second = std::move(rtao_context.tlas_buffer);
    normal_attachment = std::move(rtao_context.normal_attachment);
    depth_attachment = std::move(rtao_context.depth_attachment);
    vbgtao_occlusion_attachment = std::move(rtao_context.ambient_occlusion_attachment);
  } else if (self.gpu_scene_flags & GPU::SceneFlags::HasGTAO) {
    if (debug_uses_visbuffer) {
      vbgtao_depth_differences_attachment = vuk::declare_ia(
        "vbgtao depth differences",
        {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage,
         .extent = dst_extent,
         .format = vuk::Format::eR32Uint,
         .sample_count = vuk::Samples::e1,
         .level_count = 1,
         .layer_count = 1}
      );
    } else {
      vbgtao_depth_differences_attachment = std::move(visbuffer_attachment);
    }

    auto ao_context = AmbientOcclusionContext{
      .noise_attachment = std::move(hilbert_noise_lut_attachment),
      .normal_attachment = std::move(normal_attachment),
      .depth_attachment = std::move(depth_attachment),
      .depth_differences_attachment = std::move(vbgtao_depth_differences_attachment),
      .ambient_occlusion_attachment = std::move(vbgtao_occlusion_attachment),
    };
    self.generate_ambient_occlusion(ao_context);

    hilbert_noise_lut_attachment = std::move(ao_context.noise_attachment);
    normal_attachment = std::move(ao_context.normal_attachment);
    depth_attachment = std::move(ao_context.depth_attachment);
    vbgtao_depth_differences_attachment = std::move(ao_context.depth_differences_attachment);
    vbgtao_occlusion_attachment = std::move(ao_context.ambient_occlusion_attachment);

    if (!debug_uses_visbuffer) {
      visbuffer_attachment = std::move(vbgtao_depth_differences_attachment);
    }
  }

  if (!vsm_chain_ran) {
    pointspot_page_table_for_lighting = self.vsm_pointspot_virtual_page_table.acquire(
      "vsm pointspot page table",
      vuk::eFragmentSampled
    );
    rmvsm_virtual_page_table_attachment = self.vsm_virtual_page_table.acquire(
      "vsm virtual page table",
      vuk::eFragmentSampled
    );
    vsm_physical_pages_for_lighting = vuk::acquire_ia(
      "vsm physical page table",
      self.vsm_physical_page_table_attachment,
      vuk::eFragmentSampled
    );
    constexpr static auto pointspot_views_size_bytes = RMVSMContext::POINT_SPOT_LAYER_COUNT *
                                                       sizeof(GPU::VSMPointSpotView);
    pointspot_views_for_lighting = self.renderer.render_context->alloc_transient_buffer(
      vuk::MemoryUsage::eCPUtoGPU,
      pointspot_views_size_bytes
    );
    std::memset(pointspot_views_for_lighting->mapped_ptr, 0, pointspot_views_size_bytes);
  }

  // --- DDGI Probe Tracing ---
  const auto draw_ddgi_probes = debug_view == GPU::DebugView::DDGIProbes && debugging;
  const auto ddgi_distance_culling_enabled = cvar.cvar_ddgi_distance_culling.as_bool();
  const auto ddgi_distance_culling_changed = ddgi_distance_culling_enabled != self.ddgi_distance_culling_enabled;
  auto ddgi_irradiance_attachment = vuk::Value<vuk::ImageAttachment>{};
  auto ddgi_distance_attachment = vuk::Value<vuk::ImageAttachment>{};
  auto ddgi_probe_states_buffer = vuk::Value<vuk::Buffer>{};
  auto ddgi_atlas_valid = false;
  if (!self.probe_volumes.empty()) {
    auto total_probe_count = 0_u32;
    for (const auto& volume : self.probe_volumes) {
      total_probe_count += volume.probe_count;
    }

    const auto rays_per_probe = static_cast<u32>(std::clamp(cvar.cvar_ddgi_rays_per_probe.get(), 8, 512));
    const auto ray_data_extent = GPU::ddgi_ray_data_extent(total_probe_count, rays_per_probe);

    self.allocate_ddgi_atlases(total_probe_count);

    ddgi_probe_states_buffer = vuk::acquire_buf("ddgi probe states", *self.ddgi_probe_states, vuk::eMemoryRead);
    if (!self.ddgi_history_valid) {
      auto clear_states_pass = vuk::make_pass(
        "ddgi clear probe states",
        [](vuk::CommandBuffer& cmd_list, VUK_BA(vuk::eClear) probe_states) {
          cmd_list.fill_buffer(probe_states, 0u);
          return probe_states;
        }
      );

      ddgi_probe_states_buffer = clear_states_pass(std::move(ddgi_probe_states_buffer));
    }

    if (tlas_it != self.shared_resources.buffer_resources.end() && frame_render_context.use_ray_tracing_pipeline()) {
      auto ray_data_attachment = vuk::declare_ia(
        "ddgi ray data",
        {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage,
         .extent = ray_data_extent,
         .format = vuk::Format::eR16G16B16A16Sfloat,
         .sample_count = vuk::Samples::e1,
         .level_count = 1,
         .layer_count = 1}
      );

      auto irradiance_attachment =
        self.ddgi_history_valid
          ? vuk::acquire_ia("ddgi irradiance", self.ddgi_irradiance_attachment, vuk::eFragmentSampled)
          : vuk::clear_image(vuk::discard_ia("ddgi irradiance", self.ddgi_irradiance_attachment), vuk::Black<f32>);
      auto distance_attachment =
        self.ddgi_history_valid
          ? vuk::acquire_ia("ddgi distance", self.ddgi_distance_attachment, vuk::eFragmentSampled)
          : vuk::clear_image(vuk::discard_ia("ddgi distance", self.ddgi_distance_attachment), vuk::Black<f32>);

      auto ddgi_select_context = DDGISelectContext{
        .frame_index = static_cast<u32>(self.renderer.render_context->num_frames),
        .max_interval = static_cast<u32>(std::clamp(cvar.cvar_ddgi_update_max_interval.get(), 1, 255)),
        .full_rate_distance = cvar.cvar_ddgi_update_full_rate_distance.get(),
        .update_all = !self.ddgi_history_valid,
        .force_update_all = ddgi_distance_culling_changed,
        .distance_culling_enabled = ddgi_distance_culling_enabled,
        .probe_volumes_buffer = self.renderer.render_context->scratch_buffer_span(std::span(self.probe_volumes)),
        .probe_states_buffer = std::move(ddgi_probe_states_buffer),
        .probe_update_list_buffer = vuk::acquire_buf(
          "ddgi probe update list",
          *self.ddgi_probe_update_list,
          vuk::eComputeRead
        ),
        // Uploaded zeroed so ddgi_select_probes can atomically count into it without a clear pass.
        .probe_update_args_buffer = self.renderer.render_context->scratch_buffer(GPU::ProbeUpdateArgs{}),
      };
      self.select_ddgi_probes(ddgi_select_context);

      auto ddgi_trace_context = DDGITraceContext{
        .bindless_set = &bindless_set,
        .tlas = &self.scene_tlas,
        .scene_flags = self.gpu_scene_flags,
        .rays_per_probe = rays_per_probe,
        .frame_index = static_cast<u32>(self.renderer.render_context->num_frames),
        .light_count = static_cast<u32>(self.scene.lights.size()),
        .max_ray_distance = cvar.cvar_ddgi_max_ray_distance.get(),
        .max_ray_radiance = cvar.cvar_ddgi_max_ray_radiance.get(),
        .shadow_ray_offset = cvar.cvar_ddgi_shadow_ray_offset.get(),
        .normal_bias = cvar.cvar_ddgi_normal_bias.get(),
        .sun_direction = self.directional_light.direction,
        .sun_intensity = self.directional_light.intensity,
        .ambient_color = self.sky_data.ambient_color,
        .volume_count = static_cast<u32>(self.probe_volumes.size()),
        .radiance_atlas_y_offset = GPU::ddgi_radiance_atlas_y_offset(total_probe_count),
        .distance_culling_enabled = ddgi_distance_culling_enabled,
        .bounce_valid = self.ddgi_history_valid,
        .view_bias = cvar.cvar_ddgi_view_bias.get(),
        .light_grid_origin = light_grid_context.grid_origin,
        .tlas_buffer = std::move(tlas_it->second),
        .probe_volumes_buffer = std::move(ddgi_select_context.probe_volumes_buffer),
        .probe_states_buffer = std::move(ddgi_select_context.probe_states_buffer),
        .light_grid_buffer = std::move(light_grid_context.light_grid_buffer),
        .pointspot_views_buffer = std::move(pointspot_views_for_lighting),
        .sky_view_lut_attachment = std::move(sky_view_lut_attachment),
        .sky_transmittance_lut_attachment = std::move(sky_transmittance_lut_attachment),
        .pointspot_page_table_attachment = std::move(pointspot_page_table_for_lighting),
        .vsm_physical_pages_attachment = std::move(vsm_physical_pages_for_lighting),
        .ray_data_attachment = std::move(ray_data_attachment),
        .irradiance_attachment = std::move(irradiance_attachment),
        .distance_attachment = std::move(distance_attachment),
      };
      self.trace_ddgi_probes(ddgi_trace_context);

      tlas_it->second = std::move(ddgi_trace_context.tlas_buffer);
      sky_view_lut_attachment = std::move(ddgi_trace_context.sky_view_lut_attachment);
      sky_transmittance_lut_attachment = std::move(ddgi_trace_context.sky_transmittance_lut_attachment);
      light_grid_context.light_grid_buffer = std::move(ddgi_trace_context.light_grid_buffer);
      pointspot_views_for_lighting = std::move(ddgi_trace_context.pointspot_views_buffer);
      pointspot_page_table_for_lighting = std::move(ddgi_trace_context.pointspot_page_table_attachment);
      vsm_physical_pages_for_lighting = std::move(ddgi_trace_context.vsm_physical_pages_attachment);

      auto ddgi_relocate_context = DDGIRelocateContext{
        .rays_per_probe = rays_per_probe,
        .frame_index = static_cast<u32>(self.renderer.render_context->num_frames),
        .min_frontface_distance = cvar.cvar_ddgi_min_frontface_distance.get(),
        .relocation_enabled = cvar.cvar_ddgi_probe_relocation.as_bool(),
        .distance_culling_enabled = ddgi_distance_culling_enabled,
        .probe_volumes_buffer = std::move(ddgi_trace_context.probe_volumes_buffer),
        .probe_states_buffer = std::move(ddgi_trace_context.probe_states_buffer),
        .probe_update_list_buffer = std::move(ddgi_select_context.probe_update_list_buffer),
        .probe_update_args_buffer = std::move(ddgi_select_context.probe_update_args_buffer),
        .ray_data_attachment = std::move(ddgi_trace_context.ray_data_attachment),
      };
      self.relocate_ddgi_probes(ddgi_relocate_context);

      ddgi_trace_context.probe_volumes_buffer = std::move(ddgi_relocate_context.probe_volumes_buffer);
      ddgi_trace_context.probe_states_buffer = std::move(ddgi_relocate_context.probe_states_buffer);
      ddgi_trace_context.ray_data_attachment = std::move(ddgi_relocate_context.ray_data_attachment);
      ddgi_select_context.probe_update_list_buffer = std::move(ddgi_relocate_context.probe_update_list_buffer);
      ddgi_select_context.probe_update_args_buffer = std::move(ddgi_relocate_context.probe_update_args_buffer);

      auto ddgi_update_context = DDGIUpdateContext{
        .rays_per_probe = rays_per_probe,
        .frame_index = static_cast<u32>(self.renderer.render_context->num_frames),
        .radiance_atlas_y_offset = GPU::ddgi_radiance_atlas_y_offset(total_probe_count),
        .hysteresis = cvar.cvar_ddgi_hysteresis.get(),
        .max_brightness_step = cvar.cvar_ddgi_max_brightness_step.get(),
        .firefly_ratio = cvar.cvar_ddgi_firefly_ratio.get(),
        .hysteresis_dark_bias = cvar.cvar_ddgi_hysteresis_dark_bias.get(),
        .probe_volumes_buffer = std::move(ddgi_trace_context.probe_volumes_buffer),
        .probe_states_buffer = std::move(ddgi_trace_context.probe_states_buffer),
        .probe_update_list_buffer = std::move(ddgi_select_context.probe_update_list_buffer),
        .probe_update_args_buffer = std::move(ddgi_select_context.probe_update_args_buffer),
        .ray_data_attachment = std::move(ddgi_trace_context.ray_data_attachment),
        .irradiance_attachment = std::move(ddgi_trace_context.irradiance_attachment),
        .distance_attachment = std::move(ddgi_trace_context.distance_attachment),
      };
      self.update_ddgi_probes(ddgi_update_context);
      self.ddgi_distance_culling_enabled = ddgi_distance_culling_enabled;

      ddgi_irradiance_attachment = std::move(ddgi_update_context.irradiance_attachment);
      ddgi_distance_attachment = std::move(ddgi_update_context.distance_attachment);
      ddgi_probe_states_buffer = std::move(ddgi_update_context.probe_states_buffer);
      ddgi_atlas_valid = true;
      self.gpu_scene_flags |= GPU::SceneFlags::HasDDGI;
    }
  }

  auto pbr_context = PBRContext{
    .bindless_set = &bindless_set,
    .sky_transmittance_lut_attachment = std::move(sky_transmittance_lut_attachment),
    .sky_aerial_perspective_lut_attachment = std::move(sky_aerial_perspective_attachment),
    .sky_view_lut_attachment = std::move(sky_view_lut_attachment),
    .sky_cubemap_attachment = std::move(sky_cubemap_attachment),
    .depth_attachment = std::move(depth_attachment),
    .albedo_attachment = std::move(albedo_attachment),
    .normal_attachment = std::move(normal_attachment),
    .emissive_attachment = std::move(emissive_attachment),
    .metallic_roughness_occlusion_attachment = std::move(metallic_roughness_occlusion_attachment),
    .ambient_occlusion_attachment = std::move(vbgtao_occlusion_attachment),
    .shadows_attachment = std::move(shadows_attachment),
    .light_grid_origin = light_grid_context.grid_origin,
    .light_grid_buffer = std::move(light_grid_context.light_grid_buffer),
    .pointspot_views_buffer = std::move(pointspot_views_for_lighting),
    .pointspot_page_table_attachment = std::move(pointspot_page_table_for_lighting),
    .vsm_physical_pages_attachment = std::move(vsm_physical_pages_for_lighting),
    .vsm_page_table_attachment = std::move(rmvsm_virtual_page_table_attachment),
  };
  final_attachment = self.apply_pbr(pbr_context, std::move(final_attachment));
  rmvsm_virtual_page_table_attachment = std::move(pbr_context.vsm_page_table_attachment);
  depth_attachment = std::move(pbr_context.depth_attachment);
  albedo_attachment = std::move(pbr_context.albedo_attachment);
  normal_attachment = std::move(pbr_context.normal_attachment);
  emissive_attachment = std::move(pbr_context.emissive_attachment);
  metallic_roughness_occlusion_attachment = std::move(pbr_context.metallic_roughness_occlusion_attachment);
  vbgtao_occlusion_attachment = std::move(pbr_context.ambient_occlusion_attachment);

  auto rmvsm_pointspot_page_table_attachment = std::move(pbr_context.pointspot_page_table_attachment);
  auto rmvsm_pointspot_views_buffer = std::move(pbr_context.pointspot_views_buffer);
  auto rmvsm_light_grid_buffer = std::move(pbr_context.light_grid_buffer);
  const auto rmvsm_light_grid_origin = pbr_context.light_grid_origin;

  // --- DDGI Apply ---
  if (ddgi_atlas_valid) {
    auto ddgi_apply_context = DDGIApplyContext{
      .volume_count = static_cast<u32>(self.probe_volumes.size()),
      .normal_bias = cvar.cvar_ddgi_normal_bias.get(),
      .view_bias = cvar.cvar_ddgi_view_bias.get(),
      .intensity = cvar.cvar_ddgi_intensity.get(),
      .ambient_color = self.sky_data.ambient_color,
      .probe_volumes_buffer = self.renderer.render_context->scratch_buffer_span(std::span(self.probe_volumes)),
      .probe_states_buffer = std::move(ddgi_probe_states_buffer),
      .depth_attachment = std::move(depth_attachment),
      .albedo_attachment = std::move(albedo_attachment),
      .normal_attachment = std::move(normal_attachment),
      .metallic_roughness_occlusion_attachment = std::move(metallic_roughness_occlusion_attachment),
      .ambient_occlusion_attachment = std::move(vbgtao_occlusion_attachment),
      .irradiance_attachment = std::move(ddgi_irradiance_attachment),
      .distance_attachment = std::move(ddgi_distance_attachment),
    };

    final_attachment = self.apply_ddgi(ddgi_apply_context, std::move(final_attachment));

    depth_attachment = std::move(ddgi_apply_context.depth_attachment);
    albedo_attachment = std::move(ddgi_apply_context.albedo_attachment);
    normal_attachment = std::move(ddgi_apply_context.normal_attachment);
    metallic_roughness_occlusion_attachment = std::move(ddgi_apply_context.metallic_roughness_occlusion_attachment);
    vbgtao_occlusion_attachment = std::move(ddgi_apply_context.ambient_occlusion_attachment);
    ddgi_irradiance_attachment = std::move(ddgi_apply_context.irradiance_attachment);
    ddgi_distance_attachment = std::move(ddgi_apply_context.distance_attachment);
    ddgi_probe_states_buffer = std::move(ddgi_apply_context.probe_states_buffer);
  }

  // --- 2D Pass ---
  if (!self.render_queue_2d.sprite_data.empty()) {
    // WARN: rq2d is copied each frame (it needs to be copied)

    auto forward_2d_vis_pass = vuk::make_pass(
      "2d_forward_vis_pass",
      [rq2d = self.render_queue_2d, &descriptor_set = bindless_set](
        vuk::CommandBuffer& cmd_list,
        VUK_IA(vuk::eColorWrite) target,
        VUK_IA(vuk::eDepthStencilRW) depth,
        VUK_BA(vuk::eAttributeRead) vertex_buffer,
        VUK_BA(vuk::eVertexRead) materials,
        VUK_BA(vuk::eVertexRead) camera,
        VUK_BA(vuk::eVertexRead) transforms_
      ) {
        const auto vertex_pack_2d = vuk::Packed{
          vuk::Format::eR32Uint, // 4 material_id
          vuk::Format::eR32Uint, // 4 flags
          vuk::Format::eR32Uint, // 4 transforms_id
        };

        for (const auto& batch : rq2d.batches) {
          if (batch.count < 1)
            continue;

          cmd_list.bind_graphics_pipeline("2d_forward_vis")
            .set_depth_stencil(
              vuk::PipelineDepthStencilStateCreateInfo{
                .depthTestEnable = true,
                .depthWriteEnable = true,
                .depthCompareOp = vuk::CompareOp::eGreaterOrEqual,
              }
            )
            .set_dynamic_state(vuk::DynamicStateFlagBits::eScissor | vuk::DynamicStateFlagBits::eViewport)
            .set_viewport(0, vuk::Rect2D::framebuffer())
            .set_scissor(0, vuk::Rect2D::framebuffer())
            .broadcast_color_blend(vuk::BlendPreset::eOff)
            .set_rasterization({.cullMode = vuk::CullModeFlagBits::eNone})
            .bind_vertex_buffer(0, vertex_buffer, 0, vertex_pack_2d, vuk::VertexInputRate::eInstance)
            .push_constants(
              vuk::ShaderStageFlagBits::eVertex | vuk::ShaderStageFlagBits::eFragment,
              0,
              PushConstants(materials->device_address, camera->device_address, transforms_->device_address)
            )
            .bind_persistent(1, descriptor_set)
            .draw(6, batch.count, 0, batch.offset);
        }

        return std::make_tuple(target, depth, camera, vertex_buffer, materials, transforms_);
      }
    );

    std::tie(
      visbuffer_attachment_2d,
      depth_attachment,
      self.prepared_frame.camera_buffer,
      vertex_buffer_2d,
      self.prepared_frame.materials_buffer,
      self.prepared_frame.transforms_world_buffer
    ) =
      forward_2d_vis_pass(
        std::move(visbuffer_attachment_2d),
        std::move(depth_attachment),
        std::move(vertex_buffer_2d),
        std::move(self.prepared_frame.materials_buffer),
        std::move(self.prepared_frame.camera_buffer),
        std::move(self.prepared_frame.transforms_world_buffer)
      );

    auto forward_2d_pass = vuk::make_pass(
      "2d_forward_pass",
      [rq2d = self.render_queue_2d, &descriptor_set = bindless_set](
        vuk::CommandBuffer& cmd_list,
        VUK_IA(vuk::eColorWrite) target,
        VUK_IA(vuk::eDepthStencilRW) depth,
        VUK_BA(vuk::eAttributeRead) vertex_buffer,
        VUK_BA(vuk::eVertexRead) materials,
        VUK_BA(vuk::eVertexRead) camera,
        VUK_BA(vuk::eVertexRead) transforms_
      ) {
        const auto vertex_pack_2d = vuk::Packed{
          vuk::Format::eR32Uint, // 4 material_id
          vuk::Format::eR32Uint, // 4 flags
          vuk::Format::eR32Uint, // 4 transforms_id
        };

        for (const auto& batch : rq2d.batches) {
          if (batch.count < 1)
            continue;

          cmd_list.bind_graphics_pipeline(batch.pipeline_name)
            .set_depth_stencil(
              vuk::PipelineDepthStencilStateCreateInfo{
                .depthTestEnable = true,
                .depthWriteEnable = true,
                .depthCompareOp = vuk::CompareOp::eGreaterOrEqual,
              }
            )
            .set_dynamic_state(vuk::DynamicStateFlagBits::eScissor | vuk::DynamicStateFlagBits::eViewport)
            .set_viewport(0, vuk::Rect2D::framebuffer())
            .set_scissor(0, vuk::Rect2D::framebuffer())
            .broadcast_color_blend(vuk::BlendPreset::eAlphaBlend)
            .set_rasterization({.cullMode = vuk::CullModeFlagBits::eNone})
            .bind_vertex_buffer(0, vertex_buffer, 0, vertex_pack_2d, vuk::VertexInputRate::eInstance)
            .push_constants(
              vuk::ShaderStageFlagBits::eVertex | vuk::ShaderStageFlagBits::eFragment,
              0,
              PushConstants(materials->device_address, camera->device_address, transforms_->device_address)
            )
            .bind_persistent(1, descriptor_set)
            .draw(6, batch.count, 0, batch.offset);
        }

        return std::make_tuple(target, depth, camera, vertex_buffer, materials, transforms_);
      }
    );

    std::tie(
      final_attachment,
      depth_attachment,
      self.prepared_frame.camera_buffer,
      vertex_buffer_2d,
      self.prepared_frame.materials_buffer,
      self.prepared_frame.transforms_world_buffer
    ) =
      forward_2d_pass(
        std::move(final_attachment),
        std::move(depth_attachment),
        std::move(vertex_buffer_2d),
        std::move(self.prepared_frame.materials_buffer),
        std::move(self.prepared_frame.camera_buffer),
        std::move(self.prepared_frame.transforms_world_buffer)
      );

    RenderStageContext ctx(self, self.shared_resources, RenderStage::Forward2D, *self.renderer.render_context);
    ctx.set_viewport_size(viewport_size)
      .set_image_resource("final_attachment", final_attachment)
      .set_image_resource("visbuffer_attachment_2d", std::move(visbuffer_attachment_2d));

    self.execute_stages_after(RenderStage::Forward2D, ctx);

    visbuffer_attachment_2d = ctx.get_image_resource("visbuffer_attachment_2d");
    final_attachment = ctx.get_image_resource("final_attachment");
  }

  // --- Particle Simulation Pass ---
  if (!self.prepared_frame.particle_emitters.empty()) {
    auto particle_context = ParticleContext{
      .total_capacity = self.prepared_frame.particle_total_capacity,
      .sorted_count = self.prepared_frame.particle_sorted_count,
      .emitter_count = static_cast<u32>(self.prepared_frame.particle_emitters.size()),
      .total_spawn = self.prepared_frame.particle_total_spawn,
      .needs_init = self.prepared_frame.particle_pool_reset,
      .sort_enabled = self.prepared_frame.particle_sort_enabled,
      .mesh_draws = std::span(self.prepared_frame.particle_mesh_draws),
      .emitters_buffer = self.renderer.render_context->scratch_buffer_span(
        std::span(self.prepared_frame.particle_emitters)
      ),
      .program_buffer = self.renderer.render_context->scratch_buffer_span(
        std::span(self.prepared_frame.particle_instructions)
      ),
      .constants_buffer = self.renderer.render_context->scratch_buffer_span(
        std::span(self.prepared_frame.particle_constants)
      ),
      .camera_buffer = std::move(self.prepared_frame.camera_buffer),
      .materials_buffer = std::move(self.prepared_frame.materials_buffer),
      .depth_attachment = std::move(depth_attachment),
    };

    self.simulate_particles(particle_context);
    final_attachment = self.draw_particles(particle_context, std::move(final_attachment));

    depth_attachment = std::move(particle_context.depth_attachment);
    self.prepared_frame.camera_buffer = std::move(particle_context.camera_buffer);
    self.prepared_frame.materials_buffer = std::move(particle_context.materials_buffer);
  }

  // --- DDGI Probe Debug Pass ---
  if (draw_ddgi_probes && !self.probe_volumes.empty()) {
    if (!ddgi_atlas_valid) {
      self.ddgi_history_valid = false;
    }

    auto ddgi_debug_context = DDGIDebugContext{
      .probe_radius = cvar.cvar_ddgi_probe_debug_radius.get(),
      .atlas_valid = ddgi_atlas_valid,
      .probe_volumes_buffer = self.renderer.render_context->scratch_buffer_span(std::span(self.probe_volumes)),
      .probe_states_buffer = std::move(ddgi_probe_states_buffer),
      .irradiance_attachment = ddgi_atlas_valid ? std::move(ddgi_irradiance_attachment)
                                                : vuk::discard_ia("ddgi irradiance", self.ddgi_irradiance_attachment),
      .depth_attachment = std::move(depth_attachment),
    };

    final_attachment = self.draw_ddgi_probes(ddgi_debug_context, std::move(final_attachment));
    depth_attachment = std::move(ddgi_debug_context.depth_attachment);
    ddgi_irradiance_attachment = std::move(ddgi_debug_context.irradiance_attachment);
    ddgi_probe_states_buffer = std::move(ddgi_debug_context.probe_states_buffer);
  }

  // --- FXAA Pass ---
  if (self.gpu_scene_flags & GPU::SceneFlags::HasFXAA) {
    auto fxaa_attachment = vuk::declare_ia(
      "fxaa_attachment",
      {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
       .sample_count = vuk::Samples::e1}
    );
    fxaa_attachment.same_shape_as(final_attachment);
    fxaa_attachment.same_format_as(final_attachment);
    fxaa_attachment = vuk::clear_image(std::move(fxaa_attachment), vuk::Black<f32>);

    auto fxaa_pass = vuk::make_pass(
      "fxaa",
      [](vuk::CommandBuffer& cmd_list, VUK_IA(vuk::eColorWrite) dst, VUK_IA(vuk::eFragmentSampled) src) {
        const glm::vec2 inverse_screen_size = 1.f / glm::vec2(src->extent.width, src->extent.height);
        cmd_list.bind_graphics_pipeline("fxaa")
          .set_rasterization({})
          .set_color_blend(dst, {})
          .set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
          .set_viewport(0, vuk::Rect2D::framebuffer())
          .set_scissor(0, vuk::Rect2D::framebuffer())
          .bind_image(0, 0, src)
          .bind_sampler(0, 1, vuk::LinearSamplerClamped)
          .push_constants(vuk::ShaderStageFlagBits::eFragment, 0, PushConstants(inverse_screen_size))
          .draw(3, 1, 0, 0);
        return std::make_tuple(dst, src);
      }
    );

    std::tie(final_attachment, fxaa_attachment) = fxaa_pass(std::move(fxaa_attachment), std::move(final_attachment));
  }

  auto bloom_upsampled_attachment = vuk::Value<vuk::ImageAttachment>{};
  if (self.gpu_scene_flags & GPU::SceneFlags::HasBloom) {
    const auto bloom_extent = vuk::Extent3D{
      .width = std::max(dst_extent.width / 2, 1u),
      .height = std::max(dst_extent.height / 2, 1u),
      .depth = 1,
    };
    bloom_upsampled_attachment = vuk::declare_ia(
      "bloom upsampled",
      {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage,
       .extent = bloom_extent,
       .format = vuk::Format::eB10G11R11UfloatPack32,
       .sample_count = vuk::SampleCountFlagBits::e1,
       .level_count = Texture::calculate_mip_count(bloom_extent),
       .layer_count = 1}
    );
    bloom_upsampled_attachment = vuk::clear_image(std::move(bloom_upsampled_attachment), vuk::Black<float>);
  } else {
    bloom_upsampled_attachment = vuk::declare_ia(
      "bloom disabled",
      {.usage = vuk::ImageUsageFlagBits::eSampled,
       .extent = {1, 1, 1},
       .format = vuk::Format::eB10G11R11UfloatPack32,
       .sample_count = vuk::SampleCountFlagBits::e1,
       .level_count = 1,
       .layer_count = 1}
    );
    bloom_upsampled_attachment = vuk::clear_image(std::move(bloom_upsampled_attachment), vuk::Black<float>);
  }

  /// POST PROCESSING
  auto post_process_context = PostProcessContext{
    .delta_time = static_cast<f32>(App::get_timestep().get_millis()) * 0.001f,
    .extent = dst_extent,
    .dst_attachment = std::move(dst_attachment),
    .final_attachment = std::move(final_attachment),
    .bloom_upsampled_attachment = std::move(bloom_upsampled_attachment),
  };

  if (self.gpu_scene_flags & GPU::SceneFlags::HasEyeAdaptation) {
    self.apply_eye_adaptation(post_process_context);
  }

  if (self.gpu_scene_flags & GPU::SceneFlags::HasBloom) {
    self.apply_bloom(post_process_context, cvar);
  }

  dst_attachment = self.apply_tonemap(post_process_context);

  {
    RenderStageContext ctx(self, self.shared_resources, RenderStage::PostProcessing, *self.renderer.render_context);
    ctx.set_viewport_size(viewport_size)
      .set_buffer_resource("camera_buffer", std::move(self.prepared_frame.camera_buffer))
      .set_image_resource("depth_attachment", std::move(depth_attachment))
      .set_image_resource("result_attachment", std::move(dst_attachment));

    self.execute_stages_after(RenderStage::PostProcessing, ctx);

    self.prepared_frame.camera_buffer = ctx.get_buffer_resource("camera_buffer");
    depth_attachment = ctx.get_image_resource("depth_attachment");
    dst_attachment = ctx.get_image_resource("result_attachment");
  }

  auto debug_context = DebugContext{
    .overdraw_heatmap_scale = debug_heatmap_scale,
    .debug_view = debug_view,
    .visbuffer_attachment = std::move(visbuffer_attachment),
    // The bounding-box pass also consumes depth after the debug view is composed.
    .depth_attachment = depth_attachment,
    .overdraw_attachment = std::move(overdraw_attachment),
    .albedo_attachment = std::move(albedo_attachment),
    .normal_attachment = std::move(normal_attachment),
    .emissive_attachment = std::move(emissive_attachment),
    .metallic_roughness_occlusion_attachment = std::move(metallic_roughness_occlusion_attachment),
    .ambient_occlusion_attachment = std::move(vbgtao_occlusion_attachment),
  };

  if (debug_view == GPU::DebugView::RMVSM) {
    debug_context.vsm_page_table_attachment = std::move(rmvsm_virtual_page_table_attachment);
    debug_context.vsm_clipmaps_buffer = std::move(rmvsm_virtual_clipmaps_buffer);
  } else if (debug_view == GPU::DebugView::RMVSMPointSpot) {
    debug_context.light_grid_origin = rmvsm_light_grid_origin;
    debug_context.pointspot_views_buffer = std::move(rmvsm_pointspot_views_buffer);
    debug_context.light_grid_buffer = std::move(rmvsm_light_grid_buffer);
    debug_context.vsm_pointspot_page_table_attachment = std::move(rmvsm_pointspot_page_table_attachment);
  }

  if (debugging && debug_view != GPU::DebugView::DDGIProbes && self.prepared_frame.mesh_instance_count > 0) {
    dst_attachment = self.apply_debug_view(debug_context, std::move(dst_attachment));
  }

  if (cvar.cvar_enable_debug_renderer.as_bool()) {
    dst_attachment = self.draw_bounding_boxes(std::move(depth_attachment), std::move(dst_attachment));
  }

  return dst_attachment;
}

auto RendererInstance::tlas_has_consumer(this const RendererInstance& self, const RendererCVar& cvar) -> bool {
  return cvar.cvar_rtao_enable.as_bool() ||
         (!self.probe_volumes.empty() && self.renderer.render_context->use_ray_tracing_pipeline());
}

auto RendererInstance::update(this RendererInstance& self, RendererInstanceUpdateInfo& info, const RendererCVar& cvar)
  -> void {
  ZoneScoped;

  self.update_ran_this_frame = true;

  auto& asset_man = App::mod<AssetManager>();
  auto& render_context = *self.renderer.render_context;

  self.gpu_scene_flags = {};
  self.prepared_frame = {};

  CameraComponent current_camera = {};
  CameraComponent frozen_camera = {};
  const auto freeze_culling = static_cast<bool>(cvar.cvar_freeze_culling_frustum.get());
  // an explicitly active camera wins over a later one; with none marked the last one still does,
  // which is what every single-camera scene has always relied on
  auto found_active_camera = false;

  self.scene.world
    .query_builder<const TransformComponent, const CameraComponent>() //
    .build()
    .each([&](flecs::entity e, const TransformComponent& tc, const CameraComponent& c) {
      if (freeze_culling && !self.saved_camera) {
        self.saved_camera = true;
        frozen_camera = current_camera;
      } else if (!freeze_culling && self.saved_camera) {
        self.saved_camera = false;
      }

      if (
        static_cast<bool>(cvar.cvar_freeze_culling_frustum.get()) &&
        static_cast<bool>(cvar.cvar_draw_camera_frustum.get())
      ) {
        const auto proj = frozen_camera.get_projection_matrix() * frozen_camera.get_view_matrix();
        auto& debug_renderer = App::mod<ox::DebugRenderer>();
        debug_renderer.draw_frustum(proj, glm::vec4(0, 1, 0, 1), frozen_camera.near_clip, frozen_camera.far_clip);
      }

      if (c.active || !found_active_camera) {
        current_camera = c;
        found_active_camera = c.active;
      }
    });

  CameraComponent cam = freeze_culling ? frozen_camera : current_camera;

  self.camera_data = GPU::CameraData{
    .position = glm::vec4(cam.position, 0.0f),
    .projection = cam.get_projection_matrix(),
    .inv_projection = cam.get_inv_projection_matrix(),
    .view = cam.get_view_matrix(),
    .inv_view = cam.get_inv_view_matrix(),
    .projection_view = cam.get_projection_matrix() * cam.get_view_matrix(),
    .inv_projection_view = cam.get_inverse_projection_view(),
    .previous_projection = self.previous_camera_data.projection,
    .previous_inv_projection = self.previous_camera_data.inv_projection,
    .previous_view = self.previous_camera_data.view,
    .previous_inv_view = self.previous_camera_data.inv_view,
    .previous_projection_view = self.previous_camera_data.projection_view,
    .previous_inv_projection_view = self.previous_camera_data.inv_projection_view,
    .temporalaa_jitter = cam.jitter,
    .temporalaa_jitter_prev = self.previous_camera_data.temporalaa_jitter_prev,
    .up = cam.up,
    .near_clip = cam.near_clip,
    .forward = cam.forward,
    .far_clip = cam.far_clip,
    .right = cam.right,
    .fov = cam.fov,
    .output_index = 0,
    .acceptable_lod_error = 2.0f,
  };

  self.previous_camera_data = self.camera_data;

  math::calc_frustum_planes(self.camera_data.projection_view, self.camera_data.frustum_planes);

  const auto light_grid_half_extent = glm::vec3(GPU::LIGHT_GRID_RESOLUTION) * (GPU::LIGHT_GRID_CELL_SIZE * 0.5f);
  self.light_grid_origin = glm::floor(
                             (glm::vec3(self.camera_data.position) - light_grid_half_extent) / GPU::LIGHT_GRID_CELL_SIZE
                           ) *
                           GPU::LIGHT_GRID_CELL_SIZE;

  self.scene.lights.reset();

  // rank lights before assigning the limited shadow slots
  ankerl::svector<PendingLight, 32> pending_lights = {};

  self.scene.world
    .query_builder<const TransformComponent, const LightComponent>() //
    .build()
    .each([&self, &pending_lights](flecs::entity e, const TransformComponent& tc, const LightComponent& lc) {
      if (!e.enabled()) {
        return;
      }

      const auto world_transform = self.scene.get_world_transform(e);
      const auto world_position = world_transform[3];
      const auto world_forward = glm::normalize(glm::mat3(world_transform) * glm::vec3(0.0f, 0.0f, -1.0f));

      if (lc.type == LightComponent::LightType::Directional) {
        self.gpu_scene_flags |= GPU::SceneFlags::HasDirectionalLight;
        self.directional_light.color = lc.color;
        self.directional_light.intensity = lc.intensity;
        self.sun_direction_changed = world_forward != self.previous_sun_direction;
        self.previous_sun_direction = world_forward;
        self.directional_light.direction = world_forward;
        self.first_clipmap_width = lc.first_clipmap_width;
        self.clipmap_selection_bias = lc.clipmap_selection_bias;

        self.directional_light_cast_shadows = lc.cast_shadows;
      } else {
        const auto kind = lc.type == LightComponent::LightType::Spot ? GPU::LightKind::Spot : GPU::LightKind::Point;
        const auto direction = lc.type == LightComponent::LightType::Spot ? world_forward : glm::vec3(0.0f);

        pending_lights.emplace_back(
          PendingLight{
            .light =
              GPU::Light{
                .position = world_position,
                .intensity = lc.intensity,
                .color = lc.color,
                .range = lc.radius,
                .direction = direction,
                .inner_cone_angle = lc.inner_cone_angle,
                .outer_cone_angle = lc.outer_cone_angle,
                .kind = kind,
                .shadow_map_index = -1,
              },
            .entity_id = static_cast<u64>(e.id()),
            .cast_shadows = lc.cast_shadows,
          }
        );
      }
    });

  // queried apart from the lights: an environment does not need a light on the same entity, and
  // riding along with one meant a scene that lit itself any other way silently lost its sky
  self.scene.world
    .query_builder<const AtmosphereComponent>() //
    .build()
    .each([&self](flecs::entity e, const AtmosphereComponent& atmos_info) {
      if (!e.enabled()) {
        return;
      }

      self.gpu_scene_flags |= GPU::SceneFlags::HasAtmosphere;

      self.atmosphere.rayleigh_scatter = atmos_info.rayleigh_scattering * 1e-3f;
      self.atmosphere.rayleigh_density = atmos_info.rayleigh_density;
      self.atmosphere.mie_scatter = atmos_info.mie_scattering * 1e-3f;
      self.atmosphere.mie_density = atmos_info.mie_density;
      self.atmosphere.mie_extinction = atmos_info.mie_extinction * 1e-3f;
      self.atmosphere.mie_asymmetry = atmos_info.mie_asymmetry;
      self.atmosphere.mie_haze_amount = atmos_info.mie_haze_amount;
      self.atmosphere.mie_haze_scale_height = atmos_info.mie_haze_scale_height;
      self.atmosphere.ozone_absorption = atmos_info.ozone_absorption * 1e-3f;
      self.atmosphere.ozone_height = atmos_info.ozone_height;
      self.atmosphere.ozone_thickness = atmos_info.ozone_thickness;
      self.atmosphere.aerial_perspective_start_km = atmos_info.aerial_perspective_start_km;
      self.atmosphere.aerial_perspective_exposure = atmos_info.aerial_perspective_exposure;
      self.atmosphere.sky_view_lut_size = self.sky_view_lut_extent;
      self.atmosphere.aerial_perspective_lut_size = self.sky_aerial_perspective_lut_extent;
      self.atmosphere.transmittance_lut_size = self.sky_transmittance_lut.get_extent();
      self.atmosphere.multiscattering_lut_size = self.sky_multiscatter_lut.get_extent();
    });

  self.scene.world
    .query_builder<const SkyComponent>() //
    .build()
    .each([&self](flecs::entity e, const SkyComponent& sky_info) {
      if (!e.enabled()) {
        return;
      }

      self.gpu_scene_flags |= GPU::SceneFlags::HasSky;

      self.sky_data.solid_color = sky_info.solid_color;
      self.sky_data.ambient_color = sky_info.ambient_color;
      self.sky_data.has_texture = static_cast<bool>(sky_info.texture);
    });

  if (
    (self.gpu_scene_flags & GPU::SceneFlags::HasAtmosphere) != 0 &&
    (!self.atmosphere_lut_state_valid || !atmosphere_lut_inputs_equal(self.atmosphere, self.atmosphere_lut_state))
  ) {
    self.atmosphere_lut_state = self.atmosphere;
    self.atmosphere_lut_state_valid = true;
    self.atmosphere_luts_dirty = true;
  }

  const auto point_slot_stats = assign_shadow_slots(
    pending_lights,
    GPU::LightKind::Point,
    self.shadow_point_slots,
    self.prepared_frame.shadow_point_light_count,
    self.prepared_frame.moved_point_light_mask
  );
  const auto spot_slot_stats = assign_shadow_slots(
    pending_lights,
    GPU::LightKind::Spot,
    self.shadow_spot_slots,
    self.prepared_frame.shadow_spot_light_count,
    self.prepared_frame.moved_spot_light_mask
  );

  self.prepared_frame.shadow_slots_reassigned = point_slot_stats.reassigned + spot_slot_stats.reassigned;
  self.prepared_frame.shadow_slots_invalidated = point_slot_stats.invalidated + spot_slot_stats.invalidated;

  // force worst-case cache rebuilds for profiling
  if (cvar.cvar_vsm_force_invalidate.as_bool()) {
    self.prepared_frame.moved_point_light_mask = ~0_u64;
    self.prepared_frame.moved_spot_light_mask = ~0_u64;
    self.sun_direction_changed = true;
  }

  TracyPlot("vsm shadow slots reassigned", static_cast<i64>(self.prepared_frame.shadow_slots_reassigned));
  TracyPlot("vsm shadow slots invalidated", static_cast<i64>(self.prepared_frame.shadow_slots_invalidated));

  for (auto& pending : pending_lights) {
    self.scene.lights.create_slot(std::move(pending.light));
  }

  self.probe_volumes.clear();
  self.probe_volume_keys.clear();
  if (cvar.cvar_ddgi_enable.as_bool()) {
    const auto draw_volume_bounds = static_cast<GPU::DebugView>(cvar.cvar_debug_view.get()) ==
                                      GPU::DebugView::DDGIProbes &&
                                    cvar.cvar_enable_debug_renderer.as_bool();

    // Cascades of one set are emitted contiguously, finest first, because the shader walks the array
    // set by set and indexes a set's members by cascade index off its first entry.
    self.scene.world
      .query_builder<const TransformComponent, const ProbeVolumeComponent>() //
      .build()
      .each(
        [&self,
         camera_position = cam.position](flecs::entity e, const TransformComponent&, const ProbeVolumeComponent& c) {
          if (!e.enabled()) {
            return;
          }

          const auto counts = glm::max(c.probe_counts, glm::uvec3(2));
          const auto base_spacing = glm::max(c.probe_range, glm::vec3(0.01f)) / glm::vec3(counts - 1u);
          const glm::vec3 anchor = self.scene.get_world_transform(e)[3];
          const auto center = c.follow_camera ? camera_position : anchor;
          const auto cascades = std::clamp(c.cascade_count, 1u, GPU::DDGI_MAX_CASCADE_COUNT);
          const auto probe_count = counts.x * counts.y * counts.z;

          auto set = ankerl::svector<GPU::ProbeVolume, GPU::DDGI_MAX_CASCADE_COUNT>{};
          for (u32 cascade = 0; cascade < cascades; cascade++) {
            const auto spacing = base_spacing * static_cast<f32>(1u << cascade);
            const auto scroll = glm::ivec3(glm::floor((center - anchor) / spacing + 0.5f));

            set.emplace_back(
              GPU::ProbeVolume{
                .origin = anchor + glm::vec3(scroll) * spacing,
                .spacing = spacing,
                .probe_count = probe_count,
                .spacing_rcp = 1.0f / spacing,
                .cascade_index = cascade,
                .counts = counts,
                .cascade_count = cascades,
                .scroll = scroll,
                .cascade_blend = std::clamp(c.cascade_blend, 0.0f, 1.0f),
                .center = center,
                .max_probe_distance = glm::length(spacing) * 1.5f,
              }
            );
          }

          const auto probe_offset = self.probe_volumes.empty()
                                      ? 0u
                                      : self.probe_volumes.back().probe_offset + self.probe_volumes.back().probe_count;

          if (probe_offset + probe_count * cascades > GPU::DDGI_MAX_PROBE_COUNT) {
            OX_LOG_WARN(
              "Dropped a {} cascade probe volume, the scene is over the {} probe limit.",
              cascades,
              GPU::DDGI_MAX_PROBE_COUNT
            );
            return;
          }

          for (u32 cascade = 0; cascade < cascades; cascade++) {
            set[cascade].probe_offset = probe_offset + cascade * probe_count;
            self.probe_volumes.emplace_back(set[cascade]);
            self.probe_volume_keys.emplace_back(ProbeVolumeKey{.entity = e.id(), .cascade_index = cascade});
          }
        }
      );

    if (draw_volume_bounds) {
      for (const auto& volume : self.probe_volumes) {
        const auto extents = glm::vec3(volume.counts - 1u) * volume.spacing * 0.5f;
        App::mod<ox::DebugRenderer>().draw_aabb(
          AABB(volume.origin - extents, volume.origin + extents),
          glm::vec4(0, 1, 1, 1)
        );
      }
    }
  }

  self.post_proces_settings.exposure = cvar.cvar_exposure.get();

  self.render_queue_2d.init();

  self.scene.world
    .query_builder<const TransformComponent, const SpriteComponent>() //
    .build()
    .each([&asset_man,
           &s = self.scene,
           &cam,
           &rq2d = self.render_queue_2d](flecs::entity e, const TransformComponent& tc, const SpriteComponent& comp) {
      const auto distance = glm::distance(glm::vec3(0.f, 0.f, cam.position.z), glm::vec3(0.f, 0.f, tc.position.z));
      if (auto material = asset_man.get_asset(comp.material)) {
        u16 flags = 0;
        if (comp.sort_y)
          flags |= GPU::RENDER_FLAGS_2D_SORT_Y;
        if (comp.flip_x)
          flags |= GPU::RENDER_FLAGS_2D_FLIP_X;

        if (auto transform_id = s.get_entity_transform_id(e)) {
          rq2d.add(
            flags,
            tc.position.y,
            SlotMap_decode_id(*transform_id).index,
            SlotMap_decode_id(material->material_id).index,
            distance
          );
        } else {
          OX_LOG_WARN("No registered transform for sprite entity: {}", e.name().c_str());
        }
      }
    });

  self.scene.world
    .query_builder<const AutoExposureComponent>() //
    .build()
    .each([&self](flecs::entity e, const AutoExposureComponent& c) {
      self.gpu_scene_flags |= GPU::SceneFlags::HasEyeAdaptation;
      self.eye_adaptation.max_exposure = c.max_exposure;
      self.eye_adaptation.min_exposure = c.min_exposure;
      self.eye_adaptation.adaptation_speed = c.adaptation_speed;
      self.eye_adaptation.ev100_bias = c.ev100_bias;
    });

  self.scene.world
    .query_builder<const TransformComponent, const VignetteComponent>() //
    .build()
    .each([&](flecs::entity e, const TransformComponent& tc, const VignetteComponent& c) {
      self.post_proces_settings.vignette_amount = c.amount;

      self.gpu_scene_flags |= GPU::SceneFlags::HasVignette;
    });

  self.scene.world
    .query_builder<const TransformComponent, const ChromaticAberrationComponent>() //
    .build()
    .each([&](flecs::entity e, const TransformComponent& tc, const ChromaticAberrationComponent& c) {
      self.post_proces_settings.chromatic_aberration_amount = c.amount;

      self.gpu_scene_flags |= GPU::SceneFlags::HasChromaticAberration;
    });

  self.scene.world
    .query_builder<const TransformComponent, const FilmGrainComponent>() //
    .build()
    .each([&](flecs::entity e, const TransformComponent& tc, const FilmGrainComponent& c) {
      self.post_proces_settings.film_grain_amount = c.amount;
      self.post_proces_settings.film_grain_scale = c.scale;
      self.post_proces_settings.film_grain_seed = render_context.num_frames % 16;

      self.gpu_scene_flags |= GPU::SceneFlags::HasFilmGrain;
    });

  self.scene.world
    .query_builder<const TransformComponent, const LetterboxComponent>() //
    .build()
    .each([&](flecs::entity e, const TransformComponent& tc, const LetterboxComponent& c) {
      self.post_proces_settings.letterbox_amount = c.amount;
      self.post_proces_settings.letterbox_aspect = c.target_aspect;
      self.post_proces_settings.letterbox_color = c.color;

      self.gpu_scene_flags |= GPU::SceneFlags::HasLetterbox;
    });

  self.scene.world
    .query_builder<const TransformComponent, const ScreenFadeComponent>() //
    .build()
    .each([&](flecs::entity e, const TransformComponent& tc, const ScreenFadeComponent& c) {
      self.post_proces_settings.fade_amount = c.amount;
      self.post_proces_settings.fade_color = c.color;

      self.gpu_scene_flags |= GPU::SceneFlags::HasScreenFade;
    });

  self.scene.world
    .query_builder<const TonemappingComponent>() //
    .build()
    .each([&](flecs::entity e, const TonemappingComponent& tc) {
      self.tonemap_type = tc.tonemap_type;
      //
    });

  auto zero_fill_pass = vuk::make_pass("zero fill", [](vuk::CommandBuffer& command_buffer, VUK_BA(vuk::eClear) dst) {
    command_buffer.fill_buffer(dst, 0_u32);
    return dst;
  });

  update_projected_transform_buffer<GPU::TransformWorld>(
    render_context,
    info.gpu_transforms,
    info.dirty_transform_ids,
    self.transforms_world_buffer,
    self.prepared_frame.transforms_world_buffer,
    [](const GPU::Transforms& t) { return GPU::TransformWorld{.world = t.world}; },
    "transforms_world",
    "update transform world"
  );
  update_projected_transform_buffer<GPU::TransformPrevious>(
    render_context,
    info.gpu_transforms,
    info.dirty_transform_ids,
    self.transforms_previous_buffer,
    self.prepared_frame.transforms_previous_buffer,
    [](const GPU::Transforms& t) { return GPU::TransformPrevious{.previous_world = t.previous_world}; },
    "transforms_previous",
    "update transform previous"
  );
  // Materials are global and already synced by the renderer; this instance only reads them.
  self.prepared_frame.materials_buffer = self.renderer.get_materials_buffer();

  {
    const auto lights_span = self.scene.lights.slots_unsafe();
    for (const auto& light : lights_span) {
      self.prepared_frame.spot_light_count += static_cast<u32>(light.kind == GPU::LightKind::Spot);
      self.prepared_frame.point_light_count += static_cast<u32>(light.kind == GPU::LightKind::Point);
    }
    const auto count = std::min<std::size_t>(lights_span.size(), GPU::MAX_LIGHTS);
    const auto size_bytes = count * sizeof(GPU::Light);
    if (count > 0) {
      auto src_buffer = render_context.alloc_transient_buffer(vuk::MemoryUsage::eCPUtoGPU, size_bytes);
      std::memcpy(src_buffer->mapped_ptr, lights_span.data(), size_bytes);
      auto dst_buffer = vuk::acquire_buf("lights", self.lights_buffer->subrange(0, size_bytes), vuk::eMemoryRead);
      self.prepared_frame.lights_buffer = vuk::copy(std::move(src_buffer), std::move(dst_buffer));
    } else {
      self.prepared_frame.lights_buffer = vuk::acquire_buf("lights", *self.lights_buffer, vuk::eMemoryRead);
    }
  }

  self.prepared_frame.atmosphere_buffer = self.renderer.render_context->scratch_buffer(self.atmosphere);

  // the arena has to exist before the instance array is patched with pointers into it, and the
  // instance array has to be uploaded after that patch
  self.prepared_frame.skinned_vertex_total = info.skinned_vertex_total;
  if (info.skinned_vertex_total > 0) {
    const auto positions_bytes = static_cast<usize>(info.skinned_vertex_total) * sizeof(glm::u16vec4);
    const auto normals_bytes = static_cast<usize>(info.skinned_vertex_total) * sizeof(u32);

    self.skinned_vertices_buffer = render_context.resize_buffer(
      std::move(self.skinned_vertices_buffer),
      vuk::MemoryUsage::eGPUonly,
      positions_bytes + normals_bytes
    );

    const auto arena_address = self.skinned_vertices_buffer->device_address;
    for (const auto& skinned : info.skinned_mesh_instances) {
      if (skinned.gpu_instance_index >= info.gpu_mesh_instances.size()) {
        continue;
      }

      // no skeleton yet means nothing wrote this instance's slice of the arena, so leaving the
      // pointers null renders it in bind pose instead of skinning against uninitialized transforms
      auto& gpu_mesh_instance = info.gpu_mesh_instances[skinned.gpu_instance_index];
      if (skinned.bone_count == 0) {
        gpu_mesh_instance.skinned_vertex_positions = 0;
        gpu_mesh_instance.skinned_vertex_normals = 0;
        continue;
      }

      gpu_mesh_instance.skinned_vertex_positions = arena_address + skinned.vertex_offset * sizeof(glm::u16vec4);
      gpu_mesh_instance.skinned_vertex_normals = arena_address + positions_bytes + skinned.vertex_offset * sizeof(u32);
    }

    self.skinning_transforms_buffer = render_context.resize_buffer(
      std::move(self.skinning_transforms_buffer),
      vuk::MemoryUsage::eGPUonly,
      ox::max(info.skinning_transforms.size_bytes(), sizeof(GPU::SkinningTransform))
    );
    self.prepared_frame.skinning_transforms_buffer =
      info.skinning_transforms.empty()
        ? vuk::acquire_buf("skinning transforms", *self.skinning_transforms_buffer, vuk::eNone)
        : render_context.upload_staging(info.skinning_transforms, *self.skinning_transforms_buffer);

    self.prepared_frame.skin_jobs.clear();
    self.prepared_frame.skin_jobs.reserve(info.skinned_mesh_instances.size());
    for (const auto& skinned : info.skinned_mesh_instances) {
      if (skinned.vertex_count == 0 || skinned.bone_count == 0) {
        continue;
      }

      self.prepared_frame.skin_jobs.emplace_back(
        GPU::SkinJob{
          .mesh_instance_index = skinned.gpu_instance_index,
          .vertex_offset = skinned.vertex_offset,
          .bone_offset = skinned.bone_offset,
          .vertex_count = skinned.vertex_count,
        }
      );
    }

    self.prepared_frame
      .skinned_vertices_buffer = vuk::acquire_buf("skinned vertices", *self.skinned_vertices_buffer, vuk::eNone);

    // after the arena is final, because an entry's structure is built straight out of its slice.
    // Same answer `render` builds against, so the pool holds no memory while nothing traces
    if (self.tlas_has_consumer(cvar)) {
      self.skinned_blas_pool.sync(render_context, info.skinned_mesh_instances, arena_address);
    } else {
      self.skinned_blas_pool.reset(render_context);
    }
  } else {
    self.skinned_blas_pool.reset(render_context);
  }

  if (info.meshes_dirty && !info.gpu_meshes.empty()) {
    self.meshes_buffer = render_context.resize_buffer(
      std::move(self.meshes_buffer),
      vuk::MemoryUsage::eGPUonly,
      info.gpu_meshes.size_bytes()
    );
    self.prepared_frame.meshes_buffer = render_context.upload_staging(info.gpu_meshes, *self.meshes_buffer);
  } else if (self.meshes_buffer) {
    self.prepared_frame.meshes_buffer = vuk::acquire_buf("meshes", *self.meshes_buffer, vuk::Access::eMemoryRead);
  }

  // uploaded every frame rather than under `meshes_dirty`, matching how the scene refreshes it: the
  // TLAS build dereferences these as raw pointers, and it is 8 bytes per unique mesh
  if (!info.gpu_mesh_blas_addresses.empty()) {
    self.blas_addresses_buffer = render_context.resize_buffer(
      std::move(self.blas_addresses_buffer),
      vuk::MemoryUsage::eGPUonly,
      info.gpu_mesh_blas_addresses.size_bytes()
    );
    self.prepared_frame.blas_addresses_buffer = render_context.upload_staging(
      info.gpu_mesh_blas_addresses,
      *self.blas_addresses_buffer
    );
  } else if (self.blas_addresses_buffer) {
    self.prepared_frame.blas_addresses_buffer = vuk::acquire_buf(
      "blas addresses",
      *self.blas_addresses_buffer,
      vuk::Access::eMemoryRead
    );
  }

  // an entry the pool has not actually built yet reads back as zero, which drops that instance out
  // of the TLAS rather than pointing the build at memory nothing has written. Since this runs before
  // the frame records its builds, a freshly pooled structure publishes its address next frame
  self.prepared_frame.skinned_instance_count = static_cast<u32>(info.skinned_mesh_instances.size());
  self.skinned_blas_addresses.assign(ox::max(info.skinned_mesh_instances.size(), 1_sz), 0_u64);
  for (usize i = 0; i < info.skinned_mesh_instances.size(); i++) {
    self.skinned_blas_addresses[i] = self.skinned_blas_pool.address_of(
      info.skinned_mesh_instances[i].mesh_instance_slot
    );
  }
  self.skinned_blas_addresses_buffer = render_context.resize_buffer(
    std::move(self.skinned_blas_addresses_buffer),
    vuk::MemoryUsage::eGPUonly,
    ox::size_bytes(self.skinned_blas_addresses)
  );
  self.prepared_frame.skinned_blas_addresses_buffer = render_context.upload_staging(
    std::span(self.skinned_blas_addresses),
    *self.skinned_blas_addresses_buffer
  );

  if (info.mesh_instances_dirty && !info.gpu_mesh_instances.empty()) {
    self.mesh_instances_buffer = render_context.resize_buffer(
      std::move(self.mesh_instances_buffer),
      vuk::MemoryUsage::eGPUonly,
      info.gpu_mesh_instances.size_bytes()
    );
    self.prepared_frame.mesh_instances_buffer = render_context.upload_staging(
      info.gpu_mesh_instances,
      *self.mesh_instances_buffer
    );

    auto meshlet_instance_visibility_mask_size_bytes = (info.max_meshlet_instance_count + 31) / 32 * sizeof(u32);

    self.meshlet_instance_visibility_mask_buffer = render_context.resize_buffer(
      std::move(self.meshlet_instance_visibility_mask_buffer),
      vuk::MemoryUsage::eGPUonly,
      meshlet_instance_visibility_mask_size_bytes
    );
    auto meshlet_instance_visibility_mask_buffer = vuk::acquire_buf(
      "meshlet instances visibility mask",
      *self.meshlet_instance_visibility_mask_buffer,
      vuk::eNone
    );
    self.prepared_frame.meshlet_instance_visibility_mask_buffer = zero_fill_pass(
      std::move(meshlet_instance_visibility_mask_buffer)
    );
  } else if (self.mesh_instances_buffer) {
    self.prepared_frame.mesh_instances_buffer = vuk::acquire_buf(
      "mesh instances",
      *self.mesh_instances_buffer,
      vuk::Access::eMemoryRead
    );
    self.prepared_frame.meshlet_instance_visibility_mask_buffer = vuk::acquire_buf(
      "meshlet instances visibility mask",
      *self.meshlet_instance_visibility_mask_buffer,
      vuk::eMemoryRead
    );
  }

  if (self.scene.terrain != nullptr && self.scene.terrain->is_baked()) {
    const auto patch_count = self.scene.terrain->patch_count.x * self.scene.terrain->patch_count.y;
    const auto mask_size_bytes = (patch_count + 31) / 32 * sizeof(u32);

    const auto needs_reset = self.terrain_patch_visibility_patch_count != patch_count;
    self.terrain_patch_visibility_patch_count = patch_count;

    self.terrain_patch_visibility_mask_buffer = render_context.resize_buffer(
      std::move(self.terrain_patch_visibility_mask_buffer),
      vuk::MemoryUsage::eGPUonly,
      mask_size_bytes
    );
    auto mask_buffer = vuk::acquire_buf(
      "terrain patch visibility mask",
      *self.terrain_patch_visibility_mask_buffer,
      needs_reset ? vuk::eNone : vuk::eMemoryRead
    );
    self.prepared_frame.terrain_patch_visibility_mask_buffer = needs_reset ? zero_fill_pass(std::move(mask_buffer))
                                                                           : std::move(mask_buffer);
  }

  self.prepared_frame.mesh_instance_count = info.mesh_instance_count;
  self.prepared_frame.max_meshlet_instance_count = info.max_meshlet_instance_count;
  self.prepared_frame.use_mesh_shaders = render_context.use_mesh_shaders();

  if (!info.dirty_mesh_instance_indices.empty()) {
    self.prepared_frame.dirty_mesh_instance_count = static_cast<u32>(info.dirty_mesh_instance_indices.size());
    auto dirty_mesh_instances_buffer = render_context.alloc_transient_buffer(
      vuk::MemoryUsage::eCPUtoGPU,
      info.dirty_mesh_instance_indices.size_bytes()
    );
    std::memcpy(
      dirty_mesh_instances_buffer->mapped_ptr,
      info.dirty_mesh_instance_indices.data(),
      info.dirty_mesh_instance_indices.size_bytes()
    );
    self.prepared_frame.dirty_mesh_instances_buffer = std::move(dirty_mesh_instances_buffer);
  }
  if (info.max_meshlet_instance_count > 0) {
    self.prepared_frame.meshlet_instances_buffer = render_context.alloc_transient_buffer(
      vuk::MemoryUsage::eGPUonly,
      self.prepared_frame.max_meshlet_instance_count * sizeof(GPU::MeshletInstance)
    );
    if (!self.prepared_frame.use_mesh_shaders) {
      self.prepared_frame.visible_meshlet_instances_indices_buffer = render_context.alloc_transient_buffer(
        vuk::MemoryUsage::eGPUonly,
        self.prepared_frame.max_meshlet_instance_count * sizeof(u32)
      );
      self.prepared_frame.reordered_indices_buffer = render_context.alloc_transient_buffer(
        vuk::MemoryUsage::eGPUonly,
        self.prepared_frame.max_meshlet_instance_count * GPU::Mesh::MAX_MESHLET_PRIMITIVES * 3 * sizeof(u32)
      );
    }
  }

  auto debug_renderer_enabled = (bool)cvar.cvar_enable_debug_renderer.get();

  if (debug_renderer_enabled) {
    auto& debug_renderer = App::mod<ox::DebugRenderer>();

    const auto& lines = debug_renderer.get_lines(false);
    auto [line_vertices, line_index_count] = debug_renderer.get_vertices_from_lines(lines);

    const auto& triangles = debug_renderer.get_triangles(false);
    auto [triangle_vertices, triangle_index_count] = debug_renderer.get_vertices_from_triangles(triangles);

    const u32 index_count = line_index_count + triangle_index_count;
    OX_CHECK_LT(index_count, DebugRenderer::MAX_LINE_INDICES, "Increase DebugRenderer::MAX_LINE_INDICES");

    self.prepared_frame.line_index_count = line_index_count;
    self.prepared_frame.triangle_index_count = triangle_index_count;

    std::vector<DebugRenderer::Vertex> vertices = line_vertices;
    vertices.insert(vertices.end(), triangle_vertices.begin(), triangle_vertices.end());
    std::span<DebugRenderer::Vertex> vertices_span = line_vertices;

    if (!vertices.empty()) {
      self.debug_renderer_verticies_buffer = render_context.resize_buffer(
        std::move(self.debug_renderer_verticies_buffer),
        vuk::MemoryUsage::eGPUonly,
        vertices_span.size_bytes()
      );
      self.prepared_frame.debug_renderer_verticies_buffer = render_context.upload_staging(
        vertices_span,
        *self.debug_renderer_verticies_buffer
      );
    } else if (self.debug_renderer_verticies_buffer) {
      self.prepared_frame.debug_renderer_verticies_buffer = vuk::acquire_buf(
        "debug_renderer_verticies_buffer",
        *self.debug_renderer_verticies_buffer,
        vuk::Access::eMemoryRead
      );
    }

    debug_renderer.reset();
  }

  self.update_vbgtao_info(cvar);

  if (cvar.cvar_particles_enable.as_bool()) {
    const auto particle_delta_time = static_cast<f32>(App::get_timestep().get_millis()) * 0.001f;
    self.prepare_particles(particle_delta_time, cvar.cvar_particle_sort.as_bool());
  }

  if (!self.exposure_buffer) {
    self.exposure_buffer = render_context.allocate_buffer_super(
      vuk::MemoryUsage::eGPUonly,
      sizeof(GPU::HistogramLuminance)
    );
    self.prepared_frame.exposure_buffer = vuk::acquire_buf("exposure buffer", *self.exposure_buffer, vuk::eMemoryRead);
    vuk::fill(self.prepared_frame.exposure_buffer, 1.0f);
  } else {
    self.prepared_frame.exposure_buffer = vuk::acquire_buf("exposure buffer", *self.exposure_buffer, vuk::eMemoryRead);
  }
}
} // namespace ox
