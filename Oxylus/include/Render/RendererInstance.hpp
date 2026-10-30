#pragma once

#include <ankerl/svector.h>
#include <ankerl/unordered_dense.h>
#include <array>
#include <bit>
#include <functional>
#include <glm/gtc/packing.hpp>
#include <memory>
#include <vir/Fwd.hpp>
#include <vir/Resource.hpp>
#include <vir/Types.hpp>

#include "Animation/Fwd.hpp"
#include "Asset/Texture.hpp"
#include "Render/AccelerationStructure.hpp"
#include "Render/DebugRenderer.hpp"
#include "Render/Fwd.hpp"
#include "Render/Renderer.hpp"
#include "Render/RendererCVar.hpp"
#include "Render/Upscaler.hpp"
#include "Scene/SceneGPU.hpp"
#include "Scene/Terrain.hpp"

namespace vir {
struct ColorBlendAttachmentState;
}

namespace ox {
struct RecordedFrame;

using StageVariables = ankerl::unordered_dense::map<std::string, vir::ValueID>;

enum class RenderStage {
  Initialization,
  Culling,
  VisBufferEncode,
  VisBufferDecode,
  Forward2D,
  Lighting,
  PostProcessing,
  Atmosphere,
  Debug,
  FinalOutput,
  Count,
};

enum class StagePosition {
  Before,
  After,
};

struct StageDependency {
  RenderStage target_stage;
  StagePosition position;
  int order = 0;
};

struct SharedResources {
  ankerl::unordered_dense::map<std::string, vir::ValueID> buffer_resources = {};
  ankerl::unordered_dense::map<std::string, vir::ValueID> image_resources = {};

  auto clear() -> void {
    buffer_resources.clear();
    image_resources.clear();
  }
};

// what an injected stage records into. Stages run once per frame shape, so anything that changes
// per frame has to reach the program through variables a stage declares into `variables` and fills
// in its bind function
struct RenderStageContext {
  RendererInstance& renderer_instance;
  vir::Module& module;
  SharedResources& shared_resources;
  RenderStage current_stage;
  RenderContext& render_context;

  glm::uvec2 viewport_size = {};
  // the extent of `result_attachment` from PostProcessing on, which the module can't report for the render target
  glm::uvec2 display_size = {};

  StageVariables& variables;

  ankerl::unordered_dense::map<std::string, vir::ValueID>& buffer_resources;
  ankerl::unordered_dense::map<std::string, vir::ValueID>& image_resources;

  auto get_buffer_resource(this const RenderStageContext& self, const std::string& name) -> vir::ValueID {
    return self.buffer_resources.at(name);
  }

  auto get_shared_buffer_resource(this const RenderStageContext& self, const std::string& name)
    -> option<vir::ValueID> {
    const auto it = self.shared_resources.buffer_resources.find(name);
    return it == self.shared_resources.buffer_resources.end() ? nullopt : option<vir::ValueID>(it->second);
  }

  auto get_image_resource(this const RenderStageContext& self, const std::string& name) -> vir::ValueID {
    return self.image_resources.at(name);
  }

  auto get_shared_image_resource(this const RenderStageContext& self, const std::string& name) -> option<vir::ValueID> {
    const auto it = self.shared_resources.image_resources.find(name);
    return it == self.shared_resources.image_resources.end() ? nullopt : option<vir::ValueID>(it->second);
  }

  auto set_buffer_resource(this RenderStageContext& self, const std::string& name, vir::ValueID value)
    -> RenderStageContext& {
    self.buffer_resources[name] = value;
    return self;
  }

  auto set_shared_buffer_resource(this RenderStageContext& self, const std::string& name, vir::ValueID value)
    -> RenderStageContext& {
    self.shared_resources.buffer_resources[name] = value;
    return self;
  }

  auto set_image_resource(this RenderStageContext& self, const std::string& name, vir::ValueID value)
    -> RenderStageContext& {
    self.image_resources[name] = value;
    return self;
  }

  auto set_shared_image_resource(this RenderStageContext& self, const std::string& name, vir::ValueID value)
    -> RenderStageContext& {
    self.shared_resources.image_resources[name] = value;
    return self;
  }
};

struct RenderQueue2D {
  std::vector<GPU::DrawBatch2D> batches = {};
  std::vector<GPU::SpriteGPUData> sprite_data = {};

  u32 num_sprites = 0;
  u32 previous_offset = 0;

  u32 last_batches_size = 0;
  u32 last_sprite_data_size = 0;

  void init() {
    clear();
    batches.reserve(last_batches_size);
    sprite_data.reserve(last_sprite_data_size);
    batches.emplace_back(GPU::DrawBatch2D{.offset = previous_offset, .count = 0});
  }

  void update() {
    if (!batches.empty()) {
      batches.back().count = num_sprites - batches.back().offset;
    }
    previous_offset = num_sprites;
  }

  void add(u16 render_flags, f32 position_y, u32 transform_id, u32 material_id, f32 distance) {
    const u32 flags_and_distance = math::pack_u16(render_flags, glm::packHalf1x16(distance));
    const u32 materialid_and_ypos = math::pack_u16(static_cast<u16>(material_id), glm::packHalf1x16(position_y));

    sprite_data.emplace_back(
      GPU::SpriteGPUData{
        .material_id16_ypos16 = materialid_and_ypos,
        .flags16_distance16 = flags_and_distance,
        .transform_id = transform_id,
      }
    );

    num_sprites += 1;
  }

  void sort() { std::ranges::sort(sprite_data, GPU::SpriteGreater()); }

  void clear() {
    num_sprites = 0;
    previous_offset = 0;
    last_batches_size = static_cast<u32>(batches.size());
    last_sprite_data_size = static_cast<u32>(sprite_data.size());

    batches.clear();
    sprite_data.clear();
  }
};

struct RenderStageCallback {
  std::function<void(RenderStageContext&)> callback;
  std::function<void(vir::Program&, const StageVariables&)> bind;
  StageDependency dependency;
  std::string name;
};

struct RendererInstanceUpdateInfo {
  u32 mesh_instance_count = 0;
  u32 max_meshlet_instance_count = 0;

  // the mesh and instance arrays are only rebuilt when the instance set changes, but skinned
  // instances carry per-frame vertex pointers and bounds, so their upload runs more often
  bool meshes_dirty = false;
  bool mesh_instances_dirty = false;

  std::span<GPU::TransformID> dirty_transform_ids = {};
  std::span<GPU::Transforms> gpu_transforms = {};

  std::span<GPU::Mesh> gpu_meshes = {};
  std::span<u64> gpu_mesh_blas_addresses = {};
  std::span<GPU::MeshInstance> gpu_mesh_instances = {};
  std::span<u32> dirty_mesh_instance_indices = {};

  std::span<const SkinnedMeshInstance> skinned_mesh_instances = {};
  std::span<GPU::SkinningTransform> skinning_transforms = {};
  u32 skinned_vertex_total = 0;

  std::span<GPU::MeshBounds> removed_mesh_bounds = {};
};

struct ParticleMeshDraw {
  u32 emitter_index = 0;
  GPU::Mesh gpu_mesh = {};
  vir::Buffer index_buffer = {};
  u32 index_count = 0;
};

struct TransformRun {
  u32 src = 0;
  u32 dst = 0;
  u32 count = 0;
};

// everything `update` prepares on the host for this frame. The recorded program stages it into
// buffers when it is updated, so nothing here refers to a buffer
struct FrameData {
  u32 mesh_instance_count = 0;
  u32 max_meshlet_instance_count = 0;
  bool use_mesh_shaders = false;

  u32 skinned_instance_count = 0;
  u32 skinned_vertex_total = 0;
  std::vector<GPU::SkinJob> skin_jobs = {};
  std::shared_ptr<const std::vector<vir::AccelerationStructureBuild>> skinned_blas_builds = {};
  std::shared_ptr<const std::vector<vir::AccelerationStructureBuild>> tlas_builds = {};

  // projected transforms packed per run, no runs means no upload
  std::vector<GPU::TransformWorld> transforms_world = {};
  std::vector<GPU::TransformPrevious> transforms_previous = {};
  std::vector<TransformRun> transform_runs = {};

