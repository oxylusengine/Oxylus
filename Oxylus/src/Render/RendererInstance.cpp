#include "Render/RendererInstance.hpp"

#include <algorithm>
#include <ankerl/svector.h>
#include <bit>
#include <cstdlib>
#include <ranges>
#include <vir/RenderGraph.hpp>
#include <vir/Types.hpp>

#include "Asset/AssetManager.hpp"
#include "Asset/Texture.hpp"
#include "Core/App.hpp"
#include "Core/Base.hpp"
#include "Core/Enum.hpp"
#include "Memory/Stack.hpp"
#include "Render/DebugRenderer.hpp"
#include "Render/RenderContext.hpp"
#include "Render/Utils/PushConstants.hpp"
#include "Render/Utils/Samplers.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneGPU.hpp"
#include "Utils/Log.hpp"
#include "Utils/Timer.hpp"

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

static auto to_lut_size(const vir::Extent3D& extent) -> glm::ivec3 {
  return {static_cast<i32>(extent.width), static_cast<i32>(extent.height), static_cast<i32>(extent.depth)};
}

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

auto bind_vsm_pointspot_spec_constants(vir::Module& module) -> vir::Module& {
  return module.specialize_constant(50, RMVSMContext::POINT_SPOT_IMAGE_RESOLUTION)
    .specialize_constant(51, RMVSMContext::POINT_SPOT_PAGE_TABLE_SIZE)
    .specialize_constant(52, RMVSMContext::POINT_SPOT_MIP_COUNT)
    .specialize_constant(53, RMVSMContext::PAGE_SIZE)
    .specialize_constant(54, RMVSMContext::PHYSICAL_PAGE_TABLE_SIZE);
}

// between frames, scene buffers are only read, state the GPU carries over is read and written, and
// structures are traversed. A frame imports each at its rest and exports it back there
constexpr static auto SCENE_BUFFER_REST = vir::Access::MemoryRead;
constexpr static auto GPU_STATE_REST = vir::Access::MemoryRW;
constexpr static auto STRUCTURE_REST = vir::Access::AccelerationStructureBuildRW | vir::Access::RayTracingRead |
                                       vir::Access::ComputeAccelerationStructureRead;
constexpr static auto TEXTURE_REST = vir::Access::FragmentSampled;
constexpr static auto TERRAIN_HEIGHT_REST = vir::Access::ComputeSampled;
constexpr static auto TERRAIN_EDIT_REST = vir::Access::ComputeRW;
constexpr static auto TRANSIENT_BUFFER_USAGE = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
// programs kept per instance, enough to flip between a few shapes without recompiling
constexpr static usize MAX_FRAME_PROGRAMS = 4;
constexpr static usize EVICTED_SHAPE_HISTORY = 8;

// true when the buffer was (re)allocated, which leaves it undefined
static auto ensure_buffer(RenderContext& render_context, BufferID& id, u64 size, std::string_view name) -> bool {
  const auto resized = render_context.resize_buffer(id, std::max<u64>(size, 16), name);
  if (!resized) {
    OX_LOG_ERROR("failed to allocate {}", name);
    return false;
  }

  return *resized;
}

static auto reset_frame_data(FrameData& data) -> void {
  auto transforms_world = std::move(data.transforms_world);
  auto transforms_previous = std::move(data.transforms_previous);
  auto transform_runs = std::move(data.transform_runs);
  auto meshes = std::move(data.meshes);
  auto mesh_instances = std::move(data.mesh_instances);
  auto blas_addresses = std::move(data.blas_addresses);
  auto skinned_blas_addresses = std::move(data.skinned_blas_addresses);
  auto skinning_transforms = std::move(data.skinning_transforms);
  auto lights = std::move(data.lights);
  auto dirty_mesh_instances = std::move(data.dirty_mesh_instances);
  auto removed_mesh_bounds = std::move(data.removed_mesh_bounds);
  auto skin_jobs = std::move(data.skin_jobs);
  auto particle_instructions = std::move(data.particle_instructions);
  auto particle_constants = std::move(data.particle_constants);

  data = {};

  const auto restore = [](auto& dst, auto& src) -> void {
    src.clear();
    dst = std::move(src);
  };
  restore(data.transforms_world, transforms_world);
  restore(data.transforms_previous, transforms_previous);
  restore(data.transform_runs, transform_runs);
  restore(data.meshes, meshes);
  restore(data.mesh_instances, mesh_instances);
  restore(data.blas_addresses, blas_addresses);
  restore(data.skinned_blas_addresses, skinned_blas_addresses);
  restore(data.skinning_transforms, skinning_transforms);
  restore(data.lights, lights);
  restore(data.dirty_mesh_instances, dirty_mesh_instances);
  restore(data.removed_mesh_bounds, removed_mesh_bounds);
  restore(data.skin_jobs, skin_jobs);
  restore(data.particle_instructions, particle_instructions);
  restore(data.particle_constants, particle_constants);
}

template <typename T, typename Projection>
static auto project_transforms(
  std::span<const GPU::Transforms> transforms,
  std::span<const u32> indices,
  bool is_full,
  std::vector<T>& dst,
  Projection&& projection
) -> void {
  if (is_full) {
    dst.reserve(transforms.size());
    for (const auto& transform : transforms) {
      dst.push_back(projection(transform));
    }

    return;
  }

  dst.reserve(indices.size());
  for (const auto index : indices) {
    dst.push_back(projection(transforms[index]));
  }
}

static auto prepare_transforms(
  RenderContext& render_context,
  std::span<const GPU::Transforms> transforms,
  std::span<const GPU::TransformID> dirty_ids,
  BufferID& world_buffer,
  BufferID& previous_buffer,
  FrameData& data
) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  constexpr auto full_rebuild_dirty_threshold = 0.4;
  const auto element_count = transforms.size();
  const auto is_world_reallocated = ensure_buffer(
    render_context,
    world_buffer,
    element_count * sizeof(GPU::TransformWorld),
    "transforms world"
  );
  const auto is_previous_reallocated = ensure_buffer(
    render_context,
    previous_buffer,
    element_count * sizeof(GPU::TransformPrevious),
    "transforms previous"
  );
  const auto is_reallocated = is_world_reallocated || is_previous_reallocated;
  if (element_count == 0 || (dirty_ids.empty() && !is_reallocated)) {
    return;
  }

  auto unique_indices = stack.alloc<u32>(dirty_ids.size());
  for (const auto& [unique_index, dirty_id] : std::views::zip(unique_indices, dirty_ids)) {
    unique_index = SlotMap_decode_id(dirty_id).index;
  }
  std::sort(unique_indices.begin(), unique_indices.end());
  const auto unique_end = std::unique(unique_indices.begin(), unique_indices.end());
  unique_indices = unique_indices.first(static_cast<usize>(unique_end - unique_indices.begin()));

  const auto is_full = is_reallocated || static_cast<f64>(unique_indices.size()) >=
                                           static_cast<f64>(element_count) * full_rebuild_dirty_threshold;
  project_transforms(transforms, unique_indices, is_full, data.transforms_world, [](const GPU::Transforms& t) {
    return GPU::TransformWorld{.world = t.world};
  });
  project_transforms(transforms, unique_indices, is_full, data.transforms_previous, [](const GPU::Transforms& t) {
    return GPU::TransformPrevious{.previous_world = t.previous_world};
  });

  if (is_full) {
    data.transform_runs.push_back({.src = 0, .dst = 0, .count = static_cast<u32>(element_count)});
    return;
  }

  const auto dirty_count = unique_indices.size();
  for (auto i = 0_sz; i < dirty_count;) {
    const auto start_index = unique_indices[i];
    auto run_length = 1_sz;
    while (i + run_length < dirty_count && unique_indices[i + run_length] == start_index + run_length) {
      ++run_length;
    }

    data.transform_runs.push_back(
      {.src = static_cast<u32>(i), .dst = start_index, .count = static_cast<u32>(run_length)}
    );
    i += run_length;
  }
}

struct RecordedFrame {
  vir::Program program = {};
  PreparedFrame prepared = {};
};

// the bytes of a contiguous container, or of one plain value
template <typename T>
static auto host_bytes(const T& value) -> std::span<const u8> {
  if constexpr (requires {
                  value.data();
                  value.size();
                }) {
    return {reinterpret_cast<const u8*>(value.data()), value.size() * sizeof(*value.data())};
  } else {
    static_assert(std::is_trivially_copyable_v<T>);
    return {reinterpret_cast<const u8*>(&value), sizeof(T)};
  }
}

// `update` packs the dirty transforms into runs, which change length every frame, so the copies are
// recorded when the program runs
template <typename T>
static auto record_transform_upload(
  vir::Module& module, const FrameData& data, std::string_view name, vir::ValueID dst, vir::ValueID& staging
) -> vir::ValueID {
  staging = module.declare_buffer_var(name, vir::Access::HostWrite);
  const auto body = module.declare_callback_var(name, [&data, src = staging, dst](vir::Recorder& recorder) -> void {
    if (data.transform_runs.empty()) {
      return;
    }

    const auto target = recorder.buffer(dst);
    const auto source = recorder.buffer(src);
    for (const auto& run : data.transform_runs) {
      recorder.copy_buffer(
        source,
        target,
        {
          .src_offset = static_cast<u64>(run.src) * sizeof(T),
          .dst_offset = static_cast<u64>(run.dst) * sizeof(T),
          .size = static_cast<u64>(run.count) * sizeof(T),
        }
      );
    }
  });
  return module.begin_compute({{staging, vir::Access::CopyRead}, {dst, vir::Access::CopyWrite}})
    .with_name(name)
    .record_from(body)
    .end_compute()[1];
}

auto RendererInstance::record_upload(
  vir::Module& module, std::string_view name, vir::ValueID dst, UploadVariables& upload
) -> vir::ValueID {
  upload.staging = module.declare_buffer_var(name, vir::Access::HostWrite);
  upload.is_uploading = module.declare_bool_var(name, false);
  return module.set_condition(
    upload.is_uploading,
    [&](vir::Module& arm) { return arm.copy_buffer(upload.staging, dst); },
    [&](vir::Module&) { return dst; }
  );
}

auto RendererInstance::record_fill(
  vir::Module& module, std::string_view name, vir::ValueID dst, u32 value, vir::ValueID& is_filling
) -> vir::ValueID {
  is_filling = module.declare_bool_var(name, false);
  return module.set_condition(
    is_filling,
    [&](vir::Module& arm) { return arm.fill_buffer(dst, value); },
    [&](vir::Module&) { return dst; }
  );
}

auto RendererInstance::bind_upload(
  this const RendererInstance& self,
  vir::Program& program,
  const UploadVariables& upload,
  std::span<const u8> data,
  std::string_view name
) -> void {
  if (program.has_variable(upload.staging)) {
    auto staging = self.renderer.render_context->upload_frame_buffer(data, name);
    if (!staging) {
      OX_LOG_ERROR("failed to stage {}", name);
    }
    program.set(upload.staging, staging.value_or(vir::Buffer{}));
  }

  program.set(upload.is_uploading, !data.empty());
}

auto FrameShapeHash::operator()(const FrameShape& shape) const noexcept -> u64 {
  auto hash = 0_u64;
  const auto mix = [&hash](u64 value) {
    hash = ankerl::unordered_dense::detail::wyhash::hash(hash ^ (value + 0x9e3779b97f4a7c15_u64 + (hash << 6)));
  };

  mix(shape.display_size.x);
  mix(shape.display_size.y);
  mix(shape.render_size.x);
  mix(shape.render_size.y);
  mix(std::to_underlying(shape.upscaler));
  mix(std::to_underlying(shape.debug_view));
  mix(std::to_underlying(shape.target_format));
  mix(std::to_underlying(shape.scene_flags));
  mix(std::to_underlying(shape.tonemap));
  mix(reinterpret_cast<u64>(shape.tlas));
  const auto flags = std::array{
    shape.is_debugging,
    shape.has_meshes,
    shape.has_terrain,
    shape.has_sprites,
    shape.has_particles,
    shape.has_atmosphere,
    shape.has_directional_light,
    shape.has_directional_shadows,
    shape.has_pointspot_shadows,
    shape.has_bloom,
    shape.has_fxaa,
    shape.has_gtao,
    shape.has_rtao,
    shape.has_contact_shadows,
    shape.has_eye_adaptation,
    shape.has_ddgi,
    shape.has_tlas,
    shape.has_debug_renderer,
    shape.use_mesh_shaders,
    shape.is_transparent,
    shape.is_particle_sort,
  };
  auto bits = 0_u64;
  for (usize i = 0; i < flags.size(); i++) {
    bits |= static_cast<u64>(flags[i]) << i;
  }

  mix(bits);
  mix(shape.particle_sorted_count);
  mix(shape.probe_volume_count);
  mix(shape.ddgi_probe_count);
  mix(shape.ddgi_rays_per_probe);
  for (const auto& stage : shape.stages) {
    mix(ankerl::unordered_dense::hash<std::string>{}(stage));
  }

  return hash;
}