  // meshes and instances are only filled when they changed
  std::vector<GPU::Mesh> meshes = {};
  std::vector<GPU::MeshInstance> mesh_instances = {};
  std::vector<u64> blas_addresses = {};
  // one entry per skinned instance rather than per mesh, because ten characters sharing a model
  // need ten structures. Renderer owned and uploaded every frame, so a moving address cannot
  // outlive a scene dirty flag that never fires
  std::vector<u64> skinned_blas_addresses = {};
  std::vector<GPU::SkinningTransform> skinning_transforms = {};
  std::vector<GPU::Light> lights = {};
  std::vector<u32> dirty_mesh_instances = {};
  std::vector<GPU::MeshBounds> removed_mesh_bounds = {};

  bool is_meshlet_visibility_reset = false;
  bool is_terrain_visibility_reset = false;
  bool is_exposure_reset = false;

  // sizes of the GPU only scratch the frame allocates
  u64 meshlet_instances_size = 0;
  u64 visible_meshlet_instances_size = 0;
  u64 reordered_indices_size = 0;
  u64 terrain_visible_patches_size = 0;

  u32 spot_light_count = 0;
  u32 point_light_count = 0;
  u32 shadow_point_light_count = 0;
  u32 shadow_spot_light_count = 0;
  u64 moved_point_light_mask = 0;
  u64 moved_spot_light_mask = 0;
  u32 shadow_slots_reassigned = 0;
  u32 shadow_slots_invalidated = 0;

  ankerl::svector<GPU::ParticleEmitter, 8> particle_emitters = {};
  std::vector<GPU::ParticleInstruction> particle_instructions = {};
  std::vector<glm::vec4> particle_constants = {};
  ankerl::svector<ParticleMeshDraw, 4> particle_mesh_draws = {};
  // one entry per `particle_mesh_draws`, never empty so the buffer always binds
  ankerl::svector<GPU::Mesh, 4> particle_meshes = {};
  ankerl::svector<u32, 4> particle_index_counts = {};
  u32 particle_total_spawn = 0;
  u32 particle_total_capacity = 0;
  u32 particle_sorted_count = 0;
  bool particle_pool_reset = false;
  bool particle_sort_enabled = false;
  // which alive list this frame reads, they swap every frame
  bool particle_ping = false;

  DebugRenderer::DrawRanges debug_draw_ranges = {};
};

// what changes how the frame is put together. A program is compiled per shape and reused for every
// frame that shares it, so anything that only changes what the passes compute stays out of here
struct FrameShape {
  glm::uvec2 display_size = {};
  glm::uvec2 render_size = {};
  UpscalerBackend upscaler = UpscalerBackend::None;
  GPU::DebugView debug_view = GPU::DebugView::None;
  vir::Format target_format = vir::Format::Undefined;
  // spec constants of several passes
  GPU::SceneFlags scene_flags = GPU::SceneFlags::None;
  GPU::TonemapType tonemap = GPU::TonemapType::AgX;
  VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
  bool is_debugging = false;
  bool has_meshes = false;
  bool has_terrain = false;
  bool has_sprites = false;
  bool has_particles = false;
  bool has_atmosphere = false;
  bool has_directional_light = false;
  bool has_directional_shadows = false;
  bool has_pointspot_shadows = false;
  bool has_bloom = false;
  bool has_fxaa = false;
  bool has_gtao = false;
  bool has_rtao = false;
  bool has_contact_shadows = false;
  bool has_eye_adaptation = false;
  bool has_ddgi = false;
  bool has_tlas = false;
  bool has_debug_renderer = false;
  bool use_mesh_shaders = false;
  bool is_transparent = false;
  bool is_particle_sort = false;
  // the bitonic sort is unrolled into one pass per step, so its size fixes the passes
  u32 particle_sorted_count = 0;
  // one trace and select pass per volume, and the ray data image is sized by the probes and rays
  u32 probe_volume_count = 0;
  u32 ddgi_probe_count = 0;
  u32 ddgi_rays_per_probe = 0;
  std::vector<std::string> stages = {};

  auto operator==(const FrameShape&) const -> bool = default;
};

struct FrameShapeHash {
  using is_avalanching = void;
  auto operator()(const FrameShape& shape) const noexcept -> u64;
};

struct FrameProgramStats {
  u32 cached_count = 0;
  u32 compile_count = 0;
  // compiles of a shape the cache had evicted, so more shapes alternate than it holds
  u32 recompile_count = 0;
  f32 last_compile_ms = 0.0f;
};

struct ShadowSlotState {
  u64 entity_id = 0;
  glm::vec3 position = {};
  glm::vec3 direction = {};
  f32 range = 0.0f;
  f32 outer_cone_angle = 0.0f;
};

// resources threaded through the recorded frame
struct FrameValues {
  vir::ValueID transforms_world = vir::ValueID::Invalid;
  vir::ValueID transforms_previous = vir::ValueID::Invalid;
  vir::ValueID meshes = vir::ValueID::Invalid;
  vir::ValueID blas_addresses = vir::ValueID::Invalid;
  vir::ValueID skinned_blas_addresses = vir::ValueID::Invalid;
  vir::ValueID mesh_instances = vir::ValueID::Invalid;
  vir::ValueID skinning_transforms = vir::ValueID::Invalid;
  vir::ValueID skinned_vertices = vir::ValueID::Invalid;
  vir::ValueID meshlet_instances = vir::ValueID::Invalid;
  vir::ValueID visible_meshlet_instances_indices = vir::ValueID::Invalid;
  vir::ValueID meshlet_instance_visibility_mask = vir::ValueID::Invalid;
  vir::ValueID reordered_indices = vir::ValueID::Invalid;
  vir::ValueID materials = vir::ValueID::Invalid;
  vir::ValueID camera = vir::ValueID::Invalid;
  vir::ValueID atmosphere = vir::ValueID::Invalid;
  vir::ValueID lights = vir::ValueID::Invalid;
  vir::ValueID exposure = vir::ValueID::Invalid;
  vir::ValueID dirty_mesh_instances = vir::ValueID::Invalid;
  vir::ValueID removed_mesh_bounds = vir::ValueID::Invalid;
  vir::ValueID terrain_patch_visibility_mask = vir::ValueID::Invalid;
  vir::ValueID tlas = vir::ValueID::Invalid;
};

struct CullGeometryContext {
  // When true, uses `cull_meshlets_hiz` (VisBuffer path with occlusion culling)
  bool use_hiz = false;
  // When true, uses `cull_meshlets_hpb` (VSM shadowmaps path with page culling)
  bool use_hpb = false;
  // When true, runs the `cull_meshes` pre-pass (allocates/zeroes the visibility
  // and dispatch buffers). Run once per sequence: visbuffer early pass or
  // shadowmap cascade 0; subsequent culls in the sequence set this to false.
  bool init_cull_meshes = false;

  GPU::CullFlag cull_flags = GPU::CullFlag::TestAll;
  vir::ValueID vsm_clipmaps_buffer = vir::ValueID::Invalid;
  vir::ValueID vsm_clipmap_dirty_flags_buffer = vir::ValueID::Invalid;

  // HiZ pyramid attachment consumed by `cull_meshlets_hiz` (only when `use_hiz`).
  vir::ValueID hiz_attachment = vir::ValueID::Invalid;

  // HPB pyramid attachment consumed by `cull_meshlets_hpb` (only when `use_hpb`)
  vir::ValueID hpb_attachment = vir::ValueID::Invalid;

  vir::ValueID visibility_buffer = vir::ValueID::Invalid;
  vir::ValueID cull_meshlets_cmd_buffer = vir::ValueID::Invalid;
  vir::ValueID draw_geometry_cmd_buffer = vir::ValueID::Invalid;
};

struct CullGeometryPointSpotContext {
  bool init = false;

  // the page table mip this context culls against, fixed per recorded pass
  u32 curr_mip = 0;

  vir::ValueID views_buffer = vir::ValueID::Invalid;
  vir::ValueID layer_dirty_mask_buffer = vir::ValueID::Invalid;
  vir::ValueID page_table_attachment = vir::ValueID::Invalid;
  vir::ValueID hpb_attachment = vir::ValueID::Invalid;

  vir::ValueID vsm_meshlet_instances_buffer = vir::ValueID::Invalid;
  vir::ValueID vsm_meshlet_instance_count_buffer = vir::ValueID::Invalid;
  vir::ValueID visible_indices_buffer = vir::ValueID::Invalid;
  vir::ValueID reordered_indices_buffer = vir::ValueID::Invalid;

  vir::ValueID draw_cmd_buffer = vir::ValueID::Invalid;
};

struct MainGeometryContext {
  bool draw_overdraw = false;

  GPU::CullFlag cull_flags = GPU::CullFlag::TestAll;

  vir::ValueID depth_attachment = vir::ValueID::Invalid;
  vir::ValueID hiz_attachment = vir::ValueID::Invalid;
  vir::ValueID visbuffer_attachment = vir::ValueID::Invalid;
  vir::ValueID overdraw_attachment = vir::ValueID::Invalid;
  vir::ValueID albedo_attachment = vir::ValueID::Invalid;
  vir::ValueID normal_attachment = vir::ValueID::Invalid;
  vir::ValueID emissive_attachment = vir::ValueID::Invalid;
  vir::ValueID metallic_roughness_occlusion_attachment = vir::ValueID::Invalid;
  vir::ValueID velocity_attachment = vir::ValueID::Invalid;

  vir::ValueID draw_geometry_cmd_buffer = vir::ValueID::Invalid;
  vir::ValueID visibility_buffer = vir::ValueID::Invalid;
};

struct LightGridContext {
  vir::ValueID light_grid_buffer = vir::ValueID::Invalid;
};

struct TerrainContext {
  GPU::CullFlag cull_flags = GPU::CullFlag::TestFrustum;

  vir::ValueID terrain_buffer = vir::ValueID::Invalid;
  // Compacted list of patch indices that survived culling, plus its indirect draw.
  vir::ValueID visible_patches_buffer = vir::ValueID::Invalid;
  vir::ValueID draw_cmd_buffer = vir::ValueID::Invalid;
  // One persistent bit per patch, so the early pass can draw last frame's visible set.
  vir::ValueID patch_visibility_mask_buffer = vir::ValueID::Invalid;

  vir::ValueID heightmap_attachment = vir::ValueID::Invalid;
  vir::ValueID patch_minmax_attachment = vir::ValueID::Invalid;
  vir::ValueID hiz_attachment = vir::ValueID::Invalid;
  vir::ValueID visbuffer_attachment = vir::ValueID::Invalid;
  vir::ValueID depth_attachment = vir::ValueID::Invalid;
};

struct TerrainBrushContext {
  TerrainMapValues maps = {};
  vir::ValueID hit_buffer = vir::ValueID::Invalid;
};

struct TerrainDecodeContext {
  vir::ValueID terrain_buffer = vir::ValueID::Invalid;
  vir::ValueID brush_hit_buffer = vir::ValueID::Invalid;
  vir::ValueID normalmap_attachment = vir::ValueID::Invalid;
  vir::ValueID splatmap_attachment = vir::ValueID::Invalid;

  vir::ValueID visbuffer_attachment = vir::ValueID::Invalid;
  vir::ValueID depth_attachment = vir::ValueID::Invalid;
  vir::ValueID albedo_attachment = vir::ValueID::Invalid;
  vir::ValueID normal_attachment = vir::ValueID::Invalid;
  vir::ValueID emissive_attachment = vir::ValueID::Invalid;
  vir::ValueID metallic_roughness_occlusion_attachment = vir::ValueID::Invalid;
  vir::ValueID velocity_attachment = vir::ValueID::Invalid;
};

struct RMVSMContext {
  constexpr static u32 PAGE_SIZE = 64;
  constexpr static u32 MAX_DIRECTIONAL_CLIPMAP_COUNT = 10;
  constexpr static u32 DIRECTIONAL_IMAGE_RESOLUTION = 1 << 12;
  constexpr static u32 DIRECTIONAL_PAGE_TABLE_SIZE = DIRECTIONAL_IMAGE_RESOLUTION / PAGE_SIZE;
  constexpr static u32 DIRECTIONAL_MAX_PAGE_COUNT = DIRECTIONAL_PAGE_TABLE_SIZE * DIRECTIONAL_PAGE_TABLE_SIZE;
  constexpr static u32 DIRECTIONAL_PAGE_MASK_COUNT = (DIRECTIONAL_MAX_PAGE_COUNT + 31) / 32;

  constexpr static u32 POINT_SPOT_IMAGE_RESOLUTION = 1 << 11;
  constexpr static u32 POINT_SPOT_PAGE_TABLE_SIZE = POINT_SPOT_IMAGE_RESOLUTION / PAGE_SIZE;
  constexpr static u32 POINT_SPOT_MIP_COUNT = 6;
  constexpr static u32 POINT_SPOT_HPB_LEVEL_COUNT = std::bit_width(POINT_SPOT_PAGE_TABLE_SIZE);
  constexpr static u32 POINT_SPOT_SPOT_LAYER_OFFSET = GPU::MAX_SHADOW_POINT_LIGHTS * 6;
  constexpr static u32 POINT_SPOT_LAYER_COUNT = POINT_SPOT_SPOT_LAYER_OFFSET + GPU::MAX_SHADOW_SPOT_LIGHTS;
  constexpr static u32 POINT_SPOT_MAX_MESHLET_INSTANCES = 1u << 18;
  constexpr static u32 POINT_SPOT_MAX_INDEX_COUNT = 1u << 23;
  // must match CULLING_MESH_COUNT in Shaders/defines.slang
  constexpr static u32 CULLING_MESH_COUNT = 64;

  constexpr static u32 PHYSICAL_PAGE_TABLE_SIZE = DIRECTIONAL_IMAGE_RESOLUTION + POINT_SPOT_IMAGE_RESOLUTION;
  constexpr static u32 PHYSICAL_PAGES_PER_DIM = PHYSICAL_PAGE_TABLE_SIZE / PAGE_SIZE;
  constexpr static u32 PHYSICAL_PAGE_COUNT = PHYSICAL_PAGES_PER_DIM * PHYSICAL_PAGES_PER_DIM;

  constexpr static u32 MAX_ALLOC_REQUEST_COUNT = 2 * PHYSICAL_PAGE_COUNT;
  constexpr static f32 POINT_LIGHT_FOV_DEG = 95.0f;
  constexpr static f32 POINT_LIGHT_NEAR = 0.05f;
  constexpr static f32 SPOT_LIGHT_NEAR = 0.05f;
  constexpr static f32 MAX_SHADOW_DISTANCE = 1000.0f;

  vir::Extent3D depth_extent = {};

  vir::ValueID directional_clipmaps_buffer = vir::ValueID::Invalid;

  vir::ValueID depth_attachment = vir::ValueID::Invalid;
  vir::ValueID normal_attachment = vir::ValueID::Invalid;
  vir::ValueID virtual_page_table_attachment = vir::ValueID::Invalid;
  vir::ValueID physical_page_table_attachment = vir::ValueID::Invalid;