RendererInstance::RendererInstance(Scene& owner_scene, Renderer& parent_renderer)
    : scene(owner_scene),
      renderer(parent_renderer) {
  auto& render_context = App::get_rendercontext();
  render_queue_2d.init();

  constexpr usize stage_count = static_cast<usize>(RenderStage::Count);
  before_callbacks.resize(stage_count);
  after_callbacks.resize(stage_count);

  sky_transmittance_lut = Texture::create({
    .format = vir::Format::RGBA16Sfloat,
    .extent = {.width = 256, .height = 64, .depth = 1},
    .usage = vir::ImageUsageFlags::Sampled | vir::ImageUsageFlags::Storage,
  });
  OX_ASSERT(sky_transmittance_lut);

  sky_multiscatter_lut = Texture::create({
    .format = vir::Format::RGBA16Sfloat,
    .extent = {.width = 32, .height = 32, .depth = 1},
    .usage = vir::ImageUsageFlags::Sampled | vir::ImageUsageFlags::Storage,
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
    .format = vir::Format::R16Uint,
    .extent = {.width = HILBERT_NOISE_LUT_WIDTH, .height = HILBERT_NOISE_LUT_WIDTH, .depth = 1},
    .usage = vir::ImageUsageFlags::Sampled,
  });
  OX_ASSERT(hilbert_noise_lut);
  hilbert_noise_lut.upload({reinterpret_cast<u8*>(hilbert_noise), sizeof(hilbert_noise)}, TEXTURE_REST);

  sky_cubemap = Texture::create({
    .format = vir::Format::RGBA16Sfloat,
    .extent = {.width = 32, .height = 32, .depth = 1},
    .layer_count = 6,
    .image_flags = vir::ImageCreateFlags::CubeCompatible,
    .usage = vir::ImageUsageFlags::Sampled | vir::ImageUsageFlags::Storage,
    .view_type = vir::ImageViewType::Cube,
  });
  OX_ASSERT(sky_cubemap);

  vsm_virtual_page_table = Texture::create({
    .format = vir::Format::R32Uint,
    .extent =
      {
        .width = RMVSMContext::DIRECTIONAL_PAGE_TABLE_SIZE,
        .height = RMVSMContext::DIRECTIONAL_PAGE_TABLE_SIZE,
        .depth = 1,
      },
    .layer_count = RMVSMContext::MAX_DIRECTIONAL_CLIPMAP_COUNT,
    .level_count = 1,
    .usage = vir::ImageUsageFlags::Storage | vir::ImageUsageFlags::Sampled,
    .view_type = vir::ImageViewType::e2DArray,
  });

  vsm_pointspot_virtual_page_table = Texture::create({
    .format = vir::Format::R32Uint,
    .extent =
      {
        .width = RMVSMContext::POINT_SPOT_PAGE_TABLE_SIZE,
        .height = RMVSMContext::POINT_SPOT_PAGE_TABLE_SIZE,
        .depth = 1,
      },
    .layer_count = RMVSMContext::POINT_SPOT_LAYER_COUNT,
    .level_count = RMVSMContext::POINT_SPOT_MIP_COUNT,
    .usage = vir::ImageUsageFlags::Storage | vir::ImageUsageFlags::Sampled,
    .view_type = vir::ImageViewType::e2DArray,
  });

  // the shadow draws write it as uint and lighting samples it as float, through views made per binding
  vsm_physical_page_table = Texture::create({
    .format = vir::Format::R32Sfloat,
    .extent =
      {.width = RMVSMContext::PHYSICAL_PAGE_TABLE_SIZE, .height = RMVSMContext::PHYSICAL_PAGE_TABLE_SIZE, .depth = 1},
    .image_flags = vir::ImageCreateFlags::MutableFormat,
    .usage = vir::ImageUsageFlags::Storage | vir::ImageUsageFlags::Sampled | vir::ImageUsageFlags::TransferDst,
  });

  {
    // everything above starts out cleared and sampled, which is where the frame first reads it
    const auto black = vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 1.0f);
    const auto black_u32 = vir::ClearValue::rgba_u32(0, 0, 0, 1);
    auto module = vir::Module{};
    for (const auto* texture :
         {&sky_cubemap, &sky_transmittance_lut, &sky_multiscatter_lut, &vsm_physical_page_table}) {
      const auto cleared = module.clear(texture->import_discarded(module, TEXTURE_REST), black);
      module.export_to(cleared, TEXTURE_REST);
    }

    for (const auto* texture : {&vsm_virtual_page_table, &vsm_pointspot_virtual_page_table}) {
      const auto cleared = module.clear(texture->import_discarded(module, TEXTURE_REST), black_u32);
      module.export_to(cleared, TEXTURE_REST);
    }

    if (const auto cleared = render_context.execute_blocking(std::move(module)); !cleared) {
      OX_LOG_FATAL("failed to clear the renderer's persistent textures ({})", std::to_underlying(cleared.error()));
    }
  }

  rebuild_execution_order();
}

RendererInstance::~RendererInstance() {
  auto& render_context = *renderer.render_context;
  skinned_blas_pool.reset(render_context);
  scene_tlas.destroy(render_context);

  for (auto* buffer : {
         &transforms_world_buffer,
         &transforms_previous_buffer,
         &mesh_instances_buffer,
         &skinning_transforms_buffer,
         &skinned_vertices_buffer,
         &meshes_buffer,
         &blas_addresses_buffer,
         &skinned_blas_addresses_buffer,
         &lights_buffer,
         &meshlet_instance_visibility_mask_buffer,
         &terrain_patch_visibility_mask_buffer,
         &exposure_buffer,
         &placeholder_buffer,
         &particle_buffer,
         &particle_alive_list_a,
         &particle_alive_list_b,
         &particle_dead_list,
         &particle_dead_counts,
         &particle_counters,
         &particle_sort_keys,
         &ddgi_probe_states,
         &ddgi_probe_update_list,
       }) {
    if (*buffer != BufferID::Invalid) {
      render_context.destroy_buffer(*buffer);
      *buffer = BufferID::Invalid;
    }
  }
}

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
  this RendererInstance& self,
  RenderStage stage,
  vir::Module& module,
  SharedResources& resources,
  PreparedFrame& prepared
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
      auto ctx = RenderStageContext{
        .renderer_instance = self,
        .module = module,
        .shared_resources = self.shared_resources,
        .current_stage = stage,
        .render_context = *self.renderer.render_context,
        .viewport_size = self.viewport_size_,
        .variables = prepared.stage_variables[callback.name],
        .buffer_resources = resources.buffer_resources,
        .image_resources = resources.image_resources,
      };
      callback.callback(ctx);
    }
  }
}

auto RendererInstance::execute_stages_after(
  this RendererInstance& self,
  RenderStage stage,
  vir::Module& module,
  SharedResources& resources,
  PreparedFrame& prepared
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
      auto ctx = RenderStageContext{
        .renderer_instance = self,
        .module = module,
        .shared_resources = self.shared_resources,
        .current_stage = stage,
        .render_context = *self.renderer.render_context,
        .viewport_size = self.viewport_size_,
        .variables = prepared.stage_variables[callback.name],
        .buffer_resources = resources.buffer_resources,
        .image_resources = resources.image_resources,
      };
      callback.callback(ctx);
    }
  }
}

auto RendererInstance::add_stage_before(
  this RendererInstance& self,
  RenderStage stage,
  const std::string& name,
  std::function<void(RenderStageContext&)> callback,
  std::function<void(vir::Program&, const StageVariables&)> bind,
  int order
) -> void {
  StageDependency dep{.target_stage = stage, .position = StagePosition::Before, .order = order};
  self.add_stage_callback(
    RenderStageCallback{.callback = std::move(callback), .bind = std::move(bind), .dependency = dep, .name = name}
  );
}

auto RendererInstance::add_stage_after(
  this RendererInstance& self,
  RenderStage stage,
  const std::string& name,
  std::function<void(RenderStageContext&)> callback,
  std::function<void(vir::Program&, const StageVariables&)> bind,
  int order
) -> void {
  StageDependency dep{.target_stage = stage, .position = StagePosition::After, .order = order};
  self.add_stage_callback(
    RenderStageCallback{.callback = std::move(callback), .bind = std::move(bind), .dependency = dep, .name = name}
  );
}

auto RendererInstance::render(
  this RendererInstance& self,
  const vir::ImageAttachment& target,
  glm::ivec2 viewport_origin,
  glm::ivec2 viewport_size,
  glm::ivec2 surface_size,
  const RendererCVar& cvar
) -> std::expected<vir::SubmitTicket, VkResult> {
  ZoneScoped;

  auto& render_context = *self.renderer.render_context;
  self.viewport_origin_ = viewport_origin;
  self.viewport_size_ = viewport_size;
  self.surface_size_ = surface_size;

  // stages are registered again every frame, the program keeps what it recorded
  OX_DEFER(&) {
    self.clear_stages();
    self.shared_resources.clear();
    self.render_target = nullptr;
  };

  OX_CHECK_GT(target.extent.width, 0u);
  OX_CHECK_GT(target.extent.height, 0u);

  const auto upscaler = UpscalerSettings{
    .backend = static_cast<UpscalerBackend>(
      std::clamp(cvar.cvar_upscaler_backend.get(), 0, static_cast<i32>(UpscalerBackend::Count) - 1)
    ),
    .quality = static_cast<UpscalerQuality>(
      std::clamp(cvar.cvar_upscaler_quality.get(), 0, static_cast<i32>(UpscalerQuality::Count) - 1)
    ),
    .sharpness = cvar.cvar_upscaler_sharpness.get(),
  };
  const auto is_upscaling = upscaler.backend != UpscalerBackend::None;

  // everything from the visbuffer through lighting runs at render resolution; only the upscale
  // output and the post chain behind it are display resolution
  const auto display_size = glm::uvec2(target.extent.width, target.extent.height);
  const auto render_size = is_upscaling ? upscaler_render_extent(display_size, upscaler.quality) : display_size;
  self.render_size_ = render_size;

  if (is_upscaling && !cvar.cvar_upscaler_disable_jitter.as_bool()) {
    const auto phase_count = upscaler_jitter_phase_count(render_size.x, display_size.x);
    self.previous_jitter = self.current_jitter;
    self.current_jitter = upscaler_jitter_offset(self.jitter_frame_index, phase_count);
    self.jitter_frame_index += 1;
  } else if (is_upscaling) {
    // jitter held at zero; the accumulator still runs, which isolates jitter related artifacts
    self.previous_jitter = {};
    self.current_jitter = {};
    self.jitter_frame_index += 1;
  } else {
    self.previous_jitter = {};
    self.current_jitter = {};
    self.jitter_frame_index = 0;
  }

  self.camera_data.resolution = render_size;
  self.camera_data.temporalaa_jitter = self.current_jitter;
  self.camera_data.temporalaa_jitter_prev = self.previous_jitter;
  self.camera_data.texture_mip_bias = is_upscaling ? upscaler_mip_bias(render_size, display_size) : -1.0f;

  self.render_queue_2d.update();
  self.render_queue_2d.sort();

  self.render_target = &target;
  const auto shape = self.frame_shape(display_size, target.format, cvar);
  self.prepare_frame_params(shape, cvar);

  auto* program = self.program_for(shape);
  if (!program) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }

  self.bind_frame(program->program, program->prepared);
  const auto ticket = render_context.execute(program->program);
  if (ticket && shape.has_atmosphere) {
    self.atmosphere_luts_dirty = false;
  }
  if (ticket && shape.has_tlas) {
    self.skinned_blas_pool.commit_builds();
  }
  if (shape.has_ddgi) {
    self.ddgi_history_valid = ticket.has_value();
  }
  if (self.frame_params.is_upscaling) {
    if (ticket) {
      self.fsr3_history_ping = !self.fsr3_history_ping;
      self.fsr3_history_valid = true;
      self.fsr3_previous_render_size = shape.render_size;
      self.fsr3_previous_display_size = shape.display_size;
      self.fsr3_frame_index += 1;
    } else {
      self.fsr3_history_valid = false;
    }
  }
  return ticket;
}

auto RendererInstance::frame_shape(
  this RendererInstance& self, glm::uvec2 display_size, vir::Format target_format, const RendererCVar& cvar
) -> FrameShape {
  ZoneScoped;

  auto& render_context = *self.renderer.render_context;
  const auto upscaler = static_cast<UpscalerBackend>(
    std::clamp(cvar.cvar_upscaler_backend.get(), 0, static_cast<i32>(UpscalerBackend::Count) - 1)
  );
  const auto is_upscaling = upscaler != UpscalerBackend::None;
  const auto debug_view = static_cast<GPU::DebugView>(cvar.cvar_debug_view.get());
  const auto is_debugging = debug_view != GPU::DebugView::None && cvar.cvar_enable_debug_renderer.as_bool();
  const auto has_meshes = self.frame_data.mesh_instance_count > 0;
  const auto has_tlas = render_context.use_ray_tracing() && self.tlas_has_consumer(cvar) && has_meshes &&
                        self.scene_tlas.structure != AccelerationStructureID::Invalid;
  const auto has_rtao = cvar.cvar_rtao_enable.as_bool() && has_tlas;
  const auto has_ddgi = !self.probe_volumes.empty() && has_tlas && render_context.use_ray_tracing_pipeline();

  if (cvar.cvar_bloom_enable.as_bool()) {
    self.gpu_scene_flags |= GPU::SceneFlags::HasBloom;
  }
  // a temporal upscaler already resolves aliasing, and FXAA would smear its output
  if (cvar.cvar_fxaa_enable.as_bool() && !is_upscaling) {
    self.gpu_scene_flags |= GPU::SceneFlags::HasFXAA;
  }
  // RTAO feeds the same attachment the GTAO path writes, so PBR needs the flag either way
  if (cvar.cvar_vbgtao_enable.as_bool() || has_rtao) {
    self.gpu_scene_flags |= GPU::SceneFlags::HasGTAO;
  }
  if (cvar.cvar_contact_shadows_enabled.as_bool()) {
    self.gpu_scene_flags |= GPU::SceneFlags::HasContactShadows;
  }
  if (cvar.cvar_transparent_background.as_bool()) {
    self.gpu_scene_flags |= GPU::SceneFlags::TransparentBackground;
  }
  if (cvar.cvar_particles_enable.as_bool()) {
    self.gpu_scene_flags |= GPU::SceneFlags::HasParticles;
  }
  if (cvar.cvar_particle_sort.as_bool()) {
    self.gpu_scene_flags |= GPU::SceneFlags::HasParticleSorting;
  }
  if (has_ddgi) {
    self.gpu_scene_flags |= GPU::SceneFlags::HasDDGI;
  }

  const auto flags = self.gpu_scene_flags;
  auto total_probe_count = 0_u32;
  for (const auto& volume : self.probe_volumes) {
    total_probe_count += volume.probe_count;
  }

  const auto has_particles = cvar.cvar_particles_enable.as_bool() && !self.frame_data.particle_emitters.empty();
  auto shape = FrameShape{
    .display_size = display_size,
    .render_size = self.render_size_,
    .upscaler = upscaler,
    .debug_view = is_debugging ? debug_view : GPU::DebugView::None,
    .target_format = target_format,
    .scene_flags = flags,
    .tonemap = self.tonemap_type,
    .tlas = has_tlas ? self.tlas_handle() : VK_NULL_HANDLE,
    .is_debugging = is_debugging,
    .has_meshes = has_meshes,
    .has_terrain = self.scene.terrain != nullptr && self.scene.terrain->is_baked(),
    .has_sprites = !self.render_queue_2d.sprite_data.empty(),
    .has_particles = has_particles,
    .has_atmosphere = static_cast<bool>(flags & GPU::SceneFlags::HasAtmosphere),
    .has_directional_light = static_cast<bool>(flags & GPU::SceneFlags::HasDirectionalLight),
    .has_directional_shadows = self.directional_light_cast_shadows,
    .has_pointspot_shadows = self.frame_data.shadow_point_light_count + self.frame_data.shadow_spot_light_count > 0,
    .has_bloom = static_cast<bool>(flags & GPU::SceneFlags::HasBloom),
    .has_fxaa = static_cast<bool>(flags & GPU::SceneFlags::HasFXAA),
    .has_gtao = static_cast<bool>(flags & GPU::SceneFlags::HasGTAO),
    .has_rtao = has_rtao,
    .has_contact_shadows = static_cast<bool>(flags & GPU::SceneFlags::HasContactShadows),
    .has_eye_adaptation = static_cast<bool>(flags & GPU::SceneFlags::HasEyeAdaptation),
    .has_ddgi = has_ddgi,
    .has_tlas = has_tlas,
    .has_debug_renderer = cvar.cvar_enable_debug_renderer.as_bool(),
    .use_mesh_shaders = self.frame_data.use_mesh_shaders,
    .is_transparent = static_cast<bool>(flags & GPU::SceneFlags::TransparentBackground),
    .is_particle_sort = has_particles && self.frame_data.particle_sort_enabled,
    .particle_sorted_count = has_particles ? self.frame_data.particle_sorted_count : 0_u32,
    .probe_volume_count = static_cast<u32>(self.probe_volumes.size()),
    .ddgi_probe_count = total_probe_count,
    .ddgi_rays_per_probe = static_cast<u32>(std::clamp(cvar.cvar_ddgi_rays_per_probe.get(), 8, 512)),
  };
  for (const auto& callback : self.stage_callbacks) {
    shape.stages.push_back(callback.name);
  }

  std::ranges::sort(shape.stages);
  return shape;
}