  vir::ValueID light_grid_buffer = vir::ValueID::Invalid;
  vir::ValueID pointspot_views_buffer = vir::ValueID::Invalid;
  vir::ValueID pointspot_layer_dirty_mask_buffer = vir::ValueID::Invalid;
  vir::ValueID pointspot_page_table_attachment = vir::ValueID::Invalid;
};

auto bind_vsm_pointspot_spec_constants(vir::Module& module) -> vir::Module&;

template <typename Pass>
  requires requires(Pass pass) { *pass.module; }
auto bind_vsm_pointspot_spec_constants(Pass pass) -> Pass {
  bind_vsm_pointspot_spec_constants(*pass.module);
  return pass;
}

struct AtmosphereContext {
  vir::ValueID sky_transmittance_lut_attachment = vir::ValueID::Invalid;
  vir::ValueID sky_multiscatter_lut_attachment = vir::ValueID::Invalid;
  vir::ValueID sky_view_lut_attachment = vir::ValueID::Invalid;
  vir::ValueID sky_cubemap_attachment = vir::ValueID::Invalid;
  vir::ValueID sky_aerial_perspective_lut_attachment = vir::ValueID::Invalid;
};

struct AmbientOcclusionContext {
  vir::ValueID noise_attachment = vir::ValueID::Invalid;
  vir::ValueID normal_attachment = vir::ValueID::Invalid;
  vir::ValueID depth_attachment = vir::ValueID::Invalid;
  vir::ValueID depth_differences_attachment = vir::ValueID::Invalid;
  vir::ValueID ambient_occlusion_attachment = vir::ValueID::Invalid;
};

struct RTAOContext {
  vir::ValueID tlas_buffer = vir::ValueID::Invalid;
  vir::ValueID normal_attachment = vir::ValueID::Invalid;
  vir::ValueID depth_attachment = vir::ValueID::Invalid;
  vir::ValueID ambient_occlusion_attachment = vir::ValueID::Invalid;
};

struct PBRContext {
  vir::ValueID sky_transmittance_lut_attachment = vir::ValueID::Invalid;
  vir::ValueID sky_aerial_perspective_lut_attachment = vir::ValueID::Invalid;
  vir::ValueID sky_view_lut_attachment = vir::ValueID::Invalid;
  vir::ValueID sky_cubemap_attachment = vir::ValueID::Invalid;
  vir::ValueID depth_attachment = vir::ValueID::Invalid;
  vir::ValueID albedo_attachment = vir::ValueID::Invalid;
  vir::ValueID normal_attachment = vir::ValueID::Invalid;
  vir::ValueID emissive_attachment = vir::ValueID::Invalid;
  vir::ValueID metallic_roughness_occlusion_attachment = vir::ValueID::Invalid;
  vir::ValueID ambient_occlusion_attachment = vir::ValueID::Invalid;

  vir::ValueID light_grid_buffer = vir::ValueID::Invalid;

  vir::ValueID pointspot_views_buffer = vir::ValueID::Invalid;
  vir::ValueID pointspot_page_table_attachment = vir::ValueID::Invalid;
  vir::ValueID vsm_physical_pages_attachment = vir::ValueID::Invalid;
  vir::ValueID vsm_page_table_attachment = vir::ValueID::Invalid;
  vir::ValueID directional_clipmaps_buffer = vir::ValueID::Invalid;
};

struct ProbeVolumeKey {
  u64 entity = 0;
  u32 cascade_index = 0;

  auto operator==(const ProbeVolumeKey& other) const -> bool = default;
};

struct ProbeVolumeScroll {
  ProbeVolumeKey key = {};
  glm::ivec3 scroll = {};
};

struct DDGIContext {
  vir::ValueID tlas_buffer = vir::ValueID::Invalid;
  vir::ValueID probe_volumes_buffer = vir::ValueID::Invalid;
  vir::ValueID probe_states_buffer = vir::ValueID::Invalid;
  vir::ValueID probe_update_list_buffer = vir::ValueID::Invalid;
  vir::ValueID probe_update_args_buffer = vir::ValueID::Invalid;
  vir::ValueID light_grid_buffer = vir::ValueID::Invalid;
  vir::ValueID pointspot_views_buffer = vir::ValueID::Invalid;
  vir::ValueID sky_view_lut_attachment = vir::ValueID::Invalid;
  vir::ValueID sky_transmittance_lut_attachment = vir::ValueID::Invalid;
  vir::ValueID pointspot_page_table_attachment = vir::ValueID::Invalid;
  vir::ValueID vsm_physical_pages_attachment = vir::ValueID::Invalid;
  vir::ValueID ray_data_attachment = vir::ValueID::Invalid;
  vir::ValueID irradiance_attachment = vir::ValueID::Invalid;
  vir::ValueID distance_attachment = vir::ValueID::Invalid;
};

struct DDGIApplyContext {
  vir::ValueID probe_volumes_buffer = vir::ValueID::Invalid;
  vir::ValueID probe_states_buffer = vir::ValueID::Invalid;
  vir::ValueID depth_attachment = vir::ValueID::Invalid;
  vir::ValueID albedo_attachment = vir::ValueID::Invalid;
  vir::ValueID normal_attachment = vir::ValueID::Invalid;
  vir::ValueID metallic_roughness_occlusion_attachment = vir::ValueID::Invalid;
  vir::ValueID ambient_occlusion_attachment = vir::ValueID::Invalid;
  vir::ValueID irradiance_attachment = vir::ValueID::Invalid;
  vir::ValueID distance_attachment = vir::ValueID::Invalid;
};

struct DDGIDebugContext {
  vir::ValueID probe_volumes_buffer = vir::ValueID::Invalid;
  vir::ValueID probe_states_buffer = vir::ValueID::Invalid;
  vir::ValueID irradiance_attachment = vir::ValueID::Invalid;
  vir::ValueID depth_attachment = vir::ValueID::Invalid;
};

struct DebugContext {
  vir::ValueID vsm_clipmaps_buffer = vir::ValueID::Invalid;

  vir::ValueID visbuffer_attachment = vir::ValueID::Invalid;
  vir::ValueID depth_attachment = vir::ValueID::Invalid;
  vir::ValueID overdraw_attachment = vir::ValueID::Invalid;
  vir::ValueID albedo_attachment = vir::ValueID::Invalid;
  vir::ValueID normal_attachment = vir::ValueID::Invalid;
  vir::ValueID emissive_attachment = vir::ValueID::Invalid;
  vir::ValueID metallic_roughness_occlusion_attachment = vir::ValueID::Invalid;
  vir::ValueID ambient_occlusion_attachment = vir::ValueID::Invalid;
  vir::ValueID vsm_page_table_attachment = vir::ValueID::Invalid;

  vir::ValueID pointspot_views_buffer = vir::ValueID::Invalid;
  vir::ValueID light_grid_buffer = vir::ValueID::Invalid;
  vir::ValueID vsm_pointspot_page_table_attachment = vir::ValueID::Invalid;
};

// Reactivity is a coverage value in [0, 1], so overlapping alpha blended draws combine as
// `src + dst * (1 - src)` rather than summing. That saturates instead of running away, and treats
// two half covering layers as covering more than either alone, which is what actually happened.
//
// BlendOp::eMax would say "take the strongest layer" more directly, but it hangs the Metal Vulkan
// driver inside its NIR lowering, apparently while lowering max blending into the shader. Nothing
// else in the engine uses eMax, so that path had never been exercised.
auto reactive_mask_blend_state() -> vir::ColorBlendAttachmentState;

struct FSR3Context {
  vir::ValueID color_attachment = vir::ValueID::Invalid;
  vir::ValueID depth_attachment = vir::ValueID::Invalid;
  vir::ValueID velocity_attachment = vir::ValueID::Invalid;
  vir::ValueID reactive_mask_attachment = vir::ValueID::Invalid;
  vir::ValueID exposure_buffer = vir::ValueID::Invalid;

  vir::ValueID output_attachment = vir::ValueID::Invalid;
};

struct PostProcessContext {
  vir::Extent3D extent = {};

  vir::ValueID dst_attachment = vir::ValueID::Invalid;
  vir::ValueID final_attachment = vir::ValueID::Invalid;
  vir::ValueID bloom_upsampled_attachment = vir::ValueID::Invalid;
};

struct ParticleContext {
  // indirect commands for mesh emitters are laid out ahead of time, so their count is capped
  constexpr static u32 MAX_MESH_DRAWS = 64;

  vir::ValueID emitters_buffer = vir::ValueID::Invalid;
  vir::ValueID program_buffer = vir::ValueID::Invalid;
  vir::ValueID constants_buffer = vir::ValueID::Invalid;
  vir::ValueID depth_attachment = vir::ValueID::Invalid;
  // alpha blended particles have no usable motion vector, so they mark themselves reactive instead
  vir::ValueID reactive_mask_attachment = vir::ValueID::Invalid;

  vir::ValueID particles_buffer = vir::ValueID::Invalid;
  vir::ValueID sort_keys_buffer = vir::ValueID::Invalid;
  vir::ValueID draw_cmd_buffer = vir::ValueID::Invalid;
  vir::ValueID draw_indexed_cmd_buffer = vir::ValueID::Invalid;
};

// per-frame values the recorded passes read through their variables. `render` fills this before
// the program is updated, so every function registered while recording sees the current frame
struct UploadVariables {
  vir::ValueID staging = vir::ValueID::Invalid;
  vir::ValueID is_uploading = vir::ValueID::Invalid;
};

struct CullVariables {
  vir::ValueID mesh_instance_count = vir::ValueID::Invalid;
  vir::ValueID camera = vir::ValueID::Invalid;
  vir::ValueID shadow_camera = vir::ValueID::Invalid;
  vir::ValueID clipmap_push = vir::ValueID::Invalid;
  std::array<vir::ValueID, RMVSMContext::POINT_SPOT_MIP_COUNT> pointspot_contexts = {};
  std::array<vir::ValueID, RMVSMContext::POINT_SPOT_MIP_COUNT> pointspot_cull_meshes_commands = {};

  CullVariables() {
    pointspot_contexts.fill(vir::ValueID::Invalid);
    pointspot_cull_meshes_commands.fill(vir::ValueID::Invalid);
  }
};

struct LightGridVariables {
  vir::ValueID push = vir::ValueID::Invalid;
};

struct TerrainVariables {
  vir::ValueID data = vir::ValueID::Invalid;
  vir::ValueID cull_camera = vir::ValueID::Invalid;
  vir::ValueID patch_total = vir::ValueID::Invalid;
  vir::ValueID brush_params = vir::ValueID::Invalid;
  vir::ValueID brush_generate = vir::ValueID::Invalid;
  vir::ValueID brush_derive = vir::ValueID::Invalid;
  vir::ValueID brush_minmax = vir::ValueID::Invalid;
  vir::ValueID brush_apply_groups = vir::ValueID::Invalid;
  vir::ValueID brush_derive_groups = vir::ValueID::Invalid;
  vir::ValueID brush_minmax_groups = vir::ValueID::Invalid;
  vir::ValueID is_brush_active = vir::ValueID::Invalid;
  vir::ValueID is_brush_painting = vir::ValueID::Invalid;

  vir::ValueID heightmap = vir::ValueID::Invalid;
  vir::ValueID ridgemap = vir::ValueID::Invalid;
  vir::ValueID normalmap = vir::ValueID::Invalid;
  vir::ValueID splatmap = vir::ValueID::Invalid;
  vir::ValueID patch_minmax = vir::ValueID::Invalid;
  vir::ValueID height_edit = vir::ValueID::Invalid;
  vir::ValueID splat_edit = vir::ValueID::Invalid;
  vir::ValueID track = vir::ValueID::Invalid;
  vir::ValueID track_points = vir::ValueID::Invalid;
  vir::ValueID stamps = vir::ValueID::Invalid;
};

struct ShadowVariables {
  vir::ValueID context = vir::ValueID::Invalid;
  vir::ValueID pointspot_context = vir::ValueID::Invalid;
  vir::ValueID clipmap_context = vir::ValueID::Invalid;
  vir::ValueID directional_clipmaps = vir::ValueID::Invalid;
  vir::ValueID pointspot_views = vir::ValueID::Invalid;
  vir::ValueID clipmap_scroll_deltas = vir::ValueID::Invalid;
  vir::ValueID is_sun_moved = vir::ValueID::Invalid;
  vir::ValueID pointspot_moved_lights = vir::ValueID::Invalid;
  vir::ValueID pointspot_mark_push = vir::ValueID::Invalid;
  vir::ValueID pointspot_dirty_count = vir::ValueID::Invalid;
  vir::ValueID pointspot_removed_count = vir::ValueID::Invalid;
  vir::ValueID dirty_count = vir::ValueID::Invalid;
  vir::ValueID removed_count = vir::ValueID::Invalid;
};

struct PBRVariables {
  vir::ValueID is_luts_dirty = vir::ValueID::Invalid;
  vir::ValueID sky_view_push = vir::ValueID::Invalid;
  vir::ValueID sky_cubemap_push = vir::ValueID::Invalid;
  vir::ValueID aerial_push = vir::ValueID::Invalid;
  vir::ValueID vbgtao_prefilter_push = vir::ValueID::Invalid;
  vir::ValueID vbgtao_settings = vir::ValueID::Invalid;
  vir::ValueID vbgtao_denoise_push = vir::ValueID::Invalid;
  vir::ValueID rtao_push = vir::ValueID::Invalid;
  vir::ValueID lighting = vir::ValueID::Invalid;
  vir::ValueID sky = vir::ValueID::Invalid;
};

struct DDGIVolumeVariables {
  vir::ValueID select_push = vir::ValueID::Invalid;
  vir::ValueID select_probe_count = vir::ValueID::Invalid;
  vir::ValueID trace_push = vir::ValueID::Invalid;
  vir::ValueID trace_tail_push = vir::ValueID::Invalid;
  vir::ValueID trace_probe_count = vir::ValueID::Invalid;
  vir::ValueID debug_push = vir::ValueID::Invalid;
  vir::ValueID debug_probe_count = vir::ValueID::Invalid;
};

struct DDGIVariables {
  std::vector<DDGIVolumeVariables> volumes = {};
  vir::ValueID is_history_invalid = vir::ValueID::Invalid;
  vir::ValueID rays = vir::ValueID::Invalid;
  vir::ValueID relocate_push = vir::ValueID::Invalid;
  vir::ValueID irradiance_push = vir::ValueID::Invalid;
  vir::ValueID distance_push = vir::ValueID::Invalid;
  vir::ValueID apply_push = vir::ValueID::Invalid;

  vir::ValueID probe_states = vir::ValueID::Invalid;
  vir::ValueID probe_update_list = vir::ValueID::Invalid;
  vir::ValueID irradiance = vir::ValueID::Invalid;
  vir::ValueID distance = vir::ValueID::Invalid;
};

struct FSR3Variables {
  vir::ValueID constants = vir::ValueID::Invalid;
  vir::ValueID is_reset = vir::ValueID::Invalid;
  vir::ValueID accumulate_push = vir::ValueID::Invalid;
  vir::ValueID is_sharpening = vir::ValueID::Invalid;
  vir::ValueID sharpness = vir::ValueID::Invalid;
  vir::ValueID is_debugging = vir::ValueID::Invalid;
  vir::ValueID debug_mode = vir::ValueID::Invalid;

  vir::ValueID internal_upscaled_color = vir::ValueID::Invalid;
  vir::ValueID accumulation = vir::ValueID::Invalid;
  vir::ValueID current_luma = vir::ValueID::Invalid;
  vir::ValueID luma_history = vir::ValueID::Invalid;
  vir::ValueID internal_upscaled_color_prev = vir::ValueID::Invalid;
  vir::ValueID accumulation_prev = vir::ValueID::Invalid;
  vir::ValueID previous_luma = vir::ValueID::Invalid;
  vir::ValueID luma_history_prev = vir::ValueID::Invalid;
};

struct PostProcessVariables {
  vir::ValueID histogram_generate_push = vir::ValueID::Invalid;
  vir::ValueID histogram_average_push = vir::ValueID::Invalid;
  vir::ValueID bloom_prefilter_push = vir::ValueID::Invalid;
  vir::ValueID bloom_radius = vir::ValueID::Invalid;
  vir::ValueID tonemap_push = vir::ValueID::Invalid;
  vir::ValueID render_target = vir::ValueID::Invalid;
};

struct ParticleVariables {
  vir::ValueID emitters = vir::ValueID::Invalid;
  vir::ValueID programs = vir::ValueID::Invalid;
  vir::ValueID constants = vir::ValueID::Invalid;
  vir::ValueID index_counts = vir::ValueID::Invalid;
  vir::ValueID meshes = vir::ValueID::Invalid;

  vir::ValueID particles = vir::ValueID::Invalid;
  vir::ValueID dead_list = vir::ValueID::Invalid;
  vir::ValueID dead_counts = vir::ValueID::Invalid;
  vir::ValueID counters = vir::ValueID::Invalid;
  vir::ValueID sort_keys = vir::ValueID::Invalid;
  vir::ValueID alive_in = vir::ValueID::Invalid;
  vir::ValueID alive_out = vir::ValueID::Invalid;

  vir::ValueID total_capacity = vir::ValueID::Invalid;
  vir::ValueID total_spawn = vir::ValueID::Invalid;
  vir::ValueID is_pool_reset = vir::ValueID::Invalid;
  vir::ValueID init_push = vir::ValueID::Invalid;
  vir::ValueID emit_push = vir::ValueID::Invalid;
  vir::ValueID mesh_draw_push = vir::ValueID::Invalid;
};

struct DebugVariables {
  vir::ValueID pointspot_push = vir::ValueID::Invalid;
  vir::ValueID vsm_context = vir::ValueID::Invalid;
  vir::ValueID vertices = vir::ValueID::Invalid;
  // triangles then lines, each split by depth test
  std::array<vir::ValueID, 4> shape_counts = {};
  std::array<vir::ValueID, 4> shape_offsets = {};