auto RendererInstance::prepare_frame_params(
  this RendererInstance& self, const FrameShape& shape, const RendererCVar& cvar
) -> void {
  ZoneScoped;

  auto& params = self.frame_params;
  params.shape = shape;
  params.cvar = &cvar;
  params.delta_time = static_cast<f32>(App::get_timestep().get_millis()) * 0.001f;
  params.frame_index = static_cast<u32>(self.renderer.render_context->num_frames);
  params.is_upscaling = shape.upscaler != UpscalerBackend::None;
  params.render_size = shape.render_size;
  params.display_size = shape.display_size;
  params.bloom_intensity = cvar.cvar_bloom_intensity.get();
  params.cull_camera = GPU::CullCamera{
    .projection_view = self.camera_data.projection_view,
    .position = self.camera_data.position,
    .acceptable_lod_error = self.camera_data.acceptable_lod_error,
    .resolution = self.camera_data.resolution,
    .near_clip = self.camera_data.near_clip,
    .mesh_instance_count = self.frame_data.mesh_instance_count,
    .jitter = self.current_jitter,
    .texture_mip_bias = self.camera_data.texture_mip_bias,
  };

  if (params.is_upscaling) {
    const auto render_size = shape.render_size;
    const auto display_size = shape.display_size;
    self.allocate_fsr3_resources(render_size, display_size);

    const auto is_resolution_changed = self.fsr3_previous_render_size != render_size ||
                                       self.fsr3_previous_display_size != display_size;
    const auto is_reset = !self.fsr3_history_valid || is_resolution_changed;
    if (is_reset) {
      self.fsr3_frame_index = 0;
    }

    auto constants = GPU::FSR3Constants{
      .render_size = glm::ivec2(render_size),
      .previous_frame_render_size = glm::ivec2(is_reset ? render_size : self.fsr3_previous_render_size),
      .upscale_size = glm::ivec2(display_size),
      .previous_frame_upscale_size = glm::ivec2(is_reset ? display_size : self.fsr3_previous_display_size),
      .max_render_size = glm::ivec2(self.fsr3_render_size),
      .max_upscale_size = glm::ivec2(self.fsr3_display_size),
      .jitter_offset = self.current_jitter,
      .previous_frame_jitter_offset = is_reset ? self.current_jitter : self.previous_jitter,
      // velocity is written in render resolution pixels, fsr reprojects in uv
      .motion_vector_scale = 1.0f / glm::vec2(render_size),
      .downscale_factor = glm::vec2(render_size) / glm::vec2(display_size),
      // motion vectors are built from unjittered matrices, so there is nothing to cancel
      .motion_vector_jitter_cancellation = {},
      .jitter_phase_count = static_cast<f32>(upscaler_jitter_phase_count(render_size.x, display_size.x)),
      .delta_time = params.delta_time,
      // oxylus does not pre-expose the hdr buffer, so there is no exposure delta to undo
      .delta_pre_exposure = 1.0f,
      .frame_index = static_cast<f32>(self.fsr3_frame_index),
    };

    // reversed-z with a finite far plane: the SDK's inverted, non-infinite case
    const auto near_clip = self.camera_data.near_clip;
    const auto far_clip = self.camera_data.far_clip;
    const auto depth_min = std::max(near_clip, far_clip);
    const auto depth_max = std::min(near_clip, far_clip);
    const auto q = depth_max / (depth_min - depth_max);
    const auto aspect = static_cast<f32>(render_size.x) / static_cast<f32>(render_size.y);
    const auto fov_vertical = glm::radians(self.camera_data.fov);
    const auto cot_half_fov_y = std::cos(0.5f * fov_vertical) / std::sin(0.5f * fov_vertical);
    constants.device_to_view_depth = {-1.0f * q, q * depth_min, aspect / cot_half_fov_y, 1.0f / cot_half_fov_y};
    constants.tan_half_fov = 1.0f / cot_half_fov_y;

    params.fsr3_constants = constants;
    params.fsr3_sharpness = std::clamp(cvar.cvar_upscaler_sharpness.get(), 0.0f, 1.0f);
    params.fsr3_reset = is_reset;
    params.fsr3_debug_view = static_cast<u32>(std::max(cvar.cvar_upscaler_debug_view.get(), 0));
    params.fsr3_history_ping = self.fsr3_history_ping;
  }

  params.is_terrain_brush_active = false;
  params.is_terrain_brush_painting = false;
  if (shape.has_terrain) {
    auto& terrain = *self.scene.terrain;
    params.is_terrain_brush_active = terrain.brush.active;
    params.is_terrain_brush_painting = terrain.brush.active && terrain.brush.painting;
    if (params.is_terrain_brush_painting) {
      terrain.edits_dirty = true;
    }

    terrain.brush.active = false;
    terrain.brush.painting = false;
  }

  self.prepare_virtual_shadowmap();
  self.prepare_ddgi(cvar);
}

auto RendererInstance::program_for(this RendererInstance& self, const FrameShape& shape) -> RecordedFrame* {
  ZoneScoped;

  if (const auto it = self.programs.find(shape); it != self.programs.end()) {
    const auto order = std::ranges::find(self.program_order, shape);
    std::rotate(order, order + 1, self.program_order.end());
    return it->second.get();
  }

  const auto timer = Timer();
  auto recorded = std::make_unique<RecordedFrame>();
  auto module = vir::Module{};
  self.record_frame(module, shape, recorded->prepared);
  auto compiled = self.renderer.render_context->compile(std::move(module));
  if (!compiled) {
    OX_LOG_ERROR("failed to compile the frame program ({})", std::to_underlying(compiled.error()));
    return nullptr;
  }

  if (const auto* dump = std::getenv("OX_DUMP_IR"); dump && std::string_view(dump) == "1") {
    OX_LOG_INFO("frame program IR:\n{}", compiled->dump());
  }

  auto& stats = self.program_stats;
  stats.compile_count += 1;
  stats.last_compile_ms = timer.get_elapsed_ms();

  const auto evicted = std::ranges::find(self.evicted_shapes, FrameShapeHash{}(shape));
  if (evicted != self.evicted_shapes.end()) {
    self.evicted_shapes.erase(evicted);
    stats.recompile_count += 1;
    // powers of two, so a shape thrashing every frame doesn't flood the log
    if (std::has_single_bit(stats.recompile_count)) {
      OX_LOG_WARN(
        "recompiled an evicted frame program in {:.1f} ms, {} times so far. More than {} frame shapes are alternating",
        stats.last_compile_ms,
        stats.recompile_count,
        MAX_FRAME_PROGRAMS
      );
    }
  }

  recorded->program = std::move(*compiled);
  auto* program = self.programs.emplace(shape, std::move(recorded)).first->second.get();
  self.program_order.push_back(shape);
  while (self.program_order.size() > MAX_FRAME_PROGRAMS) {
    if (self.evicted_shapes.size() >= EVICTED_SHAPE_HISTORY) {
      self.evicted_shapes.erase(self.evicted_shapes.begin());
    }

    self.evicted_shapes.push_back(FrameShapeHash{}(self.program_order.front()));
    self.programs.erase(self.program_order.front());
    self.program_order.erase(self.program_order.begin());
  }

  stats.cached_count = static_cast<u32>(self.programs.size());
  return program;
}

auto RendererInstance::bind_frame(
  this const RendererInstance& self, vir::Program& program, const PreparedFrame& prepared
) -> void {
  ZoneScoped;

  auto& render_context = *self.renderer.render_context;
  const auto& data = self.frame_data;
  program.set(prepared.transforms_world, render_context.buffer(self.transforms_world_buffer));
  program.set(prepared.transforms_previous, render_context.buffer(self.transforms_previous_buffer));
  program.set(prepared.meshes, render_context.buffer(self.meshes_buffer));
  program.set(prepared.blas_addresses, render_context.buffer(self.blas_addresses_buffer));
  program.set(prepared.skinned_blas_addresses, render_context.buffer(self.skinned_blas_addresses_buffer));
  program.set(prepared.mesh_instances, render_context.buffer(self.mesh_instances_buffer));
  program.set(prepared.skinning_transforms, render_context.buffer(self.skinning_transforms_buffer));
  program.set(prepared.lights, render_context.buffer(self.lights_buffer));
  program.set(
    prepared.meshlet_instance_visibility_mask,
    render_context.buffer(self.meshlet_instance_visibility_mask_buffer)
  );
  program.set(prepared.exposure, render_context.buffer(self.exposure_buffer));
  if (self.render_target != nullptr) {
    program.set(prepared.post_process.render_target, *self.render_target);
  }
  program.set(prepared.skinned_vertices, render_context.buffer(self.skinned_vertices_buffer));
  program.set(prepared.materials, render_context.buffer(self.renderer.materials_buffer));
  program.set(prepared.terrain_patch_visibility_mask, render_context.buffer(self.terrain_patch_visibility_mask_buffer));

  if (program.has_variable(prepared.transforms_world_staging)) {
    auto staging = render_context.upload_frame_buffer(host_bytes(data.transforms_world), "transforms world upload");
    if (!staging) {
      OX_LOG_ERROR("failed to stage {}", "transforms world upload");
    }
    program.set(prepared.transforms_world_staging, staging.value_or(vir::Buffer{}));
  }
  if (program.has_variable(prepared.transforms_previous_staging)) {
    auto staging = render_context.upload_frame_buffer(
      host_bytes(data.transforms_previous),
      "transforms previous upload"
    );
    if (!staging) {
      OX_LOG_ERROR("failed to stage {}", "transforms previous upload");
    }
    program.set(prepared.transforms_previous_staging, staging.value_or(vir::Buffer{}));
  }
  self.bind_upload(program, prepared.meshes_upload, host_bytes(data.meshes), "meshes upload");
  self.bind_upload(program, prepared.blas_addresses_upload, host_bytes(data.blas_addresses), "blas addresses upload");
  self.bind_upload(
    program,
    prepared.skinned_blas_addresses_upload,
    host_bytes(data.skinned_blas_addresses),
    "skinned blas addresses upload"
  );
  self.bind_upload(program, prepared.mesh_instances_upload, host_bytes(data.mesh_instances), "mesh instances upload");
  self.bind_upload(
    program,
    prepared.skinning_transforms_upload,
    host_bytes(data.skinning_transforms),
    "skinning transforms upload"
  );
  self.bind_upload(program, prepared.lights_upload, host_bytes(data.lights), "lights upload");
  program.set(prepared.is_meshlet_visibility_reset, data.is_meshlet_visibility_reset);
  program.set(prepared.is_terrain_visibility_reset, data.is_terrain_visibility_reset);
  program.set(prepared.is_exposure_reset, data.is_exposure_reset);

  if (program.has_variable(prepared.camera)) {
    auto staging = render_context.upload_frame_buffer(host_bytes(self.camera_data), "camera");
    if (!staging) {
      OX_LOG_ERROR("failed to stage {}", "camera");
    }
    program.set(prepared.camera, staging.value_or(vir::Buffer{}));
  }
  if (program.has_variable(prepared.atmosphere)) {
    auto staging = render_context.upload_frame_buffer(host_bytes(self.atmosphere), "atmosphere");
    if (!staging) {
      OX_LOG_ERROR("failed to stage {}", "atmosphere");
    }
    program.set(prepared.atmosphere, staging.value_or(vir::Buffer{}));
  }
  if (program.has_variable(prepared.dirty_mesh_instances)) {
    auto staging = render_context.upload_frame_buffer(host_bytes(data.dirty_mesh_instances), "dirty mesh instances");
    if (!staging) {
      OX_LOG_ERROR("failed to stage {}", "dirty mesh instances");
    }
    program.set(prepared.dirty_mesh_instances, staging.value_or(vir::Buffer{}));
  }
  if (program.has_variable(prepared.removed_mesh_bounds)) {
    auto staging = render_context.upload_frame_buffer(host_bytes(data.removed_mesh_bounds), "removed mesh bounds");
    if (!staging) {
      OX_LOG_ERROR("failed to stage {}", "removed mesh bounds");
    }
    program.set(prepared.removed_mesh_bounds, staging.value_or(vir::Buffer{}));
  }
  if (program.has_variable(prepared.probe_volumes)) {
    auto staging = render_context.upload_frame_buffer(host_bytes(self.probe_volumes), "probe volumes");
    if (!staging) {
      OX_LOG_ERROR("failed to stage {}", "probe volumes");
    }
    program.set(prepared.probe_volumes, staging.value_or(vir::Buffer{}));
  }
  if (program.has_variable(prepared.sprites)) {
    auto staging = render_context.upload_frame_buffer(host_bytes(self.render_queue_2d.sprite_data), "sprites");
    if (!staging) {
      OX_LOG_ERROR("failed to stage {}", "sprites");
    }
    program.set(prepared.sprites, staging.value_or(vir::Buffer{}));
  }

  if (program.has_variable(prepared.meshlet_instances)) {
    auto buffer = render_context.allocate_frame_buffer({
      .size = std::max<u64>(data.meshlet_instances_size, 16),
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
               VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      .location = vir::MemoryLocation::GpuOnly,
      .alignment = 16,
      .name = "meshlet instances",
    });
    if (!buffer) {
      OX_LOG_ERROR("failed to allocate {}", "meshlet instances");
    }
    program.set(prepared.meshlet_instances, buffer.value_or(vir::Buffer{}));
  }
  if (program.has_variable(prepared.visible_meshlet_instances_indices)) {
    auto buffer = render_context.allocate_frame_buffer({
      .size = std::max<u64>(data.visible_meshlet_instances_size, 16),
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
               VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      .location = vir::MemoryLocation::GpuOnly,
      .alignment = 16,
      .name = "visible meshlet instances",
    });
    if (!buffer) {
      OX_LOG_ERROR("failed to allocate {}", "visible meshlet instances");
    }
    program.set(prepared.visible_meshlet_instances_indices, buffer.value_or(vir::Buffer{}));
  }
  if (program.has_variable(prepared.reordered_indices)) {
    auto buffer = render_context.allocate_frame_buffer({
      .size = std::max<u64>(data.reordered_indices_size, 16),
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
               VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      .location = vir::MemoryLocation::GpuOnly,
      .alignment = 16,
      .name = "reordered indices",
    });
    if (!buffer) {
      OX_LOG_ERROR("failed to allocate {}", "reordered indices");
    }
    program.set(prepared.reordered_indices, buffer.value_or(vir::Buffer{}));
  }
  if (program.has_variable(prepared.terrain_visible_patches)) {
    auto buffer = render_context.allocate_frame_buffer({
      .size = std::max<u64>(data.terrain_visible_patches_size, 16),
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
               VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      .location = vir::MemoryLocation::GpuOnly,
      .alignment = 16,
      .name = "terrain visible patches",
    });
    if (!buffer) {
      OX_LOG_ERROR("failed to allocate {}", "terrain visible patches");
    }
    program.set(prepared.terrain_visible_patches, buffer.value_or(vir::Buffer{}));
  }

  self.bind_acceleration_structures(program, prepared);
  self.bind_cull_variables(program, prepared.cull);
  self.bind_light_grid_variables(program, prepared.light_grid);
  self.bind_terrain_variables(program, prepared.terrain);
  self.bind_shadow_variables(program, prepared.shadow);
  self.bind_atmosphere_variables(program, prepared.pbr);
  self.bind_ambient_occlusion_variables(program, prepared.pbr);
  self.bind_rtao_variables(program, prepared.pbr);
  self.bind_pbr_variables(program, prepared.pbr);
  self.bind_ddgi_variables(program, prepared.ddgi);
  self.bind_fsr3_variables(program, prepared.fsr3);
  self.bind_eye_adaptation_variables(program, prepared.post_process);
  self.bind_bloom_variables(program, prepared.post_process);
  self.bind_tonemap_variables(program, prepared.post_process);
  self.bind_particle_variables(program, prepared.particle);
  self.bind_debug_variables(program, prepared.debug);

  if (program.has_variable(prepared.particle.emitters)) {
    auto staging = render_context.upload_frame_buffer(host_bytes(data.particle_emitters), "particle emitters");
    if (!staging) {
      OX_LOG_ERROR("failed to stage {}", "particle emitters");
    }
    program.set(prepared.particle.emitters, staging.value_or(vir::Buffer{}));
  }
  if (program.has_variable(prepared.particle.programs)) {
    auto staging = render_context.upload_frame_buffer(host_bytes(data.particle_instructions), "particle programs");
    if (!staging) {
      OX_LOG_ERROR("failed to stage {}", "particle programs");
    }
    program.set(prepared.particle.programs, staging.value_or(vir::Buffer{}));
  }
  if (program.has_variable(prepared.particle.constants)) {
    auto staging = render_context.upload_frame_buffer(host_bytes(data.particle_constants), "particle constants");
    if (!staging) {
      OX_LOG_ERROR("failed to stage {}", "particle constants");
    }
    program.set(prepared.particle.constants, staging.value_or(vir::Buffer{}));
  }
  if (program.has_variable(prepared.particle.index_counts)) {
    auto staging = render_context.upload_frame_buffer(host_bytes(data.particle_index_counts), "particle index counts");
    if (!staging) {
      OX_LOG_ERROR("failed to stage {}", "particle index counts");
    }
    program.set(prepared.particle.index_counts, staging.value_or(vir::Buffer{}));
  }
  if (program.has_variable(prepared.particle.meshes)) {
    auto staging = render_context.upload_frame_buffer(host_bytes(data.particle_meshes), "particle meshes");
    if (!staging) {
      OX_LOG_ERROR("failed to stage {}", "particle meshes");
    }
    program.set(prepared.particle.meshes, staging.value_or(vir::Buffer{}));
  }
  if (program.has_variable(prepared.debug.vertices)) {
    auto staging = render_context.upload_frame_buffer(host_bytes(self.debug_vertices), "debug vertices");
    if (!staging) {
      OX_LOG_ERROR("failed to stage {}", "debug vertices");
    }
    program.set(prepared.debug.vertices, staging.value_or(vir::Buffer{}));
  }

  for (const auto& callback : self.stage_callbacks) {
    if (!callback.bind) {
      continue;
    }

    if (const auto it = prepared.stage_variables.find(callback.name); it != prepared.stage_variables.end()) {
      callback.bind(program, it->second);
    }
  }
}

auto RendererInstance::tlas_has_consumer(this const RendererInstance& self, const RendererCVar& cvar) -> bool {
  return cvar.cvar_rtao_enable.as_bool() ||
         (!self.probe_volumes.empty() && self.renderer.render_context->use_ray_tracing_pipeline());
}

auto RendererInstance::tlas_handle(this const RendererInstance& self) -> VkAccelerationStructureKHR {
  if (self.scene_tlas.structure == AccelerationStructureID::Invalid) {
    return VK_NULL_HANDLE;
  }

  return self.renderer.render_context->acceleration_structure(self.scene_tlas.structure).handle;
}

auto RendererInstance::record_acceleration_structures(
  this RendererInstance& self, vir::Module& module, FrameRecording& recording
) -> void {
  ZoneScoped;
  auto& prepared = recording.prepared;
  auto& values = recording.values;

  auto& render_context = *self.renderer.render_context;

  // the skinned structures, refit or rebuilt out of this frame's skinned vertices. Most frames build
  // a handful, and a frame that builds none records nothing
  prepared.skinned_blas_arena = module.declare_buffer_var("skinned blas arena", STRUCTURE_REST);
  prepared.skinned_blas_scratch = module.declare_buffer_var("skinned blas scratch", STRUCTURE_REST);
  prepared.skinned_blas_builds = module.declare_acceleration_structure_builds_var("skinned blas builds");
  auto arena = prepared.skinned_blas_arena;
  auto arena_scratch = prepared.skinned_blas_scratch;
  {
    const auto resources = std::array{
      vir::ResourceAccess{arena, vir::Access::AccelerationStructureBuildRW},
      vir::ResourceAccess{arena_scratch, vir::Access::AccelerationStructureBuildRW},
      vir::ResourceAccess{values.skinned_vertices, vir::Access::AccelerationStructureBuildRead},
    };
    const auto built = module.build_acceleration_structures(prepared.skinned_blas_builds, {}, resources);
    arena = built[0];
    arena_scratch = built[1];
    values.skinned_vertices = built[2];
  }

  prepared.tlas_instances = module.declare_buffer_var("tlas instances", vir::Access::AccelerationStructureBuildRead);
  prepared.tlas_instance_count = module.declare_u32_var("tlas instance count", 0);
  prepared.tlas_instance_counts = module.declare_bytes_var("tlas instance counts", sizeof(glm::uvec2));
  auto instances = prepared.tlas_instances;
  std::tie(
    instances,
    values.mesh_instances,
    values.transforms_world,
    values.blas_addresses,
    values.skinned_blas_addresses
  ) = std::tuple_cat( //
    module
      .begin_compute(
        {{instances, vir::Access::ComputeWrite},
         {values.mesh_instances, vir::Access::ComputeRead},
         {values.transforms_world, vir::Access::ComputeRead},
         {values.blas_addresses, vir::Access::ComputeRead},
         {values.skinned_blas_addresses, vir::Access::ComputeRead}}
      )
      .with_name("tlas write instances")
      .bind_compute_pipeline(render_context.pipeline("tlas_write_instances"))
      .bind_buffer(0, 0, values.mesh_instances)
      .bind_buffer(0, 1, values.transforms_world)
      .bind_buffer(0, 2, values.blas_addresses)
      .bind_buffer(0, 3, instances)
      .bind_buffer(0, 4, values.skinned_blas_addresses)
      .push_constants_from(prepared.tlas_instance_counts)
      .dispatch_invocations(prepared.tlas_instance_count, 1, 1)
      .end_compute()
  );

  prepared.tlas = module.declare_buffer_var("tlas", STRUCTURE_REST);
  prepared.tlas_scratch = module.declare_buffer_var("tlas scratch", STRUCTURE_REST);
  prepared.tlas_builds = module.declare_acceleration_structure_builds_var("tlas builds");
  auto tlas_scratch = prepared.tlas_scratch;
  {
    // the arena rides along so the build lands after the skinned structures it points at
    const auto resources = std::array{
      vir::ResourceAccess{prepared.tlas, vir::Access::AccelerationStructureBuildWrite},
      vir::ResourceAccess{instances, vir::Access::AccelerationStructureBuildRead},
      vir::ResourceAccess{tlas_scratch, vir::Access::AccelerationStructureBuildRW},
      vir::ResourceAccess{arena, vir::Access::AccelerationStructureBuildRead},
    };
    const auto built = module.build_acceleration_structures(prepared.tlas_builds, {}, resources);
    values.tlas = built[0];
    instances = built[1];
    tlas_scratch = built[2];
    arena = built[3];
  }

  module.export_to(arena, STRUCTURE_REST);
  module.export_to(arena_scratch, STRUCTURE_REST);
  module.export_to(instances, vir::Access::AccelerationStructureBuildRead);
  module.export_to(tlas_scratch, STRUCTURE_REST);
  self.shared_resources.buffer_resources["tlas"] = values.tlas;
}

auto RendererInstance::bind_acceleration_structures(
  this const RendererInstance& self, vir::Program& program, const PreparedFrame& prepared
) -> void {
  ZoneScoped;

  const auto& render_context = *self.renderer.render_context;
  const auto& data = self.frame_data;
  // a structure nothing has built yet still binds, against a stand-in
  const auto tlas_buffer = self.scene_tlas.structure != AccelerationStructureID::Invalid
                             ? render_context.acceleration_structure(self.scene_tlas.structure).buffer
                             : BufferID::Invalid;

  program.set(
    prepared.skinned_blas_arena,
    render_context.buffer(
      self.skinned_blas_pool.buffer != BufferID::Invalid ? self.skinned_blas_pool.buffer : self.placeholder_buffer
    )
  );
  program.set(
    prepared.skinned_blas_scratch,
    render_context.buffer(
      self.skinned_blas_pool.scratch_buffer != BufferID::Invalid ? self.skinned_blas_pool.scratch_buffer
                                                                 : self.placeholder_buffer
    )
  );
  program.set(prepared.skinned_blas_builds, data.skinned_blas_builds);
  program.set(
    prepared.tlas_instances,
    render_context.buffer(
      self.scene_tlas.instances_buffer != BufferID::Invalid ? self.scene_tlas.instances_buffer : self.placeholder_buffer
    )
  );
  program.set(prepared.tlas_instance_count, data.mesh_instance_count);
  program.set_bytes(prepared.tlas_instance_counts, glm::uvec2(data.mesh_instance_count, data.skinned_instance_count));
  program.set(
    prepared.tlas,
    render_context.buffer(tlas_buffer != BufferID::Invalid ? tlas_buffer : self.placeholder_buffer)
  );
  program.set(
    prepared.tlas_scratch,
    render_context.buffer(
      self.scene_tlas.scratch_buffer != BufferID::Invalid ? self.scene_tlas.scratch_buffer : self.placeholder_buffer
    )
  );
  program.set(prepared.tlas_builds, data.tlas_builds);
}