  DebugVariables() {
    shape_counts.fill(vir::ValueID::Invalid);
    shape_offsets.fill(vir::ValueID::Invalid);
  }
};

// the variables one recorded program reads, `bind_frame` fills them before every run
struct PreparedFrame {
  vir::ValueID transforms_world = vir::ValueID::Invalid;
  vir::ValueID transforms_previous = vir::ValueID::Invalid;
  vir::ValueID meshes = vir::ValueID::Invalid;
  vir::ValueID blas_addresses = vir::ValueID::Invalid;
  vir::ValueID skinned_blas_addresses = vir::ValueID::Invalid;
  vir::ValueID mesh_instances = vir::ValueID::Invalid;
  vir::ValueID skinning_transforms = vir::ValueID::Invalid;
  vir::ValueID lights = vir::ValueID::Invalid;
  vir::ValueID meshlet_instance_visibility_mask = vir::ValueID::Invalid;
  vir::ValueID exposure = vir::ValueID::Invalid;
  vir::ValueID skinned_vertices = vir::ValueID::Invalid;
  vir::ValueID materials = vir::ValueID::Invalid;
  vir::ValueID terrain_patch_visibility_mask = vir::ValueID::Invalid;

  vir::ValueID transforms_world_staging = vir::ValueID::Invalid;
  vir::ValueID transforms_previous_staging = vir::ValueID::Invalid;
  UploadVariables meshes_upload = {};
  UploadVariables blas_addresses_upload = {};
  UploadVariables skinned_blas_addresses_upload = {};
  UploadVariables mesh_instances_upload = {};
  UploadVariables skinning_transforms_upload = {};
  UploadVariables lights_upload = {};
  vir::ValueID is_meshlet_visibility_reset = vir::ValueID::Invalid;
  vir::ValueID is_terrain_visibility_reset = vir::ValueID::Invalid;
  vir::ValueID is_exposure_reset = vir::ValueID::Invalid;

  vir::ValueID camera = vir::ValueID::Invalid;
  vir::ValueID atmosphere = vir::ValueID::Invalid;
  vir::ValueID dirty_mesh_instances = vir::ValueID::Invalid;
  vir::ValueID removed_mesh_bounds = vir::ValueID::Invalid;
  vir::ValueID probe_volumes = vir::ValueID::Invalid;
  vir::ValueID sprites = vir::ValueID::Invalid;

  vir::ValueID meshlet_instances = vir::ValueID::Invalid;
  vir::ValueID visible_meshlet_instances_indices = vir::ValueID::Invalid;
  vir::ValueID reordered_indices = vir::ValueID::Invalid;
  vir::ValueID terrain_visible_patches = vir::ValueID::Invalid;

  vir::ValueID skinned_blas_arena = vir::ValueID::Invalid;
  vir::ValueID skinned_blas_scratch = vir::ValueID::Invalid;
  vir::ValueID skinned_blas_builds = vir::ValueID::Invalid;
  vir::ValueID tlas_instances = vir::ValueID::Invalid;
  vir::ValueID tlas_instance_count = vir::ValueID::Invalid;
  vir::ValueID tlas_instance_counts = vir::ValueID::Invalid;
  vir::ValueID tlas = vir::ValueID::Invalid;
  vir::ValueID tlas_scratch = vir::ValueID::Invalid;
  vir::ValueID tlas_builds = vir::ValueID::Invalid;

  ankerl::unordered_dense::map<std::string, StageVariables> stage_variables = {};

  CullVariables cull = {};
  LightGridVariables light_grid = {};
  TerrainVariables terrain = {};
  ShadowVariables shadow = {};
  PBRVariables pbr = {};
  DDGIVariables ddgi = {};
  FSR3Variables fsr3 = {};
  PostProcessVariables post_process = {};
  ParticleVariables particle = {};
  DebugVariables debug = {};
};

// recording-local resource versions, separate from the cached variable IDs
struct FrameRecording {
  const FrameShape& shape;
  PreparedFrame& prepared;
  FrameValues values = {};
};

struct FrameParams {
  // the shape the current program was recorded for
  FrameShape shape = {};
  const RendererCVar* cvar = nullptr;
  f32 delta_time = 0.0f;
  u32 frame_index = 0;
  bool is_upscaling = false;
  glm::uvec2 render_size = {};
  glm::uvec2 display_size = {};
  GPU::CullCamera cull_camera = {};
  f32 bloom_intensity = 0.0f;

  // fsr3
  GPU::FSR3Constants fsr3_constants = {};
  f32 fsr3_sharpness = 0.0f;
  bool fsr3_reset = false;
  u32 fsr3_debug_view = 0;
  bool fsr3_history_ping = false;

  // ddgi
  u32 ddgi_total_probe_count = 0;
  u32 ddgi_rays_per_probe = 128;
  bool ddgi_history_valid = false;
  bool ddgi_distance_culling_changed = false;
  bool ddgi_distance_culling_enabled = true;
  ankerl::svector<glm::ivec3, 8> ddgi_scroll_deltas = {};

  // terrain
  bool is_terrain_brush_active = false;
  bool is_terrain_brush_painting = false;

  // virtual shadow maps
  bool vsm_sun_moved = false;
  GPU::VSMContext vsm = {};
  GPU::VSMPointSpotContext vsm_pointspot = {};
  std::array<GPU::VirtualClipmap, RMVSMContext::MAX_DIRECTIONAL_CLIPMAP_COUNT> vsm_directional_clipmaps = {};
  std::array<glm::ivec2, RMVSMContext::MAX_DIRECTIONAL_CLIPMAP_COUNT> vsm_clipmap_scroll_deltas = {};
  std::array<GPU::VSMPointSpotView, RMVSMContext::POINT_SPOT_LAYER_COUNT> vsm_pointspot_views = {};
};

class RendererInstance {
public:
  explicit RendererInstance(Scene& owner_scene, Renderer& parent_renderer);
  ~RendererInstance();

  RendererInstance(const RendererInstance&) = delete;
  RendererInstance& operator=(const RendererInstance&) = delete;
  RendererInstance(RendererInstance&&) = delete;
  RendererInstance& operator=(RendererInstance&&) = delete;

  auto add_stage_callback(this RendererInstance& self, RenderStageCallback callback) -> void;

  // a stage is recorded with the program and kept for as long as its name stays registered, so it
  // has to read per-frame data through the variables it declares, which `bind` sets before every run
  auto add_stage_before(
    this RendererInstance& self,
    RenderStage stage,
    const std::string& name,
    std::function<void(RenderStageContext&)> callback,
    std::function<void(vir::Program&, const StageVariables&)> bind = {},
    int order = 0
  ) -> void;

  auto add_stage_after(
    this RendererInstance& self,
    RenderStage stage,
    const std::string& name,
    std::function<void(RenderStageContext&)> callback,
    std::function<void(vir::Program&, const StageVariables&)> bind = {},
    int order = 0
  ) -> void;

  auto clear_stages(this RendererInstance& self) -> void;

  // renders into `target`, which the frame leaves at `vir::Access::FragmentSampled`
  auto render(
    this RendererInstance& self,
    const vir::ImageAttachment& target,
    glm::ivec2 viewport_origin,
    glm::ivec2 viewport_size,
    glm::ivec2 surface_size,
    const RendererCVar& cvar
  ) -> std::expected<vir::SubmitTicket, VkResult>;

  auto update(this RendererInstance& self, RendererInstanceUpdateInfo& info, const RendererCVar& cvar) -> void;

  auto get_viewport_size(this const RendererInstance& self) -> glm::uvec2 { return self.viewport_size_; }

  // resolution the scene is actually rasterized at; below the display size while an upscaler runs,
  // so anything indexing the visbuffer or g-buffer by pixel has to go through this
  auto get_render_size(this const RendererInstance& self) -> glm::uvec2 { return self.render_size_; }