auto RendererInstance::record_frame(
  this RendererInstance& self, vir::Module& module, const FrameShape& shape, PreparedFrame& prepared
) -> void {
  ZoneScoped;

  using ContactShadowsPush = PushConstants<glm::vec3, u32, f32, f32>;

  auto& render_context = *self.renderer.render_context;
  const auto& data = self.frame_data;
  const auto render_extent = vir::Extent3D{shape.render_size.x, shape.render_size.y, 1};
  const auto display_extent = vir::Extent3D{shape.display_size.x, shape.display_size.y, 1};
  const auto is_upscaling = shape.upscaler != UpscalerBackend::None;
  const auto is_overdraw = shape.is_debugging && shape.debug_view == GPU::DebugView::Overdraw;
  const auto black = vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 1.0f);
  const auto white = vir::ClearValue::rgba_f32(1.0f, 1.0f, 1.0f, 1.0f);
  const auto zero = vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 0.0f);

  // --- per frame inputs ---
  auto recording = FrameRecording{.shape = shape, .prepared = prepared};
  auto& values = recording.values;
  prepared.transforms_world = module.declare_buffer_var("transforms world", SCENE_BUFFER_REST);
  prepared.transforms_previous = module.declare_buffer_var("transforms previous", SCENE_BUFFER_REST);
  values.transforms_world = record_transform_upload<GPU::TransformWorld>(
    module,
    data,
    "transforms world upload",
    prepared.transforms_world,
    prepared.transforms_world_staging
  );
  values.transforms_previous = record_transform_upload<GPU::TransformPrevious>(
    module,
    data,
    "transforms previous upload",
    prepared.transforms_previous,
    prepared.transforms_previous_staging
  );

  prepared.meshes = module.declare_buffer_var("meshes", SCENE_BUFFER_REST);
  prepared.blas_addresses = module.declare_buffer_var("blas addresses", SCENE_BUFFER_REST);
  prepared.skinned_blas_addresses = module.declare_buffer_var("skinned blas addresses", SCENE_BUFFER_REST);
  // read back by the next frame through the pointers in here, so it rests read and write
  prepared.mesh_instances = module.declare_buffer_var("mesh instances", GPU_STATE_REST);
  prepared.skinning_transforms = module.declare_buffer_var("skinning transforms", SCENE_BUFFER_REST);
  prepared.lights = module.declare_buffer_var("lights", SCENE_BUFFER_REST);
  values.meshes = record_upload(module, "meshes upload", prepared.meshes, prepared.meshes_upload);
  values.blas_addresses = record_upload(
    module,
    "blas addresses upload",
    prepared.blas_addresses,
    prepared.blas_addresses_upload
  );
  values.skinned_blas_addresses = record_upload(
    module,
    "skinned blas addresses upload",
    prepared.skinned_blas_addresses,
    prepared.skinned_blas_addresses_upload
  );
  values.mesh_instances = record_upload(
    module,
    "mesh instances upload",
    prepared.mesh_instances,
    prepared.mesh_instances_upload
  );
  values.skinning_transforms = record_upload(
    module,
    "skinning transforms upload",
    prepared.skinning_transforms,
    prepared.skinning_transforms_upload
  );
  values.lights = record_upload(module, "lights upload", prepared.lights, prepared.lights_upload);

  prepared.meshlet_instance_visibility_mask = module.declare_buffer_var(
    "meshlet instance visibility mask",
    GPU_STATE_REST
  );
  prepared.exposure = module.declare_buffer_var("exposure", GPU_STATE_REST);
  values.meshlet_instance_visibility_mask = record_fill(
    module,
    "meshlet instance visibility reset",
    prepared.meshlet_instance_visibility_mask,
    0,
    prepared.is_meshlet_visibility_reset
  );
  values.exposure = record_fill(
    module,
    "exposure reset",
    prepared.exposure,
    std::bit_cast<u32>(1.0f),
    prepared.is_exposure_reset
  );

  prepared.skinned_vertices = module.declare_buffer_var("skinned vertices", GPU_STATE_REST);
  prepared.materials = module.declare_buffer_var("materials", SCENE_BUFFER_REST);
  values.skinned_vertices = prepared.skinned_vertices;
  values.materials = prepared.materials;

  prepared.camera = module.declare_buffer_var("camera", vir::Access::None);
  prepared.atmosphere = module.declare_buffer_var("atmosphere", vir::Access::None);
  prepared.dirty_mesh_instances = module.declare_buffer_var("dirty mesh instances", vir::Access::None);
  prepared.removed_mesh_bounds = module.declare_buffer_var("removed mesh bounds", vir::Access::None);
  prepared.meshlet_instances = module.declare_buffer_var("meshlet instances", vir::Access::None);
  prepared.visible_meshlet_instances_indices = module.declare_buffer_var(
    "visible meshlet instances",
    vir::Access::None
  );
  prepared.reordered_indices = module.declare_buffer_var("reordered indices", vir::Access::None);
  values.camera = prepared.camera;
  values.atmosphere = prepared.atmosphere;
  values.dirty_mesh_instances = prepared.dirty_mesh_instances;
  values.removed_mesh_bounds = prepared.removed_mesh_bounds;
  values.meshlet_instances = prepared.meshlet_instances;
  values.visible_meshlet_instances_indices = prepared.visible_meshlet_instances_indices;
  values.reordered_indices = prepared.reordered_indices;

  // --- attachments ---
  const auto hdr_format = shape.is_transparent ? vir::Format::RGBA16Sfloat : vir::Format::B10G11R11UfloatPack32;
  auto final_attachment = module.transient_image({.extent = render_extent, .format = hdr_format, .name = "final"});
  final_attachment = module.clear(final_attachment, shape.is_transparent ? zero : black);
  auto depth_attachment = module.transient_image(
    {.extent = render_extent, .format = vir::Format::D32Sfloat, .name = "depth"}
  );

  depth_attachment = module.clear(depth_attachment, vir::ClearValue::depth(0.0f));

  const auto hiz_extent = vir::Extent3D{
    .width = std::bit_ceil((render_extent.width + 1) >> 1),
    .height = std::bit_ceil((render_extent.height + 1) >> 1),
    .depth = 1,
  };
  auto hiz_attachment = module.transient_image({
    .extent = hiz_extent,
    .format = vir::Format::R32Sfloat,
    .mip_levels = std::min(Texture::calculate_mip_count(hiz_extent), 13u),
    .name = "hiz",
  });
  hiz_attachment = module.clear(hiz_attachment, zero);

  auto sky_transmittance_lut_attachment = self.sky_transmittance_lut.import(module, TEXTURE_REST);
  auto sky_multiscatter_lut_attachment = self.sky_multiscatter_lut.import(module, TEXTURE_REST);
  auto sky_cubemap_attachment = self.sky_cubemap.import(module, TEXTURE_REST);
  auto hilbert_noise_attachment = self.hilbert_noise_lut.import(module, TEXTURE_REST);
  auto sky_view_lut_attachment = module.transient_image({
    .extent = self.sky_view_lut_extent,
    .format = vir::Format::RGBA16Sfloat,
    .name = "sky view lut",
  });
  sky_view_lut_attachment = module.clear(sky_view_lut_attachment, black);
  auto sky_aerial_perspective_attachment = module.transient_image({
    .extent = self.sky_aerial_perspective_lut_extent,
    .format = vir::Format::RGBA16Sfloat,
    .image_type = vir::ImageType::e3D,
    .view_type = vir::ImageViewType::e3D,
    .name = "sky aerial perspective",
  });

  sky_aerial_perspective_attachment = module.clear(sky_aerial_perspective_attachment, black);

  auto visbuffer_attachment = module.transient_image(
    {.extent = render_extent, .format = vir::Format::R32Uint, .name = "visbuffer"}
  );
  // cleared to an invalid transform id
  auto visbuffer_2d_attachment = module.transient_image(
    {.extent = render_extent, .format = vir::Format::R32Uint, .name = "visbuffer 2d"}
  );
  visbuffer_2d_attachment = module.clear(
    visbuffer_2d_attachment,
    vir::ClearValue::rgba_u32(~0_u32, ~0_u32, ~0_u32, ~0_u32)
  );
  auto overdraw_attachment = module.transient_image({
    .extent = is_overdraw ? render_extent : vir::Extent3D{1, 1, 1},
    .format = vir::Format::R32Uint,
    .name = "overdraw",
  });

  // before anything reads vertex positions: the culling passes, both visbuffer encoders and every
  // shadow pass all fetch through the same pointers this rewrites
  if (shape.has_meshes) {
    self.skin_vertices(module, recording);
  }

  {
    const auto extent = glm::uvec2(render_extent.width, render_extent.height);
    std::tie(visbuffer_attachment, overdraw_attachment) = std::tuple_cat(
      module
        .begin_compute(
          {{visbuffer_attachment, vir::Access::ComputeWrite}, {overdraw_attachment, vir::Access::ComputeWrite}}
        )
        .with_name("vis clear")
        .bind_compute_pipeline(render_context.pipeline("visbuffer_clear"))
        .specialize_constant(0, is_overdraw)
        .bind_image(0, 0, visbuffer_attachment)
        .bind_image(0, 1, overdraw_attachment)
        .push_constants(PushConstants(extent))
        .dispatch_invocations_per_pixel(visbuffer_attachment)
        .end_compute()
    );
  }

  auto shadows_attachment = module.transient_image(
    {.extent = render_extent, .format = vir::Format::RG8Unorm, .name = "shadows"}
  );

  shadows_attachment = module.clear(shadows_attachment, white);
  auto albedo_attachment = module.transient_image(
    {.extent = render_extent, .format = vir::Format::RGBA8Srgb, .name = "albedo"}
  );

  albedo_attachment = module.clear(albedo_attachment, black);
  auto normal_attachment = module.transient_image(
    {.extent = render_extent, .format = vir::Format::RGBA8Snorm, .name = "normal"}
  );

  normal_attachment = module.clear(normal_attachment, black);
  auto emissive_attachment = module.transient_image(
    {.extent = render_extent, .format = vir::Format::B10G11R11UfloatPack32, .name = "emissive"}
  );

  emissive_attachment = module.clear(emissive_attachment, black);
  auto metallic_roughness_occlusion_attachment = module.transient_image(
    {.extent = render_extent, .format = vir::Format::RG8Unorm, .name = "metallic roughness occlusion"}
  );

  metallic_roughness_occlusion_attachment = module.clear(metallic_roughness_occlusion_attachment, black);
  // alpha blended content (sprites, particles) cannot produce a meaningful motion vector, so it
  // marks itself reactive here and the upscaler leans on the current frame in those pixels
  auto reactive_mask_attachment = module.transient_image(
    {.extent = render_extent, .format = vir::Format::R8Unorm, .name = "reactive mask"}
  );
  reactive_mask_attachment = module.clear(reactive_mask_attachment, black);
  // background pixels keep a zero vector, which reprojects onto themselves
  auto velocity_attachment = module.transient_image(
    {.extent = render_extent, .format = vir::Format::RG16Sfloat, .name = "velocity"}
  );
  velocity_attachment = module.clear(velocity_attachment, zero);
  auto ambient_occlusion_attachment = module.transient_image(
    {.extent = render_extent, .format = vir::Format::R8Unorm, .name = "ambient occlusion"}
  );

  ambient_occlusion_attachment = module.clear(ambient_occlusion_attachment, white);

  auto vsm_virtual_page_table_attachment = self.vsm_virtual_page_table.import(module, TEXTURE_REST);
  auto vsm_pointspot_page_table_attachment = self.vsm_pointspot_virtual_page_table.import(module, TEXTURE_REST);
  auto vsm_physical_pages_attachment = self.vsm_physical_page_table.import(module, TEXTURE_REST);
  auto vsm_directional_clipmaps_buffer = vir::ValueID::Invalid;
  auto pointspot_views_buffer = vir::ValueID::Invalid;

  auto light_grid_context = LightGridContext{};
  build_light_grid(module, render_context, recording, light_grid_context);
  auto light_grid_buffer = light_grid_context.light_grid_buffer;

  if (shape.has_tlas) {
    self.record_acceleration_structures(module, recording);
  }

  const auto record_visbuffer_stages = [&](vir::ValueID& visbuffer, vir::ValueID& depth) -> void {
    auto resources = SharedResources{};
    resources.image_resources = {{"visbuffer_attachment", visbuffer}, {"depth_attachment", depth}};
    resources.buffer_resources = {
      {"meshlet_instances_buffer", values.meshlet_instances},
      {"mesh_instances_buffer", values.mesh_instances},
    };
    self.execute_stages_after(RenderStage::VisBufferEncode, module, resources, prepared);
    visbuffer = resources.image_resources.at("visbuffer_attachment");
    depth = resources.image_resources.at("depth_attachment");
    values.meshlet_instances = resources.buffer_resources.at("meshlet_instances_buffer");
    values.mesh_instances = resources.buffer_resources.at("mesh_instances_buffer");
  };

  // --- 3D ---
  if (shape.has_meshes || shape.has_terrain) {
    auto main_geometry_context = MainGeometryContext{
      .draw_overdraw = is_overdraw,
      .depth_attachment = depth_attachment,
      .hiz_attachment = hiz_attachment,
      .visbuffer_attachment = visbuffer_attachment,
      .overdraw_attachment = overdraw_attachment,
      .albedo_attachment = albedo_attachment,
      .normal_attachment = normal_attachment,
      .emissive_attachment = emissive_attachment,
      .metallic_roughness_occlusion_attachment = metallic_roughness_occlusion_attachment,
      .velocity_attachment = velocity_attachment,
    };

    // shared by both culls so the visibility and dispatch buffers the early pass's `cull_meshes`
    // allocates carry into the late pass
    auto cull_geometry_context = CullGeometryContext{
      .use_hiz = true,
      .init_cull_meshes = true,
    };

    auto terrain_context = TerrainContext{};
    auto terrain_brush_context = TerrainBrushContext{};
    if (shape.has_terrain) {
      terrain_context.terrain_buffer = self.build_terrain_buffer(module, recording);
      prepared.terrain_visible_patches = module.declare_buffer_var("terrain visible patches", vir::Access::None);
      prepared.terrain_patch_visibility_mask = module.declare_buffer_var(
        "terrain patch visibility mask",
        GPU_STATE_REST
      );
      terrain_context.visible_patches_buffer = prepared.terrain_visible_patches;
      terrain_context.patch_visibility_mask_buffer = record_fill(
        module,
        "terrain patch visibility reset",
        prepared.terrain_patch_visibility_mask,
        0,
        prepared.is_terrain_visibility_reset
      );

      // each map rests where Terrain::bake leaves it
      prepared.terrain
        .heightmap = module.declare_image_var("terrain heightmap", vir::Format::R16Unorm, TERRAIN_HEIGHT_REST);
      prepared.terrain.ridgemap = module.declare_image_var("terrain ridgemap", vir::Format::R16Sfloat, TEXTURE_REST);
      prepared.terrain.normalmap = module.declare_image_var("terrain normalmap", vir::Format::RG16Sfloat, TEXTURE_REST);
      prepared.terrain.splatmap = module.declare_image_var("terrain splatmap", vir::Format::RGBA8Unorm, TEXTURE_REST);
      prepared.terrain
        .patch_minmax = module.declare_image_var("terrain patch minmax", vir::Format::RG16Unorm, TERRAIN_HEIGHT_REST);
      prepared.terrain
        .height_edit = module.declare_image_var("terrain height edit", vir::Format::R32Sfloat, TERRAIN_EDIT_REST);
      prepared.terrain
        .splat_edit = module.declare_image_var("terrain splat edit", vir::Format::RGBA8Unorm, TERRAIN_EDIT_REST);
      terrain_brush_context.maps = TerrainMapValues{
        .heightmap = prepared.terrain.heightmap,
        .ridgemap = prepared.terrain.ridgemap,
        .normalmap = prepared.terrain.normalmap,
        .splatmap = prepared.terrain.splatmap,
        .patch_minmax = prepared.terrain.patch_minmax,
        .height_edit = prepared.terrain.height_edit,
        .splat_edit = prepared.terrain.splat_edit,
      };
      self.apply_terrain_brush(module, recording, terrain_brush_context);
      terrain_context.heightmap_attachment = terrain_brush_context.maps.heightmap;
      terrain_context.patch_minmax_attachment = terrain_brush_context.maps.patch_minmax;
    }

    const auto run_geometry_pass = [&](bool is_late) {
      if (is_late) {
        cull_geometry_context.cull_flags |= GPU::CullFlag::LatePass;
        cull_geometry_context.init_cull_meshes = false;
      }

      if (shape.has_meshes) {
        cull_geometry_context.hiz_attachment = main_geometry_context.hiz_attachment;
        cull_geometry(module, render_context, recording, cull_geometry_context);
        main_geometry_context.hiz_attachment = cull_geometry_context.hiz_attachment;
        main_geometry_context.draw_geometry_cmd_buffer = cull_geometry_context.draw_geometry_cmd_buffer;
        main_geometry_context.cull_flags = cull_geometry_context.cull_flags;
        main_geometry_context.visibility_buffer = cull_geometry_context.visibility_buffer;
        self.draw_for_visbuffer(module, recording, main_geometry_context);
        cull_geometry_context.visibility_buffer = main_geometry_context.visibility_buffer;
        if (shape.use_mesh_shaders) {
          cull_geometry_context.cull_meshlets_cmd_buffer = main_geometry_context.draw_geometry_cmd_buffer;
        }
      }

      if (shape.has_terrain) {
        terrain_context.cull_flags = GPU::CullFlag::TestFrustum | GPU::CullFlag::TestOcclusion;
        if (is_late) {
          terrain_context.cull_flags |= GPU::CullFlag::LatePass;
        }

        terrain_context.hiz_attachment = main_geometry_context.hiz_attachment;
        cull_terrain(module, render_context, recording, terrain_context);
        main_geometry_context.hiz_attachment = terrain_context.hiz_attachment;

        terrain_context.visbuffer_attachment = main_geometry_context.visbuffer_attachment;
        terrain_context.depth_attachment = main_geometry_context.depth_attachment;
        self.draw_terrain_for_visbuffer(module, recording, terrain_context);
        main_geometry_context.visbuffer_attachment = terrain_context.visbuffer_attachment;
        main_geometry_context.depth_attachment = terrain_context.depth_attachment;
      }
    };

    run_geometry_pass(false);
    self.generate_hiz(module, render_context, main_geometry_context);
    run_geometry_pass(true);

    record_visbuffer_stages(main_geometry_context.visbuffer_attachment, main_geometry_context.depth_attachment);

    if (shape.has_meshes) {
      self.decode_visbuffer(module, recording, main_geometry_context);
    }

    if (shape.has_terrain) {
      auto terrain_decode_context = TerrainDecodeContext{
        .terrain_buffer = terrain_context.terrain_buffer,
        .brush_hit_buffer = terrain_brush_context.hit_buffer,
        .normalmap_attachment = terrain_brush_context.maps.normalmap,
        .splatmap_attachment = terrain_brush_context.maps.splatmap,
        .visbuffer_attachment = main_geometry_context.visbuffer_attachment,
        .depth_attachment = main_geometry_context.depth_attachment,
        .albedo_attachment = main_geometry_context.albedo_attachment,
        .normal_attachment = main_geometry_context.normal_attachment,
        .emissive_attachment = main_geometry_context.emissive_attachment,
        .metallic_roughness_occlusion_attachment = main_geometry_context.metallic_roughness_occlusion_attachment,
        .velocity_attachment = main_geometry_context.velocity_attachment,
      };
      self.decode_terrain(module, recording, terrain_decode_context);
      main_geometry_context.visbuffer_attachment = terrain_decode_context.visbuffer_attachment;
      main_geometry_context.depth_attachment = terrain_decode_context.depth_attachment;
      main_geometry_context.albedo_attachment = terrain_decode_context.albedo_attachment;
      main_geometry_context.normal_attachment = terrain_decode_context.normal_attachment;
      main_geometry_context.emissive_attachment = terrain_decode_context.emissive_attachment;
      main_geometry_context.metallic_roughness_occlusion_attachment = terrain_decode_context
                                                                        .metallic_roughness_occlusion_attachment;
      main_geometry_context.velocity_attachment = terrain_decode_context.velocity_attachment;

      // the maps the brush may have rewritten go back to rest for the next frame and the CPU readbacks
      const auto& maps = terrain_brush_context.maps;
      module.export_to(terrain_context.heightmap_attachment, TERRAIN_HEIGHT_REST);
      module.export_to(terrain_context.patch_minmax_attachment, TERRAIN_HEIGHT_REST);
      module.export_to(maps.ridgemap, TEXTURE_REST);
      module.export_to(terrain_decode_context.normalmap_attachment, TEXTURE_REST);
      module.export_to(terrain_decode_context.splatmap_attachment, TEXTURE_REST);
      module.export_to(maps.height_edit, TERRAIN_EDIT_REST);
      module.export_to(maps.splat_edit, TERRAIN_EDIT_REST);

      module.export_to(terrain_context.patch_visibility_mask_buffer, GPU_STATE_REST);
    }

    visbuffer_attachment = main_geometry_context.visbuffer_attachment;
    depth_attachment = main_geometry_context.depth_attachment;
    overdraw_attachment = main_geometry_context.overdraw_attachment;
    albedo_attachment = main_geometry_context.albedo_attachment;
    normal_attachment = main_geometry_context.normal_attachment;
    emissive_attachment = main_geometry_context.emissive_attachment;
    metallic_roughness_occlusion_attachment = main_geometry_context.metallic_roughness_occlusion_attachment;
    velocity_attachment = main_geometry_context.velocity_attachment;

    if ((shape.has_directional_shadows || shape.has_pointspot_shadows) && shape.has_meshes) {
      auto rmvsm_context = RMVSMContext{
        .depth_extent = render_extent,
        .depth_attachment = depth_attachment,
        .normal_attachment = normal_attachment,
        .virtual_page_table_attachment = vsm_virtual_page_table_attachment,
        .physical_page_table_attachment = vsm_physical_pages_attachment,
        .light_grid_buffer = light_grid_buffer,
        .pointspot_page_table_attachment = vsm_pointspot_page_table_attachment,
      };
      self.draw_virtual_shadowmap(module, recording, rmvsm_context);
      depth_attachment = rmvsm_context.depth_attachment;
      normal_attachment = rmvsm_context.normal_attachment;
      light_grid_buffer = rmvsm_context.light_grid_buffer;
      pointspot_views_buffer = rmvsm_context.pointspot_views_buffer;
      vsm_pointspot_page_table_attachment = rmvsm_context.pointspot_page_table_attachment;
      vsm_virtual_page_table_attachment = rmvsm_context.virtual_page_table_attachment;
      vsm_physical_pages_attachment = rmvsm_context.physical_page_table_attachment;
      vsm_directional_clipmaps_buffer = rmvsm_context.directional_clipmaps_buffer;

      if (shape.has_directional_shadows) {
        auto shadow_resolve_context = ShadowResolveContext{
          .directional_clipmaps_buffer = vsm_directional_clipmaps_buffer,
          .depth_attachment = depth_attachment,
          .normal_attachment = normal_attachment,
          .virtual_page_table_attachment = vsm_virtual_page_table_attachment,
          .physical_page_table_attachment = vsm_physical_pages_attachment,
          .shadows_attachment = shadows_attachment,
        };
        self.resolve_shadowmap(module, recording, shadow_resolve_context);
        depth_attachment = shadow_resolve_context.depth_attachment;
        normal_attachment = shadow_resolve_context.normal_attachment;
        shadows_attachment = shadow_resolve_context.shadows_attachment;
        vsm_virtual_page_table_attachment = shadow_resolve_context.virtual_page_table_attachment;
        vsm_directional_clipmaps_buffer = shadow_resolve_context.directional_clipmaps_buffer;
        vsm_physical_pages_attachment = shadow_resolve_context.physical_page_table_attachment;
      }
    }

    prepared.shadow.contact_push = module.declare_bytes_var("contact shadows push", sizeof(ContactShadowsPush));
    std::tie(shadows_attachment, depth_attachment, values.camera) = std::tuple_cat(
      module
        .begin_compute(
          {{shadows_attachment, vir::Access::ComputeRW},
           {depth_attachment, vir::Access::ComputeSampled},
           {values.camera, vir::Access::ComputeUniformRead}}
        )
        .with_name("contact shadows")
        .bind_compute_pipeline(render_context.pipeline("contact_shadows"))
        .bind_image(0, 0, depth_attachment)
        .bind_image(0, 1, shadows_attachment)
        .bind_buffer(0, 2, values.camera)
        .bind_sampler(0, 3, render_context.cached_sampler_handle(NearestSamplerClamped))
        .bind_sampler(0, 4, render_context.cached_sampler_handle(LinearSamplerClamped))
        .push_constants_from(prepared.shadow.contact_push)
        .dispatch_invocations_per_pixel(shadows_attachment)
        .end_compute()
    );
  }

  if (!shape.has_meshes && !shape.has_terrain) {
    record_visbuffer_stages(visbuffer_attachment, depth_attachment);
  }

  if (shape.has_atmosphere && shape.has_directional_light) {
    auto atmosphere_context = AtmosphereContext{
      .sky_transmittance_lut_attachment = sky_transmittance_lut_attachment,
      .sky_multiscatter_lut_attachment = sky_multiscatter_lut_attachment,
      .sky_view_lut_attachment = sky_view_lut_attachment,
      .sky_cubemap_attachment = sky_cubemap_attachment,
      .sky_aerial_perspective_lut_attachment = sky_aerial_perspective_attachment,
    };
    self.draw_atmosphere(module, recording, atmosphere_context);
    sky_transmittance_lut_attachment = atmosphere_context.sky_transmittance_lut_attachment;
    sky_multiscatter_lut_attachment = atmosphere_context.sky_multiscatter_lut_attachment;
    sky_view_lut_attachment = atmosphere_context.sky_view_lut_attachment;
    sky_cubemap_attachment = atmosphere_context.sky_cubemap_attachment;
    sky_aerial_perspective_attachment = atmosphere_context.sky_aerial_perspective_lut_attachment;
  }

  if (shape.has_rtao) {
    auto rtao_context = RTAOContext{
      .tlas_buffer = values.tlas,
      .normal_attachment = normal_attachment,
      .depth_attachment = depth_attachment,
      .ambient_occlusion_attachment = ambient_occlusion_attachment,
    };
    self.generate_rtao(module, recording, rtao_context);
    values.tlas = rtao_context.tlas_buffer;
    normal_attachment = rtao_context.normal_attachment;
    depth_attachment = rtao_context.depth_attachment;
    ambient_occlusion_attachment = rtao_context.ambient_occlusion_attachment;
  } else if (shape.has_gtao) {
    auto ao_context = AmbientOcclusionContext{
      .noise_attachment = hilbert_noise_attachment,
      .normal_attachment = normal_attachment,
      .depth_attachment = depth_attachment,
      .depth_differences_attachment = module.transient_image(
        {.extent = render_extent, .format = vir::Format::R32Uint, .name = "vbgtao depth differences"}
      ),
      .ambient_occlusion_attachment = ambient_occlusion_attachment,
    };
    self.generate_ambient_occlusion(module, recording, ao_context);
    hilbert_noise_attachment = ao_context.noise_attachment;
    normal_attachment = ao_context.normal_attachment;
    depth_attachment = ao_context.depth_attachment;
    ambient_occlusion_attachment = ao_context.ambient_occlusion_attachment;
  }

  // lighting reads the point and spot views even on a frame that ran no shadow pass
  if (pointspot_views_buffer == vir::ValueID::Invalid) {
    pointspot_views_buffer = module.transient_buffer({
      .size = RMVSMContext::POINT_SPOT_LAYER_COUNT * sizeof(GPU::VSMPointSpotView),
      .usage = TRANSIENT_BUFFER_USAGE,
      .location = vir::MemoryLocation::GpuOnly,
      .name = "vsm pointspot views",
    });
    pointspot_views_buffer = module.fill_buffer(pointspot_views_buffer, 0);
  }

  // --- DDGI tracing ---
  auto probe_volumes_buffer = vir::ValueID::Invalid;
  auto probe_states_buffer = vir::ValueID::Invalid;
  auto ddgi_irradiance_attachment = vir::ValueID::Invalid;
  auto ddgi_distance_attachment = vir::ValueID::Invalid;
  if (shape.probe_volume_count > 0) {
    prepared.probe_volumes = module.declare_buffer_var("probe volumes", vir::Access::None);
    probe_volumes_buffer = prepared.probe_volumes;
    prepared.ddgi.probe_states = module.declare_buffer_var("ddgi probe states", GPU_STATE_REST);
    prepared.ddgi.irradiance = module.declare_image_var("ddgi irradiance", vir::Format::RGBA16Sfloat, TEXTURE_REST);
    prepared.ddgi.distance = module.declare_image_var("ddgi distance", vir::Format::RG16Sfloat, TEXTURE_REST);
    probe_states_buffer = prepared.ddgi.probe_states;
    ddgi_irradiance_attachment = prepared.ddgi.irradiance;
    ddgi_distance_attachment = prepared.ddgi.distance;

    prepared.ddgi.is_history_invalid = module.declare_bool_var("ddgi history invalid", false);
    probe_states_buffer = module.set_condition(
      prepared.ddgi.is_history_invalid,
      [&](vir::Module& arm) { return arm.fill_buffer(probe_states_buffer, 0); },
      [&](vir::Module&) { return probe_states_buffer; }
    );

    if (shape.has_ddgi) {
      prepared.ddgi.probe_update_list = module.declare_buffer_var("ddgi probe update list", GPU_STATE_REST);
      auto ddgi_context = DDGIContext{
        .tlas_buffer = values.tlas,
        .probe_volumes_buffer = probe_volumes_buffer,
        .probe_states_buffer = probe_states_buffer,
        .probe_update_list_buffer = prepared.ddgi.probe_update_list,
        .light_grid_buffer = light_grid_buffer,
        .pointspot_views_buffer = pointspot_views_buffer,
        .sky_view_lut_attachment = sky_view_lut_attachment,
        .sky_transmittance_lut_attachment = sky_transmittance_lut_attachment,
        .pointspot_page_table_attachment = vsm_pointspot_page_table_attachment,
        .vsm_physical_pages_attachment = vsm_physical_pages_attachment,
        .irradiance_attachment = ddgi_irradiance_attachment,
        .distance_attachment = ddgi_distance_attachment,
      };
      self.record_ddgi(module, recording, ddgi_context);
      values.tlas = ddgi_context.tlas_buffer;
      probe_volumes_buffer = ddgi_context.probe_volumes_buffer;
      probe_states_buffer = ddgi_context.probe_states_buffer;
      light_grid_buffer = ddgi_context.light_grid_buffer;
      pointspot_views_buffer = ddgi_context.pointspot_views_buffer;
      sky_view_lut_attachment = ddgi_context.sky_view_lut_attachment;
      sky_transmittance_lut_attachment = ddgi_context.sky_transmittance_lut_attachment;
      vsm_pointspot_page_table_attachment = ddgi_context.pointspot_page_table_attachment;
      vsm_physical_pages_attachment = ddgi_context.vsm_physical_pages_attachment;
      ddgi_irradiance_attachment = ddgi_context.irradiance_attachment;
      ddgi_distance_attachment = ddgi_context.distance_attachment;
      module.export_to(ddgi_context.probe_update_list_buffer, GPU_STATE_REST);
    }
  }

  // --- lighting ---
  auto pbr_context = PBRContext{
    .sky_transmittance_lut_attachment = sky_transmittance_lut_attachment,
    .sky_aerial_perspective_lut_attachment = sky_aerial_perspective_attachment,
    .sky_view_lut_attachment = sky_view_lut_attachment,
    .sky_cubemap_attachment = sky_cubemap_attachment,
    .depth_attachment = depth_attachment,
    .albedo_attachment = albedo_attachment,
    .normal_attachment = normal_attachment,
    .emissive_attachment = emissive_attachment,
    .metallic_roughness_occlusion_attachment = metallic_roughness_occlusion_attachment,
    .ambient_occlusion_attachment = ambient_occlusion_attachment,
    .shadows_attachment = shadows_attachment,
    .light_grid_buffer = light_grid_buffer,
    .pointspot_views_buffer = pointspot_views_buffer,
    .pointspot_page_table_attachment = vsm_pointspot_page_table_attachment,
    .vsm_physical_pages_attachment = vsm_physical_pages_attachment,
    .vsm_page_table_attachment = vsm_virtual_page_table_attachment,
  };
  final_attachment = self.apply_pbr(module, recording, pbr_context, final_attachment);
  sky_transmittance_lut_attachment = pbr_context.sky_transmittance_lut_attachment;
  sky_cubemap_attachment = pbr_context.sky_cubemap_attachment;
  depth_attachment = pbr_context.depth_attachment;
  albedo_attachment = pbr_context.albedo_attachment;
  normal_attachment = pbr_context.normal_attachment;
  emissive_attachment = pbr_context.emissive_attachment;
  metallic_roughness_occlusion_attachment = pbr_context.metallic_roughness_occlusion_attachment;
  ambient_occlusion_attachment = pbr_context.ambient_occlusion_attachment;
  light_grid_buffer = pbr_context.light_grid_buffer;
  pointspot_views_buffer = pbr_context.pointspot_views_buffer;
  vsm_pointspot_page_table_attachment = pbr_context.pointspot_page_table_attachment;
  vsm_physical_pages_attachment = pbr_context.vsm_physical_pages_attachment;
  vsm_virtual_page_table_attachment = pbr_context.vsm_page_table_attachment;

  if (shape.has_ddgi) {
    auto ddgi_apply_context = DDGIApplyContext{
      .probe_volumes_buffer = probe_volumes_buffer,
      .probe_states_buffer = probe_states_buffer,
      .depth_attachment = depth_attachment,
      .albedo_attachment = albedo_attachment,
      .normal_attachment = normal_attachment,
      .metallic_roughness_occlusion_attachment = metallic_roughness_occlusion_attachment,
      .ambient_occlusion_attachment = ambient_occlusion_attachment,
      .irradiance_attachment = ddgi_irradiance_attachment,
      .distance_attachment = ddgi_distance_attachment,
    };
    final_attachment = self.apply_ddgi(module, recording, ddgi_apply_context, final_attachment);
    probe_volumes_buffer = ddgi_apply_context.probe_volumes_buffer;
    probe_states_buffer = ddgi_apply_context.probe_states_buffer;
    depth_attachment = ddgi_apply_context.depth_attachment;
    albedo_attachment = ddgi_apply_context.albedo_attachment;
    normal_attachment = ddgi_apply_context.normal_attachment;
    metallic_roughness_occlusion_attachment = ddgi_apply_context.metallic_roughness_occlusion_attachment;
    ambient_occlusion_attachment = ddgi_apply_context.ambient_occlusion_attachment;
    ddgi_irradiance_attachment = ddgi_apply_context.irradiance_attachment;
    ddgi_distance_attachment = ddgi_apply_context.distance_attachment;
  }

  // --- 2D ---
  if (shape.has_sprites) {
    prepared.sprites = module.declare_buffer_var("sprites", vir::Access::None);
    auto sprites = prepared.sprites;
    // the batches change every frame, so they draw from a callback over the queue
    const auto draw_batches = [&self](vir::Recorder& recorder) -> void {
      for (const auto& batch : self.render_queue_2d.batches) {
        if (batch.count < 1) {
          continue;
        }

        recorder.push_constants(batch.offset, 32).draw(batch.count * 6, 1);
      }
    };
    static constexpr auto sprite_depth = vir::DepthState{
      .is_test_enabled = true,
      .is_write_enabled = true,
      .compare_op = VK_COMPARE_OP_GREATER_OR_EQUAL,
    };

    std::tie(
      visbuffer_2d_attachment,
      depth_attachment,
      sprites,
      values.materials,
      values.camera,
      values.transforms_world
    ) = std::tuple_cat( //
      module
        .begin_rendering(
          {{visbuffer_2d_attachment, vir::Access::ColorWrite},
           {depth_attachment, vir::Access::DepthStencilRW},
           {sprites, vir::Access::VertexRead},
           {values.materials, vir::Access::VertexRead | vir::Access::FragmentRead},
           {values.camera, vir::Access::VertexRead},
           {values.transforms_world, vir::Access::VertexRead}}
        )
        .with_name("2d forward vis")
        .bind_graphics_pipeline(render_context.pipeline("2d_forward_vis"))
        .set_depth(sprite_depth)
        .set_viewport(0, vir::Rect2D::framebuffer())
        .set_scissor(0, vir::Rect2D::framebuffer())
        .push_constant_address(0, values.materials)
        .push_constant_address(8, values.camera)
        .push_constant_address(16, values.transforms_world)
        .push_constant_address(24, sprites)
        .record_from(module.declare_callback_var("2d forward vis batches", draw_batches))
        .end_rendering()
    );

    std::tie(
      final_attachment,
      reactive_mask_attachment,
      depth_attachment,
      std::ignore,
      values.materials,
      values.camera,
      values.transforms_world
    ) = std::tuple_cat( //
      module
        .begin_rendering(
          {{final_attachment, vir::Access::ColorRW},
           {reactive_mask_attachment, vir::Access::ColorRW},
           {depth_attachment, vir::Access::DepthStencilRW},
           {sprites, vir::Access::VertexRead},
           {values.materials, vir::Access::VertexRead | vir::Access::FragmentRead},
           {values.camera, vir::Access::VertexRead},
           {values.transforms_world, vir::Access::VertexRead}}
        )
        .with_name("2d forward")
        .bind_graphics_pipeline(render_context.pipeline("2d_forward"))
        .set_depth(sprite_depth)
        .set_color_blend(0, vir::BlendPreset::AlphaBlend)
        .set_color_blend(1, reactive_mask_blend_state())
        .set_viewport(0, vir::Rect2D::framebuffer())
        .set_scissor(0, vir::Rect2D::framebuffer())
        .push_constant_address(0, values.materials)
        .push_constant_address(8, values.camera)
        .push_constant_address(16, values.transforms_world)
        .push_constant_address(24, sprites)
        .record_from(module.declare_callback_var("2d forward batches", draw_batches))
        .end_rendering()
    );
  }

  {
    auto resources = SharedResources{};
    resources.image_resources = {
      {"final_attachment", final_attachment},
      {"visbuffer_attachment_2d", visbuffer_2d_attachment},
    };
    self.execute_stages_after(RenderStage::Forward2D, module, resources, prepared);
    visbuffer_2d_attachment = resources.image_resources.at("visbuffer_attachment_2d");
    final_attachment = resources.image_resources.at("final_attachment");
  }

  // --- particles ---
  if (shape.has_particles) {
    prepared.particle.emitters = module.declare_buffer_var("particle emitters", vir::Access::None);
    prepared.particle.programs = module.declare_buffer_var("particle programs", vir::Access::None);
    prepared.particle.constants = module.declare_buffer_var("particle constants", vir::Access::None);

    auto particle_context = ParticleContext{
      .emitters_buffer = prepared.particle.emitters,
      .program_buffer = prepared.particle.programs,
      .constants_buffer = prepared.particle.constants,
      .depth_attachment = depth_attachment,
      .reactive_mask_attachment = reactive_mask_attachment,
    };
    self.simulate_particles(module, recording, particle_context);
    final_attachment = self.draw_particles(module, recording, particle_context, final_attachment);
    depth_attachment = particle_context.depth_attachment;
    reactive_mask_attachment = particle_context.reactive_mask_attachment;
  }

  // --- DDGI probe debug ---
  if (shape.debug_view == GPU::DebugView::DDGIProbes && shape.probe_volume_count > 0) {
    auto ddgi_debug_context = DDGIDebugContext{
      .probe_volumes_buffer = probe_volumes_buffer,
      .probe_states_buffer = probe_states_buffer,
      .irradiance_attachment = ddgi_irradiance_attachment,
      .depth_attachment = depth_attachment,
    };
    final_attachment = draw_ddgi_probes(module, render_context, recording, ddgi_debug_context, final_attachment);
    depth_attachment = ddgi_debug_context.depth_attachment;
    probe_states_buffer = ddgi_debug_context.probe_states_buffer;
    ddgi_irradiance_attachment = ddgi_debug_context.irradiance_attachment;
  }

  if (shape.probe_volume_count > 0) {
    module.export_to(probe_states_buffer, GPU_STATE_REST);
    module.export_to(ddgi_irradiance_attachment, TEXTURE_REST);
    module.export_to(ddgi_distance_attachment, TEXTURE_REST);
  }

  // --- FSR3, between scene composition and post processing, so bloom and tonemap run on the
  // upscaled image ---
  if (is_upscaling) {
    auto fsr3_context = FSR3Context{
      .color_attachment = final_attachment,
      .depth_attachment = depth_attachment,
      .velocity_attachment = velocity_attachment,
      .reactive_mask_attachment = reactive_mask_attachment,
      .exposure_buffer = values.exposure,
    };
    final_attachment = self.apply_fsr3(module, recording, fsr3_context);
    depth_attachment = fsr3_context.depth_attachment;
    velocity_attachment = fsr3_context.velocity_attachment;
    reactive_mask_attachment = fsr3_context.reactive_mask_attachment;
    values.exposure = fsr3_context.exposure_buffer;
  }

  if (shape.has_fxaa) {
    auto fxaa_attachment = module.transient_image({.extent = render_extent, .format = hdr_format, .name = "fxaa"});
    fxaa_attachment = module.clear(fxaa_attachment, black);
    const auto inverse_screen_size = 1.0f / glm::vec2(render_extent.width, render_extent.height);
    final_attachment = //
      module
        .begin_rendering({{fxaa_attachment, vir::Access::ColorWrite}, {final_attachment, vir::Access::FragmentSampled}})
        .with_name("fxaa")
        .bind_graphics_pipeline(render_context.pipeline("fxaa"))
        .set_viewport(0, vir::Rect2D::framebuffer())
        .set_scissor(0, vir::Rect2D::framebuffer())
        .bind_image(0, 0, final_attachment)
        .bind_sampler(0, 1, render_context.cached_sampler_handle(LinearSamplerClamped))
        .push_constants(PushConstants(inverse_screen_size))
        .draw(3, 1)
        .end_rendering()[0];
  }

  // --- post processing ---
  const auto bloom_extent = shape.has_bloom ? vir::Extent3D{
                                                .width = std::max(display_extent.width / 2, 1u),
                                                .height = std::max(display_extent.height / 2, 1u),
                                                .depth = 1,
                                              }
                                            : vir::Extent3D{1, 1, 1};
  prepared.post_process.render_target = module.declare_image_var("render target", shape.target_format, TEXTURE_REST);
  auto bloom_upsampled_attachment = module.transient_image({
    .extent = bloom_extent,
    .format = vir::Format::B10G11R11UfloatPack32,
    .mip_levels = shape.has_bloom ? Texture::calculate_mip_count(bloom_extent) : 1,
    .name = "bloom upsampled",
  });
  bloom_upsampled_attachment = module.clear(bloom_upsampled_attachment, black);
  auto post_process_context = PostProcessContext{
    .extent = display_extent,
    .dst_attachment = prepared.post_process.render_target,
    .final_attachment = final_attachment,
    .bloom_upsampled_attachment = bloom_upsampled_attachment,
  };

  if (shape.has_eye_adaptation) {
    apply_eye_adaptation(module, render_context, recording, post_process_context);
  }

  if (shape.has_bloom) {
    apply_bloom(module, render_context, recording, post_process_context);
  }

  const auto render_target_root = module.resource_root(post_process_context.dst_attachment);
  auto dst_attachment = apply_tonemap(module, render_context, recording, post_process_context);
  // stages may hand back an image of their own, the frame always ends in the render target
  auto render_target = dst_attachment;

  // overlay passes in the PostProcessing stage bind depth as a real attachment next to the display
  // resolution result, and vulkan needs the extents to match
  if (is_upscaling) {
    auto upscaled_depth = module.transient_image(
      {.extent = display_extent, .format = vir::Format::D32Sfloat, .name = "depth display"}
    );
    upscaled_depth = module.clear(upscaled_depth, vir::ClearValue::depth(0.0f));
    depth_attachment = //
      module
        .begin_rendering(
          {{upscaled_depth, vir::Access::DepthStencilRW}, {depth_attachment, vir::Access::FragmentSampled}}
        )
        .with_name("depth upscale")
        .bind_graphics_pipeline(render_context.pipeline("depth_upscale"))
        .set_depth({.is_test_enabled = true, .is_write_enabled = true, .compare_op = VK_COMPARE_OP_ALWAYS})
        .set_viewport(0, vir::Rect2D::framebuffer())
        .set_scissor(0, vir::Rect2D::framebuffer())
        .bind_image(0, 0, depth_attachment)
        .bind_sampler(0, 1, render_context.cached_sampler_handle(NearestSamplerClamped))
        .draw(3, 1)
        .end_rendering()[0];
  }

  {
    auto resources = SharedResources{};
    resources.buffer_resources = {{"camera_buffer", values.camera}};
    resources.image_resources = {{"depth_attachment", depth_attachment}, {"result_attachment", dst_attachment}};
    self.execute_stages_after(RenderStage::PostProcessing, module, resources, prepared);
    values.camera = resources.buffer_resources.at("camera_buffer");
    depth_attachment = resources.image_resources.at("depth_attachment");
    dst_attachment = resources.image_resources.at("result_attachment");
    if (module.resource_root(dst_attachment) == render_target_root) {
      render_target = dst_attachment;
    }
  }

  if (shape.is_debugging && shape.debug_view != GPU::DebugView::DDGIProbes && shape.has_meshes) {
    if (vsm_directional_clipmaps_buffer == vir::ValueID::Invalid) {
      vsm_directional_clipmaps_buffer = module.transient_buffer({
        .size = RMVSMContext::MAX_DIRECTIONAL_CLIPMAP_COUNT * sizeof(GPU::VirtualClipmap),
        .usage = TRANSIENT_BUFFER_USAGE,
        .location = vir::MemoryLocation::GpuOnly,
        .name = "vsm directional clipmaps",
      });
      vsm_directional_clipmaps_buffer = module.fill_buffer(vsm_directional_clipmaps_buffer, 0);
    }
    auto debug_context = DebugContext{
      .vsm_clipmaps_buffer = vsm_directional_clipmaps_buffer,
      .visbuffer_attachment = visbuffer_attachment,
      .depth_attachment = depth_attachment,
      .overdraw_attachment = overdraw_attachment,
      .albedo_attachment = albedo_attachment,
      .normal_attachment = normal_attachment,
      .emissive_attachment = emissive_attachment,
      .metallic_roughness_occlusion_attachment = metallic_roughness_occlusion_attachment,
      .ambient_occlusion_attachment = ambient_occlusion_attachment,
      .vsm_page_table_attachment = vsm_virtual_page_table_attachment,
      .pointspot_views_buffer = pointspot_views_buffer,
      .light_grid_buffer = light_grid_buffer,
      .vsm_pointspot_page_table_attachment = vsm_pointspot_page_table_attachment,
    };
    dst_attachment = self.apply_debug_view(module, recording, debug_context, dst_attachment);
    vsm_virtual_page_table_attachment = debug_context.vsm_page_table_attachment;
    vsm_pointspot_page_table_attachment = debug_context.vsm_pointspot_page_table_attachment;
    if (module.resource_root(dst_attachment) == render_target_root) {
      render_target = dst_attachment;
    }
  }

  if (shape.has_debug_renderer) {
    dst_attachment = self.draw_debug_shapes(module, recording, depth_attachment, dst_attachment);
    if (module.resource_root(dst_attachment) == render_target_root) {
      render_target = dst_attachment;
    }
  }

  {
    // last, over everything the frame drew, like the scene's own UI
    auto resources = SharedResources{};
    resources.image_resources = {{"depth_attachment", depth_attachment}, {"result_attachment", dst_attachment}};
    self.execute_stages_after(RenderStage::FinalOutput, module, resources, prepared);
    dst_attachment = resources.image_resources.at("result_attachment");
    if (module.resource_root(dst_attachment) == render_target_root) {
      render_target = dst_attachment;
    }
  }

  if (module.resource_root(dst_attachment) != render_target_root) {
    dst_attachment = module.copy_image(dst_attachment, render_target);
  }

  // --- everything that outlives the frame goes back to rest ---
  module.export_to(dst_attachment, TEXTURE_REST);
  for (const auto texture : {
         sky_transmittance_lut_attachment,
         sky_multiscatter_lut_attachment,
         sky_cubemap_attachment,
         hilbert_noise_attachment,
         vsm_virtual_page_table_attachment,
         vsm_pointspot_page_table_attachment,
         vsm_physical_pages_attachment,
       }) {
    module.export_to(texture, TEXTURE_REST);
  }

  for (const auto buffer : {
         values.transforms_world,
         values.transforms_previous,
         values.meshes,
         values.blas_addresses,
         values.skinned_blas_addresses,
         values.skinning_transforms,
         values.lights,
         values.materials,
       }) {
    module.export_to(buffer, SCENE_BUFFER_REST);
  }

  for (const auto buffer : {
         values.mesh_instances,
         values.meshlet_instance_visibility_mask,
         values.exposure,
         values.skinned_vertices,
       }) {
    module.export_to(buffer, GPU_STATE_REST);
  }

  if (shape.has_tlas) {
    module.export_to(values.tlas, STRUCTURE_REST);
  }
}