  // the record half of every pass, run once per frame shape
  static auto record_upload(vir::Module& module, std::string_view name, vir::ValueID dst, UploadVariables& upload)
    -> vir::ValueID;
  static auto record_fill(
    vir::Module& module, std::string_view name, vir::ValueID dst, u32 value, vir::ValueID& is_filling
  ) -> vir::ValueID;
  auto bind_cull_variables(this const RendererInstance& self, vir::Program& program, const CullVariables& variables)
    -> void;
  auto bind_light_grid_variables(
    this const RendererInstance& self, vir::Program& program, const LightGridVariables& variables
  ) -> void;
  auto bind_terrain_variables(
    this const RendererInstance& self, vir::Program& program, const TerrainVariables& variables
  ) -> void;
  auto bind_shadow_variables(this const RendererInstance& self, vir::Program& program, const ShadowVariables& variables)
    -> void;

  static auto generate_hiz(vir::Module& module, RenderContext& render_context, MainGeometryContext& context) -> void;
  auto skin_vertices(this RendererInstance& self, vir::Module& module, FrameRecording& recording) -> void;
  static auto cull_geometry(
    vir::Module& module, RenderContext& render_context, FrameRecording& recording, CullGeometryContext& context
  ) -> void;
  static auto cull_geometry_pointspot(
    vir::Module& module, RenderContext& render_context, FrameRecording& recording, CullGeometryPointSpotContext& context
  ) -> void;
  static auto build_light_grid(
    vir::Module& module, RenderContext& render_context, FrameRecording& recording, LightGridContext& context
  ) -> void;
  auto apply_terrain_brush(
    this RendererInstance& self, vir::Module& module, FrameRecording& recording, TerrainBrushContext& context
  ) -> void;
  static auto cull_terrain(
    vir::Module& module, RenderContext& render_context, FrameRecording& recording, TerrainContext& context
  ) -> void;
  auto draw_terrain_for_visbuffer(
    this RendererInstance& self, vir::Module& module, FrameRecording& recording, TerrainContext& context
  ) -> void;
  auto decode_terrain(
    this RendererInstance& self, vir::Module& module, FrameRecording& recording, TerrainDecodeContext& context
  ) -> void;
  auto build_terrain_buffer(this RendererInstance& self, vir::Module& module, FrameRecording& recording)
    -> vir::ValueID;
  auto draw_for_visbuffer(
    this RendererInstance& self, vir::Module& module, FrameRecording& recording, MainGeometryContext& context
  ) -> void;
  auto draw_for_visbuffer_ms(
    this RendererInstance& self, vir::Module& module, FrameRecording& recording, MainGeometryContext& context
  ) -> void;
  auto decode_visbuffer(
    this RendererInstance& self, vir::Module& module, FrameRecording& recording, MainGeometryContext& context
  ) -> void;
  auto draw_virtual_shadowmap(
    this RendererInstance& self, vir::Module& module, FrameRecording& recording, RMVSMContext& context
  ) -> void;
  // the host side of the shadow maps, once per frame before the program updates
  auto prepare_virtual_shadowmap(this RendererInstance& self) -> void;
  auto bind_atmosphere_variables(
    this const RendererInstance& self, vir::Program& program, const PBRVariables& variables
  ) -> void;
  auto bind_ambient_occlusion_variables(
    this const RendererInstance& self, vir::Program& program, const PBRVariables& variables
  ) -> void;
  auto bind_rtao_variables(this const RendererInstance& self, vir::Program& program, const PBRVariables& variables)
    -> void;
  auto bind_pbr_variables(this const RendererInstance& self, vir::Program& program, const PBRVariables& variables)
    -> void;
  auto bind_ddgi_variables(this const RendererInstance& self, vir::Program& program, const DDGIVariables& variables)
    -> void;
  auto bind_fsr3_variables(this const RendererInstance& self, vir::Program& program, const FSR3Variables& variables)
    -> void;
  auto bind_eye_adaptation_variables(
    this const RendererInstance& self, vir::Program& program, const PostProcessVariables& variables
  ) -> void;
  auto bind_bloom_variables(
    this const RendererInstance& self, vir::Program& program, const PostProcessVariables& variables
  ) -> void;
  auto bind_tonemap_variables(
    this const RendererInstance& self, vir::Program& program, const PostProcessVariables& variables
  ) -> void;

  auto bind_particle_variables(
    this const RendererInstance& self, vir::Program& program, const ParticleVariables& variables
  ) -> void;
  auto bind_debug_variables(this const RendererInstance& self, vir::Program& program, const DebugVariables& variables)
    -> void;

  auto draw_atmosphere(
    this RendererInstance& self, vir::Module& module, FrameRecording& recording, AtmosphereContext& context
  ) -> void;
  auto generate_ambient_occlusion(
    this RendererInstance& self, vir::Module& module, FrameRecording& recording, AmbientOcclusionContext& context
  ) -> void;
  auto generate_rtao(this RendererInstance& self, vir::Module& module, FrameRecording& recording, RTAOContext& context)
    -> void;

  // mirrors the two sites that actually consume the TLAS, so both the pool and the build skip the
  // work when nothing traces
  auto tlas_has_consumer(this const RendererInstance& self, const RendererCVar& cvar) -> bool;
  // baked into the program, so it is part of the frame shape
  auto tlas_handle(this const RendererInstance& self) -> VkAccelerationStructureKHR;
  auto apply_pbr(
    this RendererInstance& self, vir::Module& module, FrameRecording& recording, PBRContext& context, vir::ValueID dst
  ) -> vir::ValueID;
  static auto apply_eye_adaptation(
    vir::Module& module, RenderContext& render_context, FrameRecording& recording, PostProcessContext& context
  ) -> void;
  static auto apply_bloom(
    vir::Module& module, RenderContext& render_context, FrameRecording& recording, PostProcessContext& context
  ) -> void;
  static auto apply_tonemap(
    vir::Module& module, RenderContext& render_context, FrameRecording& recording, PostProcessContext& context
  ) -> vir::ValueID;
  auto apply_debug_view(
    this RendererInstance& self, vir::Module& module, FrameRecording& recording, DebugContext& context, vir::ValueID dst
  ) -> vir::ValueID;
  auto allocate_ddgi_atlases(this RendererInstance& self, u32 probe_count) -> void;
  // the host side of the probes, once per frame before the program updates
  auto prepare_ddgi(this RendererInstance& self, const RendererCVar& cvar) -> void;
  auto record_ddgi(this RendererInstance& self, vir::Module& module, FrameRecording& recording, DDGIContext& context)
    -> void;
  auto apply_ddgi(
    this RendererInstance& self,
    vir::Module& module,
    FrameRecording& recording,
    DDGIApplyContext& context,
    vir::ValueID dst
  ) -> vir::ValueID;
  static auto draw_ddgi_probes(
    vir::Module& module,
    RenderContext& render_context,
    FrameRecording& recording,
    DDGIDebugContext& context,
    vir::ValueID dst
  ) -> vir::ValueID;
  auto draw_debug_shapes(
    this RendererInstance& self, vir::Module& module, FrameRecording& recording, vir::ValueID depth, vir::ValueID dst
  ) -> vir::ValueID;

  auto prepare_particles(this RendererInstance& self, f32 delta_time, bool sort_enabled) -> void;
  auto simulate_particles(
    this RendererInstance& self, vir::Module& module, FrameRecording& recording, ParticleContext& context
  ) -> void;
  auto draw_particles(
    this RendererInstance& self,
    vir::Module& module,
    FrameRecording& recording,
    ParticleContext& context,
    vir::ValueID dst
  ) -> vir::ValueID;

  auto update_vbgtao_info(this RendererInstance&, const RendererCVar& cvar) -> void;

  auto allocate_fsr3_resources(this RendererInstance& self, glm::uvec2 render_size, glm::uvec2 display_size) -> void;
  auto apply_fsr3(this RendererInstance& self, vir::Module& module, FrameRecording& recording, FSR3Context& context)
    -> vir::ValueID;
  static auto sharpen_fsr3(
    vir::Module& module,
    RenderContext& render_context,
    vir::ValueID enabled,
    vir::ValueID sharpness,
    vir::ValueID constants,
    vir::ValueID history,
    vir::ValueID exposure,
    vir::ValueID output
  ) -> std::array<vir::ValueID, 4>;

  FrameProgramStats program_stats = {};