auto RendererInstance::update(this RendererInstance& self, RendererInstanceUpdateInfo& info, const RendererCVar& cvar)
  -> void {
  ZoneScoped;

  auto& asset_man = App::mod<AssetManager>();
  auto& render_context = *self.renderer.render_context;

  self.gpu_scene_flags = {};
  reset_frame_data(self.frame_data);

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
        self.scene.debug_renderer
          .draw_frustum(proj, glm::vec4(0, 1, 0, 1), frozen_camera.near_clip, frozen_camera.far_clip);
      }

      if (c.active || !found_active_camera) {
        current_camera = c;
        found_active_camera = c.active;
      }
    });

  CameraComponent cam = freeze_culling ? frozen_camera : current_camera;

  self.camera_data = GPU::Camera{
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
      self.atmosphere.sky_view_lut_size = to_lut_size(self.sky_view_lut_extent);
      self.atmosphere.aerial_perspective_lut_size = to_lut_size(self.sky_aerial_perspective_lut_extent);
      self.atmosphere.transmittance_lut_size = to_lut_size(self.sky_transmittance_lut.get_extent());
      self.atmosphere.multiscattering_lut_size = to_lut_size(self.sky_multiscatter_lut.get_extent());
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
    self.frame_data.shadow_point_light_count,
    self.frame_data.moved_point_light_mask
  );
  const auto spot_slot_stats = assign_shadow_slots(
    pending_lights,
    GPU::LightKind::Spot,
    self.shadow_spot_slots,
    self.frame_data.shadow_spot_light_count,
    self.frame_data.moved_spot_light_mask
  );

  self.frame_data.shadow_slots_reassigned = point_slot_stats.reassigned + spot_slot_stats.reassigned;
  self.frame_data.shadow_slots_invalidated = point_slot_stats.invalidated + spot_slot_stats.invalidated;

  // force worst-case cache rebuilds for profiling
  if (cvar.cvar_vsm_force_invalidate.as_bool()) {
    self.frame_data.moved_point_light_mask = ~0_u64;
    self.frame_data.moved_spot_light_mask = ~0_u64;
    self.sun_direction_changed = true;
  }

  TracyPlot("vsm shadow slots reassigned", static_cast<i64>(self.frame_data.shadow_slots_reassigned));
  TracyPlot("vsm shadow slots invalidated", static_cast<i64>(self.frame_data.shadow_slots_invalidated));

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
        self.scene.debug_renderer.draw_aabb(
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

  auto& data = self.frame_data;
  prepare_transforms(
    render_context,
    info.gpu_transforms,
    info.dirty_transform_ids,
    self.transforms_world_buffer,
    self.transforms_previous_buffer,
    data
  );
  // anything loaded after Renderer::update ran this frame (every later module) would otherwise draw
  // with material indices the gpu buffer does not have yet
  self.renderer.sync_materials();

  {
    const auto lights_span = self.scene.lights.slots_unsafe();
    for (const auto& light : lights_span) {
      data.spot_light_count += static_cast<u32>(light.kind == GPU::LightKind::Spot);
      data.point_light_count += static_cast<u32>(light.kind == GPU::LightKind::Point);
    }

    ensure_buffer(render_context, self.lights_buffer, GPU::MAX_LIGHTS * sizeof(GPU::Light), "lights");
    const auto count = std::min<usize>(lights_span.size(), GPU::MAX_LIGHTS);
    const auto lights = lights_span.first(count);
    data.lights.assign(lights.begin(), lights.end());
  }

  // the arena has to exist before the instance array is patched with pointers into it, and the
  // instance array has to be uploaded after that patch
  data.skinned_vertex_total = info.skinned_vertex_total;
  {
    const auto positions_bytes = static_cast<usize>(info.skinned_vertex_total) * sizeof(glm::u16vec4);
    const auto normals_bytes = static_cast<usize>(info.skinned_vertex_total) * sizeof(u32);
    ensure_buffer(render_context, self.skinned_vertices_buffer, positions_bytes + normals_bytes, "skinned vertices");

    const auto arena_address = render_context.buffer(self.skinned_vertices_buffer).device_address;
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

    ensure_buffer(
      render_context,
      self.skinning_transforms_buffer,
      info.skinning_transforms.size_bytes(),
      "skinning transforms"
    );
    data.skinning_transforms.assign(info.skinning_transforms.begin(), info.skinning_transforms.end());

    data.skin_jobs.clear();
    data.skin_jobs.reserve(info.skinned_mesh_instances.size());
    for (const auto& skinned : info.skinned_mesh_instances) {
      if (skinned.vertex_count == 0 || skinned.bone_count == 0) {
        continue;
      }

      data.skin_jobs.emplace_back(
        GPU::SkinJob{
          .mesh_instance_index = skinned.gpu_instance_index,
          .vertex_offset = skinned.vertex_offset,
          .bone_offset = skinned.bone_offset,
          .vertex_count = skinned.vertex_count,
          .bone_count = skinned.bone_count,
        }
      );
    }

    // after the arena is final, because an entry's structure is built straight out of its slice.
    // Same answer the frame shape keys on, so the pool holds no memory while nothing traces
    if (info.skinned_vertex_total > 0 && self.tlas_has_consumer(cvar)) {
      self.skinned_blas_pool.sync(render_context, info.skinned_mesh_instances, arena_address);
    } else {
      self.skinned_blas_pool.reset(render_context);
    }
  }

  if (info.meshes_dirty || self.meshes_buffer == BufferID::Invalid) {
    ensure_buffer(render_context, self.meshes_buffer, info.gpu_meshes.size_bytes(), "meshes");
    data.meshes.assign(info.gpu_meshes.begin(), info.gpu_meshes.end());
  }

  // uploaded every frame rather than under `meshes_dirty`, matching how the scene refreshes it: the
  // TLAS build dereferences these as raw pointers, and it is 8 bytes per unique mesh
  ensure_buffer(
    render_context,
    self.blas_addresses_buffer,
    info.gpu_mesh_blas_addresses.size_bytes(),
    "blas addresses"
  );
  data.blas_addresses.assign(info.gpu_mesh_blas_addresses.begin(), info.gpu_mesh_blas_addresses.end());

  // an entry the pool has not actually built yet reads back as zero, which drops that instance out
  // of the TLAS rather than pointing the build at memory nothing has written. Since this runs before
  // the frame stages its builds, a freshly pooled structure publishes its address next frame
  data.skinned_instance_count = static_cast<u32>(info.skinned_mesh_instances.size());
  data.skinned_blas_addresses.assign(ox::max(info.skinned_mesh_instances.size(), 1_sz), 0_u64);
  for (usize i = 0; i < info.skinned_mesh_instances.size(); i++) {
    data.skinned_blas_addresses[i] = self.skinned_blas_pool.address_of(
      info.skinned_mesh_instances[i].mesh_instance_slot
    );
  }
  ensure_buffer(
    render_context,
    self.skinned_blas_addresses_buffer,
    std::span<const u64>(data.skinned_blas_addresses).size_bytes(),
    "skinned blas addresses"
  );

  if (info.mesh_instances_dirty || self.mesh_instances_buffer == BufferID::Invalid) {
    ensure_buffer(render_context, self.mesh_instances_buffer, info.gpu_mesh_instances.size_bytes(), "mesh instances");
    data.mesh_instances.assign(info.gpu_mesh_instances.begin(), info.gpu_mesh_instances.end());

    const auto visibility_mask_size = (info.max_meshlet_instance_count + 31) / 32 * sizeof(u32);
    ensure_buffer(
      render_context,
      self.meshlet_instance_visibility_mask_buffer,
      visibility_mask_size,
      "meshlet instance visibility mask"
    );
    data.is_meshlet_visibility_reset = true;
  }

  if (self.scene.terrain != nullptr && self.scene.terrain->is_baked()) {
    const auto patch_count = self.scene.terrain->patch_count.x * self.scene.terrain->patch_count.y;
    const auto is_reallocated = ensure_buffer(
      render_context,
      self.terrain_patch_visibility_mask_buffer,
      (patch_count + 31) / 32 * sizeof(u32),
      "terrain patch visibility mask"
    );
    if (is_reallocated || self.terrain_patch_visibility_patch_count != patch_count) {
      data.is_terrain_visibility_reset = true;
    }

    self.terrain_patch_visibility_patch_count = patch_count;
    data.terrain_visible_patches_size = static_cast<u64>(patch_count) * sizeof(u32);
  }

  data.mesh_instance_count = info.mesh_instance_count;
  data.max_meshlet_instance_count = info.max_meshlet_instance_count;
  data.use_mesh_shaders = render_context.use_mesh_shaders();
  data.meshlet_instances_size = static_cast<u64>(data.max_meshlet_instance_count) * sizeof(GPU::MeshletInstance);
  if (!data.use_mesh_shaders) {
    data.visible_meshlet_instances_size = static_cast<u64>(data.max_meshlet_instance_count) * sizeof(u32);
    data.reordered_indices_size = static_cast<u64>(data.max_meshlet_instance_count) *
                                  GPU::Mesh::MAX_MESHLET_PRIMITIVES * 3 * sizeof(u32);
  }

  data.dirty_mesh_instances.assign(info.dirty_mesh_instance_indices.begin(), info.dirty_mesh_instance_indices.end());
  data.removed_mesh_bounds.assign(info.removed_mesh_bounds.begin(), info.removed_mesh_bounds.end());

  auto& debug_renderer = self.scene.debug_renderer;
  if (!cvar.cvar_enable_debug_renderer.as_bool()) {
    debug_renderer.discard();
  } else {
    self.debug_vertices.clear();
    data.debug_draw_ranges = debug_renderer.flush(
      {.position = cam.position, .right = cam.right, .up = cam.up},
      self.debug_vertices
    );
  }

  self.update_vbgtao_info(cvar);

  if (cvar.cvar_particles_enable.as_bool()) {
    // a running scene's particles follow its gameplay clock so they pause with it, the editor
    // previews them in real time
    const auto particle_delta_time = self.scene.is_running()
                                       ? self.scene.last_step_delta
                                       : static_cast<f32>(App::get_timestep().get_millis()) * 0.001f;
    self.prepare_particles(particle_delta_time, cvar.cvar_particle_sort.as_bool());
  }

  if (ensure_buffer(render_context, self.exposure_buffer, sizeof(GPU::HistogramLuminance), "exposure")) {
    data.is_exposure_reset = true;
  }

  // stand-in for buffers a frame binds while nothing backs them, like the skinned arena of a scene
  // without skinned meshes
  ensure_buffer(render_context, self.placeholder_buffer, 16, "placeholder");

  // the structures are recorded from here so the frame only has to hand them to its program
  if (
    render_context.use_ray_tracing() && self.tlas_has_consumer(cvar) && data.mesh_instance_count > 0 &&
    self.scene_tlas.reserve(render_context, data.mesh_instance_count)
  ) {
    data.skinned_blas_builds = std::make_shared<const std::vector<vir::AccelerationStructureBuild>>(
      stage_skinned_blas_builds(
        render_context,
        self.skinned_blas_pool,
        SkinnedBLASBudget{
          .rebuild_primitive_budget = static_cast<u32>(ox::max(0, cvar.cvar_rt_skinned_rebuild_budget.get())),
          .max_refits_before_rebuild = static_cast<u32>(ox::max(0, cvar.cvar_rt_skinned_max_refits.get())),
        }
      )
    );
    data.tlas_builds = std::make_shared<const std::vector<vir::AccelerationStructureBuild>>(
      1,
      self.scene_tlas.build(render_context, data.mesh_instance_count)
    );
  }
}
} // namespace ox