  // read by passes and their variables, filled per frame
  FrameParams frame_params = {};
  FrameData frame_data = {};

private:
  SharedResources shared_resources = {};
  std::vector<RenderStageCallback> stage_callbacks;
  std::vector<std::vector<usize>> before_callbacks;
  std::vector<std::vector<usize>> after_callbacks;

  ankerl::unordered_dense::map<FrameShape, std::unique_ptr<RecordedFrame>, FrameShapeHash> programs = {};
  std::vector<FrameShape> program_order = {};
  ankerl::svector<u64, 8> evicted_shapes = {};
  const vir::ImageAttachment* render_target = nullptr;

  auto rebuild_execution_order(this RendererInstance& self) -> void;
  auto execute_stages_before(
    this RendererInstance& self,
    RenderStage stage,
    vir::Module& module,
    SharedResources& resources,
    PreparedFrame& prepared
  ) -> void;
  auto execute_stages_after(
    this RendererInstance& self,
    RenderStage stage,
    vir::Module& module,
    SharedResources& resources,
    PreparedFrame& prepared
  ) -> void;

  // settles the scene flags this frame renders with, so it runs once per frame before anything reads them
  auto frame_shape(
    this RendererInstance& self, glm::uvec2 display_size, vir::Format target_format, const RendererCVar& cvar
  ) -> FrameShape;
  auto record_acceleration_structures(this RendererInstance& self, vir::Module& module, FrameRecording& recording)
    -> void;
  auto bind_acceleration_structures(
    this const RendererInstance& self, vir::Program& program, const PreparedFrame& prepared
  ) -> void;
  auto prepare_frame_params(this RendererInstance& self, const FrameShape& shape, const RendererCVar& cvar) -> void;
  auto record_frame(this RendererInstance& self, vir::Module& module, const FrameShape& shape, PreparedFrame& prepared)
    -> void;
  auto bind_frame(this const RendererInstance& self, vir::Program& program, const PreparedFrame& prepared) -> void;
  auto bind_upload(
    this const RendererInstance& self,
    vir::Program& program,
    const UploadVariables& upload,
    std::span<const u8> data,
    std::string_view name
  ) -> void;
  auto program_for(this RendererInstance& self, const FrameShape& shape) -> RecordedFrame*;

  Scene& scene;
  Renderer& renderer;
  RenderQueue2D render_queue_2d = {};
  bool saved_camera = false;

  glm::uvec2 viewport_size_ = {};
  glm::uvec2 viewport_origin_ = {};
  glm::uvec2 surface_size_ = {};
  // resolution the scene is rasterized at, below the display size whenever an upscaler is active
  glm::uvec2 render_size_ = {};

  glm::vec2 current_jitter = {};
  glm::vec2 previous_jitter = {};
  u32 jitter_frame_index = 0;

  vir::Extent3D sky_view_lut_extent = {.width = 312, .height = 192, .depth = 1};
  vir::Extent3D sky_aerial_perspective_lut_extent = {.width = 32, .height = 32, .depth = 32};

  GPU::Camera camera_data = {};
  GPU::Camera previous_camera_data = {};

  GPU::SceneFlags gpu_scene_flags = {};

  GPU::TonemapType tonemap_type = GPU::TonemapType::AgX;

  bool directional_light_cast_shadows = true;
  bool sun_direction_changed = false;
  glm::vec3 light_grid_origin = {};
  glm::vec3 previous_sun_direction = {};
  GPU::DirectionalLight directional_light = {};
  f32 first_clipmap_width = 1.0f;
  f32 clipmap_selection_bias = 2.0f;
  ankerl::svector<GPU::ProbeVolume, 8> probe_volumes = {};
  ankerl::svector<ProbeVolumeKey, 8> probe_volume_keys = {};
  ankerl::svector<ProbeVolumeScroll, 8> probe_volume_scrolls = {};
  GPU::Atmosphere atmosphere = {};
  GPU::Atmosphere atmosphere_lut_state = {};
  bool atmosphere_lut_state_valid = false;
  bool atmosphere_luts_dirty = true;
  GPU::Sky sky_data = {};
  GPU::EyeAdaptationSettings eye_adaptation = {};
  GPU::VBGTAOSettings vbgtao_info = {};
  GPU::PostProcessSettings post_proces_settings = {};

  Texture sky_transmittance_lut = {};
  Texture sky_multiscatter_lut = {};
  Texture hilbert_noise_lut = {};
  Texture sky_cubemap = {};

  BufferID transforms_world_buffer = BufferID::Invalid;
  BufferID transforms_previous_buffer = BufferID::Invalid;
  BufferID mesh_instances_buffer = BufferID::Invalid;
  BufferID skinning_transforms_buffer = BufferID::Invalid;
  // in the same quantized format as the bind pose, so every downstream consumer works by pointer
  // swap alone
  BufferID skinned_vertices_buffer = BufferID::Invalid;
  BufferID meshes_buffer = BufferID::Invalid;
  BufferID blas_addresses_buffer = BufferID::Invalid;
  BufferID skinned_blas_addresses_buffer = BufferID::Invalid;
  SkinnedBLASPool skinned_blas_pool{};
  SceneTLAS scene_tlas{};
  // kept across frames to reuse its capacity
  std::vector<DebugRenderer::Vertex> debug_vertices = {};
  BufferID lights_buffer = BufferID::Invalid;
  BufferID meshlet_instance_visibility_mask_buffer = BufferID::Invalid;
  BufferID terrain_patch_visibility_mask_buffer = BufferID::Invalid;
  u32 terrain_patch_visibility_patch_count = 0;
  BufferID exposure_buffer = BufferID::Invalid;
  BufferID placeholder_buffer = BufferID::Invalid;

  BufferID particle_buffer = BufferID::Invalid;
  BufferID particle_alive_list_a = BufferID::Invalid;
  BufferID particle_alive_list_b = BufferID::Invalid;
  BufferID particle_dead_list = BufferID::Invalid;
  BufferID particle_dead_counts = BufferID::Invalid;
  BufferID particle_counters = BufferID::Invalid;
  BufferID particle_sort_keys = BufferID::Invalid;
  u32 particle_pool_capacity = 0;
  bool particle_pool_ping = false;

  u32 ddgi_atlas_probe_count = 0;
  u64 ddgi_atlas_layout_key = 0;
  bool ddgi_history_valid = false;
  bool ddgi_distance_culling_enabled = true;
  Texture ddgi_irradiance = {};
  Texture ddgi_distance = {};
  BufferID ddgi_probe_states = BufferID::Invalid;
  BufferID ddgi_probe_update_list = BufferID::Invalid;

  // FSR3 history. the ping-pong pairs alternate on `fsr3_history_ping` each frame; sizes are keyed
  // on `fsr3_render_size` / `fsr3_display_size` so a resize reallocates and forces a history reset
  glm::uvec2 fsr3_render_size = {};
  glm::uvec2 fsr3_display_size = {};
  bool fsr3_history_valid = false;
  bool fsr3_history_ping = false;
  u32 fsr3_frame_index = 0;
  glm::uvec2 fsr3_previous_render_size = {};
  glm::uvec2 fsr3_previous_display_size = {};
  glm::vec2 fsr3_previous_jitter = {};
  std::array<Texture, 2> fsr3_internal_upscaled_color{};
  std::array<Texture, 2> fsr3_accumulation{};
  std::array<Texture, 2> fsr3_luma{};
  std::array<Texture, 2> fsr3_luma_history{};

  Texture vsm_virtual_page_table = {};
  Texture vsm_pointspot_virtual_page_table = {};
  std::array<glm::ivec2, RMVSMContext::MAX_DIRECTIONAL_CLIPMAP_COUNT> previous_directional_clipmap_offsets = {};
  f32 previous_directional_clipmap_width = 0.0f;
  f32 previous_directional_shadow_distance = 0.0f;
  bool directional_vsm_cache_valid = false;
  std::array<ShadowSlotState, GPU::MAX_SHADOW_POINT_LIGHTS> shadow_point_slots = {};
  std::array<ShadowSlotState, GPU::MAX_SHADOW_SPOT_LIGHTS> shadow_spot_slots = {};
  // one image read as both float and uint, through views the program makes per binding
  Texture vsm_physical_page_table = {};
};
} // namespace ox
