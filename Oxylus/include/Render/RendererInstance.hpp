#pragma once

#include <ankerl/unordered_dense.h>
#include <array>

#include "Asset/Texture.hpp"
#include "Render/Renderer.hpp"
#include "Render/RendererCVar.hpp"
#include "Scene/SceneGPU.hpp"
#include "Scene/Terrain.hpp"

namespace vuk {
class CommandBuffer;
}

namespace ox {
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
  ankerl::unordered_dense::map<std::string, vuk::Value<vuk::Buffer>> buffer_resources = {};
  ankerl::unordered_dense::map<std::string, vuk::Value<vuk::ImageAttachment>> image_resources = {};

  auto clear() -> void {
    buffer_resources.clear();
    image_resources.clear();
  }
};

struct RenderStageContext {
  RendererInstance& renderer_instance;
  SharedResources& shared_resources;
  RenderStage current_stage;
  RenderContext& render_context;

  glm::uvec2 viewport_size;

  ankerl::unordered_dense::map<std::string, vuk::Value<vuk::Buffer>> buffer_resources = {};
  ankerl::unordered_dense::map<std::string, vuk::Value<vuk::ImageAttachment>> image_resources = {};

  RenderStageContext(RendererInstance& instance, SharedResources& shared_r, RenderStage stage, RenderContext& rctx)
      : renderer_instance(instance),
        shared_resources(shared_r),
        current_stage(stage),
        render_context(rctx) {}

  auto get_buffer_resource(this const RenderStageContext& self, const std::string& name) -> vuk::Value<vuk::Buffer> {
    return std::move(self.buffer_resources.at(name));
  }

  auto get_shared_buffer_resource(this const RenderStageContext& self, const std::string& name)
    -> option<vuk::Value<vuk::Buffer>> {
    const auto it = self.shared_resources.buffer_resources.find(name);
    if (it == self.shared_resources.buffer_resources.end()) {
      return nullopt;
    }

    return std::move(it->second);
  }

  auto get_image_resource(this const RenderStageContext& self, const std::string& name)
    -> vuk::Value<vuk::ImageAttachment> {
    return std::move(self.image_resources.at(name));
  }

  auto get_shared_image_resource(this const RenderStageContext& self, const std::string& name)
    -> option<vuk::Value<vuk::ImageAttachment>> {
    const auto it = self.shared_resources.image_resources.find(name);
    if (it == self.shared_resources.image_resources.end()) {
      return nullopt;
    }

    return std::move(it->second);
  }

  auto set_viewport_size(this RenderStageContext& self, glm::uvec2 size) -> RenderStageContext& {
    self.viewport_size = size;
    return self;
  }

  auto set_buffer_resource(this RenderStageContext& self, const std::string& name, vuk::Value<vuk::Buffer> value)
    -> RenderStageContext& {
    self.buffer_resources[name] = value;
    return self;
  }

  auto set_shared_buffer_resource(this RenderStageContext& self, const std::string& name, vuk::Value<vuk::Buffer> value)
    -> RenderStageContext& {
    self.shared_resources.buffer_resources[name] = value;
    return self;
  }

  auto set_image_resource(
    this RenderStageContext& self, const std::string& name, vuk::Value<vuk::ImageAttachment> value
  ) -> RenderStageContext& {
    self.image_resources[name] = value;
    return self;
  }

  auto set_shared_image_resource(
    this RenderStageContext& self, const std::string& name, vuk::Value<vuk::ImageAttachment> value
  ) -> RenderStageContext& {
    self.shared_resources.image_resources[name] = value;
    return self;
  }
};

struct RenderStageCallback {
  std::function<void(RenderStageContext&)> callback;
  StageDependency dependency;
  std::string name;
};

struct RendererInstanceUpdateInfo {
  u32 mesh_instance_count = 0;
  u32 max_meshlet_instance_count = 0;

  std::span<GPU::TransformID> dirty_transform_ids = {};
  std::span<GPU::Transforms> gpu_transforms = {};

  std::span<GPU::Mesh> gpu_meshes = {};
  std::span<GPU::MeshInstance> gpu_mesh_instances = {};
  std::span<u32> dirty_mesh_instance_indices = {};
};

struct PreparedFrame {
  u32 mesh_instance_count = 0;
  u32 max_meshlet_instance_count = 0;
  bool use_mesh_shaders = false;
  vuk::Value<vuk::Buffer> transforms_world_buffer = {};
  vuk::Value<vuk::Buffer> transforms_previous_buffer = {};
  vuk::Value<vuk::Buffer> meshes_buffer = {};
  vuk::Value<vuk::Buffer> mesh_instances_buffer = {};
  vuk::Value<vuk::Buffer> meshlet_instances_buffer = {};
  vuk::Value<vuk::Buffer> visible_meshlet_instances_indices_buffer = {};
  vuk::Value<vuk::Buffer> meshlet_instance_visibility_mask_buffer = {};
  vuk::Value<vuk::Buffer> reordered_indices_buffer = {};
  vuk::Value<vuk::Buffer> materials_buffer = {};
  vuk::Value<vuk::Buffer> camera_buffer = {};
  vuk::Value<vuk::Buffer> atmosphere_buffer = {};
  vuk::Value<vuk::Buffer> lights_buffer = {};
  vuk::Value<vuk::Buffer> exposure_buffer = {};

  u32 spot_light_count = 0;
  u32 point_light_count = 0;
  // High-water mark of occupied shadow slots, not a population count: slots are
  // held across frames so the occupied set can have holes. Consumers use it to
  // build a conservative "which slots may be live" mask.
  u32 shadow_point_light_count = 0;
  u32 shadow_spot_light_count = 0;
  u64 moved_point_light_mask = 0;
  u64 moved_spot_light_mask = 0;
  // Both counts are slots whose pages get fully reset this frame, so they drive
  // how much geometry the point/spot VSM chain has to re-render.
  u32 shadow_slots_reassigned = 0;
  u32 shadow_slots_invalidated = 0;

  vuk::Value<vuk::Buffer> dirty_mesh_instances_buffer = {};
  u32 dirty_mesh_instance_count = 0;

  vuk::Value<vuk::Buffer> terrain_patch_visibility_mask_buffer = {};

  u32 line_index_count = 0;
  u32 triangle_index_count = 0;
  vuk::Value<vuk::Buffer> debug_renderer_verticies_buffer = {};
};

// A point/spot shadow slot and the light that held it last frame. Slots persist
// across frames so cached VSM pages stay valid; the stored view parameters are
// what those pages were rendered with, and any mismatch (including a different
// occupant) invalidates the slot's layers.
struct ShadowSlotState {
  u64 entity_id = 0;
  glm::vec3 position = {};
  glm::vec3 direction = {};
  f32 range = 0.0f;
  f32 outer_cone_angle = 0.0f;
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
  GPU::CullCamera cull_camera = {};
  vuk::Value<vuk::Buffer> vsm_clipmaps_buffer = {};
  vuk::Value<vuk::Buffer> vsm_clipmap_dirty_flags_buffer = {};
  u32 vsm_clipmap_count = 0;

  // HiZ pyramid attachment consumed by `cull_meshlets_hiz` (only when `use_hiz`).
  vuk::Value<vuk::ImageAttachment> hiz_attachment = {};

  // HPB pyramid attachment consumed by `cull_meshlets_hpb` (only when `use_hpb`)
  vuk::Value<vuk::ImageAttachment> hpb_attachment = {};

  vuk::Value<vuk::Buffer> visibility_buffer = {};
  vuk::Value<vuk::Buffer> cull_meshlets_cmd_buffer = {};
  vuk::Value<vuk::Buffer> draw_geometry_cmd_buffer = {};
};

struct CullGeometryPointSpotContext {
  bool init = false;

  // curr_mip is set by the caller per iteration.
  GPU::VSMPointSpotContext ps_ctx = {};

  vuk::Value<vuk::Buffer> views_buffer = {};
  vuk::Value<vuk::Buffer> layer_dirty_mask_buffer = {};
  vuk::Value<vuk::ImageAttachment> page_table_attachment = {};
  vuk::Value<vuk::ImageAttachment> hpb_attachment = {};

  vuk::Value<vuk::Buffer> vsm_meshlet_instances_buffer = {};
  vuk::Value<vuk::Buffer> vsm_meshlet_instance_count_buffer = {};
  vuk::Value<vuk::Buffer> visible_indices_buffer = {};
  vuk::Value<vuk::Buffer> reordered_indices_buffer = {};

  vuk::Value<vuk::Buffer> draw_cmd_buffer = {};
};

struct MainGeometryContext {
  bool draw_overdraw = false;

  GPU::CullFlag cull_flags = GPU::CullFlag::TestAll;
  GPU::CullCamera cull_camera = {};

  vuk::PersistentDescriptorSet* bindless_set = nullptr;
  vuk::Value<vuk::ImageAttachment> depth_attachment = {};
  vuk::Value<vuk::ImageAttachment> hiz_attachment = {};
  vuk::Value<vuk::ImageAttachment> visbuffer_attachment = {};
  vuk::Value<vuk::ImageAttachment> overdraw_attachment = {};
  vuk::Value<vuk::ImageAttachment> albedo_attachment = {};
  vuk::Value<vuk::ImageAttachment> normal_attachment = {};
  vuk::Value<vuk::ImageAttachment> emissive_attachment = {};
  vuk::Value<vuk::ImageAttachment> metallic_roughness_occlusion_attachment = {};

  vuk::Value<vuk::Buffer> draw_geometry_cmd_buffer = {};
  vuk::Value<vuk::Buffer> visibility_buffer = {};
};

struct LightGridContext {
  glm::vec3 grid_origin = {};

  vuk::Value<vuk::Buffer> light_grid_buffer = {};
};

struct TerrainContext {
  const Terrain* terrain = nullptr;

  GPU::CullFlag cull_flags = GPU::CullFlag::TestFrustum;
  GPU::CullCamera cull_camera = {};

  vuk::Value<vuk::Buffer> terrain_buffer = {};
  // Compacted list of patch indices that survived culling, plus its indirect draw.
  vuk::Value<vuk::Buffer> visible_patches_buffer = {};
  vuk::Value<vuk::Buffer> draw_cmd_buffer = {};
  // One persistent bit per patch, so the early pass can draw last frame's visible set.
  vuk::Value<vuk::Buffer> patch_visibility_mask_buffer = {};

  vuk::Value<vuk::ImageAttachment> heightmap_attachment = {};
  vuk::Value<vuk::ImageAttachment> patch_minmax_attachment = {};
  vuk::Value<vuk::ImageAttachment> hiz_attachment = {};
  vuk::Value<vuk::ImageAttachment> visbuffer_attachment = {};
  vuk::Value<vuk::ImageAttachment> depth_attachment = {};
};

struct TerrainBrushContext {
  const Terrain* terrain = nullptr;

  TerrainMaps maps = {};
  vuk::Value<vuk::Buffer> hit_buffer = {};
};

struct TerrainDecodeContext {
  vuk::PersistentDescriptorSet* bindless_set = nullptr;

  vuk::Value<vuk::Buffer> terrain_buffer = {};
  vuk::Value<vuk::Buffer> brush_hit_buffer = {};
  vuk::Value<vuk::ImageAttachment> normalmap_attachment = {};
  vuk::Value<vuk::ImageAttachment> splatmap_attachment = {};

  vuk::Value<vuk::ImageAttachment> visbuffer_attachment = {};
  vuk::Value<vuk::ImageAttachment> depth_attachment = {};
  vuk::Value<vuk::ImageAttachment> albedo_attachment = {};
  vuk::Value<vuk::ImageAttachment> normal_attachment = {};
  vuk::Value<vuk::ImageAttachment> emissive_attachment = {};
  vuk::Value<vuk::ImageAttachment> metallic_roughness_occlusion_attachment = {};
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
  // Occupancy pyramid over a single page-table mip, so its base is the full
  // page table and it loses a level for every mip the base shrinks by.
  constexpr static u32 POINT_SPOT_HPB_LEVEL_COUNT = std::bit_width(POINT_SPOT_PAGE_TABLE_SIZE);
  constexpr static u32 POINT_SPOT_SPOT_LAYER_OFFSET = GPU::MAX_SHADOW_POINT_LIGHTS * 6;
  constexpr static u32 POINT_SPOT_LAYER_COUNT = POINT_SPOT_SPOT_LAYER_OFFSET + GPU::MAX_SHADOW_SPOT_LIGHTS;
  constexpr static u32 POINT_SPOT_MAX_MESHLET_INSTANCES = 1u << 18;
  constexpr static u32 POINT_SPOT_MAX_INDEX_COUNT = 1u << 23;
  // Must match CULLING_MESH_COUNT in Shaders/defines.slang; needed here because
  // `cull_meshes` is dispatched indirectly and has to size its own groups.
  constexpr static u32 CULLING_MESH_COUNT = 64;

  constexpr static u32 PHYSICAL_PAGE_TABLE_SIZE = DIRECTIONAL_IMAGE_RESOLUTION + POINT_SPOT_IMAGE_RESOLUTION;
  constexpr static u32 PHYSICAL_PAGES_PER_DIM = PHYSICAL_PAGE_TABLE_SIZE / PAGE_SIZE;
  constexpr static u32 PHYSICAL_PAGE_COUNT = PHYSICAL_PAGES_PER_DIM * PHYSICAL_PAGES_PER_DIM;

  constexpr static u32 MAX_ALLOC_REQUEST_COUNT = 2 * PHYSICAL_PAGE_COUNT;
  constexpr static f32 POINT_LIGHT_FOV_DEG = 95.0f;
  constexpr static f32 POINT_LIGHT_NEAR = 0.05f;
  constexpr static f32 SPOT_LIGHT_NEAR = 0.05f;

  vuk::PersistentDescriptorSet* bindless_set = nullptr;
  bool sun_moved = false;
  vuk::Extent3D depth_extent = {};
  f32 max_shadow_dist = 1000.0f;

  vuk::Value<vuk::Buffer> directional_clipmaps_buffer = {};

  vuk::Value<vuk::ImageAttachment> depth_attachment = {};
  vuk::Value<vuk::ImageAttachment> virtual_page_table_attachment = {};
  vuk::Value<vuk::ImageAttachment> physical_page_table_attachment = {};

  glm::vec3 light_grid_origin = {};
  vuk::Value<vuk::Buffer> light_grid_buffer = {};
  vuk::Value<vuk::Buffer> pointspot_views_buffer = {};
  vuk::Value<vuk::Buffer> pointspot_layer_dirty_mask_buffer = {};
  vuk::Value<vuk::ImageAttachment> pointspot_page_table_attachment = {};
};

// Bakes the point/spot virtualization sizes (specialization constants 50-54, see
// rmvsm.slang) into the currently-bound pipeline from RMVSMContext. Shared by
// every point/spot pass so the values are driven from a single source.
auto bind_vsm_pointspot_spec_constants(vuk::CommandBuffer& cmd) -> vuk::CommandBuffer&;

struct ShadowResolveContext {
  // TODO: Add shadowmap kind enum
  f32 max_shadow_dist = 1000.0f;

  vuk::Value<vuk::Buffer> directional_clipmaps_buffer = {};
  vuk::Value<vuk::ImageAttachment> depth_attachment = {};
  vuk::Value<vuk::ImageAttachment> normal_attachment = {};
  vuk::Value<vuk::ImageAttachment> virtual_page_table_attachment = {};
  vuk::Value<vuk::ImageAttachment> physical_page_table_attachment = {};

  vuk::Value<vuk::ImageAttachment> resolved_shadows_attachment = {};
};

struct AtmosphereContext {
  vuk::Value<vuk::ImageAttachment> sky_transmittance_lut_attachment = {};
  vuk::Value<vuk::ImageAttachment> sky_multiscatter_lut_attachment = {};
  vuk::Value<vuk::ImageAttachment> sky_view_lut_attachment = {};
  vuk::Value<vuk::ImageAttachment> sky_cubemap_attachment = {};
  vuk::Value<vuk::ImageAttachment> sky_aerial_perspective_lut_attachment = {};
};

struct AmbientOcclusionContext {
  vuk::Value<vuk::ImageAttachment> noise_attachment = {};
  vuk::Value<vuk::ImageAttachment> normal_attachment = {};
  vuk::Value<vuk::ImageAttachment> depth_attachment = {};
  vuk::Value<vuk::ImageAttachment> depth_differences_attachment = {};
  vuk::Value<vuk::ImageAttachment> ambient_occlusion_attachment = {};
};

struct PBRContext {
  vuk::PersistentDescriptorSet* bindless_set = nullptr;

  vuk::Value<vuk::ImageAttachment> sky_transmittance_lut_attachment = {};
  vuk::Value<vuk::ImageAttachment> sky_aerial_perspective_lut_attachment = {};
  vuk::Value<vuk::ImageAttachment> sky_view_lut_attachment = {};
  vuk::Value<vuk::ImageAttachment> sky_cubemap_attachment = {};
  vuk::Value<vuk::ImageAttachment> depth_attachment = {};
  vuk::Value<vuk::ImageAttachment> albedo_attachment = {};
  vuk::Value<vuk::ImageAttachment> normal_attachment = {};
  vuk::Value<vuk::ImageAttachment> emissive_attachment = {};
  vuk::Value<vuk::ImageAttachment> metallic_roughness_occlusion_attachment = {};
  vuk::Value<vuk::ImageAttachment> ambient_occlusion_attachment = {};
  vuk::Value<vuk::ImageAttachment> contact_shadows_attachment = {};
  vuk::Value<vuk::ImageAttachment> resolved_shadows_attachment = {};

  glm::vec3 light_grid_origin = {};
  vuk::Value<vuk::Buffer> light_grid_buffer = {};

  vuk::Value<vuk::Buffer> pointspot_views_buffer = {};
  vuk::Value<vuk::ImageAttachment> pointspot_page_table_attachment = {};
  vuk::Value<vuk::ImageAttachment> vsm_physical_pages_attachment = {};
  vuk::Value<vuk::ImageAttachment> vsm_page_table_attachment = {};
};

struct DebugContext {
  f32 overdraw_heatmap_scale = 0.0f;
  GPU::DebugView debug_view = GPU::DebugView::None;

  vuk::Value<vuk::Buffer> vsm_clipmaps_buffer = {};

  vuk::Value<vuk::ImageAttachment> visbuffer_attachment = {};
  vuk::Value<vuk::ImageAttachment> depth_attachment = {};
  vuk::Value<vuk::ImageAttachment> overdraw_attachment = {};
  vuk::Value<vuk::ImageAttachment> albedo_attachment = {};
  vuk::Value<vuk::ImageAttachment> normal_attachment = {};
  vuk::Value<vuk::ImageAttachment> emissive_attachment = {};
  vuk::Value<vuk::ImageAttachment> metallic_roughness_occlusion_attachment = {};
  vuk::Value<vuk::ImageAttachment> ambient_occlusion_attachment = {};
  vuk::Value<vuk::ImageAttachment> vsm_page_table_attachment = {};

  glm::vec3 light_grid_origin = {};
  vuk::Value<vuk::Buffer> pointspot_views_buffer = {};
  vuk::Value<vuk::Buffer> light_grid_buffer = {};
  vuk::Value<vuk::ImageAttachment> vsm_pointspot_page_table_attachment = {};
};

struct PostProcessContext {
  f32 delta_time = 0.0f;
  vuk::Extent3D extent = {};
  f32 bloom_intensity = 0.0f;

  vuk::Value<vuk::ImageAttachment> dst_attachment = {};
  vuk::Value<vuk::ImageAttachment> final_attachment = {};
  vuk::Value<vuk::ImageAttachment> bloom_upsampled_attachment = {};
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

  auto add_stage_before(
    this RendererInstance& self,
    RenderStage stage,
    const std::string& name,
    std::function<void(RenderStageContext&)> callback,
    int order = 0
  ) -> void;

  auto add_stage_after(
    this RendererInstance& self,
    RenderStage stage,
    const std::string& name,
    std::function<void(RenderStageContext&)> callback,
    int order = 0
  ) -> void;

  auto clear_stages(this RendererInstance& self) -> void;

  auto render(
    this RendererInstance& self,
    vuk::Value<vuk::ImageAttachment>&& dst_attachment,
    glm::ivec2 viewport_origin,
    glm::ivec2 viewport_size,
    glm::ivec2 surface_size,
    const RendererCVar& cvar
  ) -> vuk::Value<vuk::ImageAttachment>;

  auto update(this RendererInstance& self, RendererInstanceUpdateInfo& info, const RendererCVar& cvar) -> void;

  auto get_viewport_size(this const RendererInstance& self) -> glm::uvec2 { return self.viewport_size_; }

  auto generate_hiz(this RendererInstance&, MainGeometryContext& context) -> void;
  auto cull_geometry(this RendererInstance& self, CullGeometryContext& context) -> void;
  auto cull_geometry_pointspot(this RendererInstance& self, CullGeometryPointSpotContext& context) -> void;
  auto build_light_grid(this RendererInstance&, LightGridContext& context) -> void;
  auto apply_terrain_brush(this RendererInstance& self, TerrainBrushContext& context) -> void;
  auto cull_terrain(this RendererInstance& self, TerrainContext& context) -> void;
  auto draw_terrain_for_visbuffer(this RendererInstance& self, TerrainContext& context) -> void;
  auto decode_terrain(this RendererInstance& self, TerrainDecodeContext& context) -> void;
  auto build_terrain_buffer(this RendererInstance& self, const Terrain& terrain) -> vuk::Value<vuk::Buffer>;
  auto draw_for_visbuffer(this RendererInstance&, MainGeometryContext& context) -> void;
  auto draw_for_visbuffer_ms(this RendererInstance&, MainGeometryContext& context) -> void;
  auto decode_visbuffer(this RendererInstance&, MainGeometryContext& context) -> void;
  auto draw_virtual_shadowmap(this RendererInstance&, RMVSMContext& context) -> void;
  auto resolve_shadowmap(this RendererInstance&, ShadowResolveContext& context) -> void;
  auto draw_atmosphere(this RendererInstance&, AtmosphereContext& context) -> void;
  auto generate_ambient_occlusion(this RendererInstance&, AmbientOcclusionContext& context) -> void;
  auto apply_pbr(this RendererInstance&, PBRContext& context, vuk::Value<vuk::ImageAttachment>&& dst_attachment)
    -> vuk::Value<vuk::ImageAttachment>;
  auto apply_eye_adaptation(this RendererInstance&, PostProcessContext& context) -> void;
  auto apply_bloom(this RendererInstance& self, PostProcessContext& context, const RendererCVar& cvar) -> void;
  auto apply_tonemap(this RendererInstance&, PostProcessContext& context) -> vuk::Value<vuk::ImageAttachment>;
  auto apply_debug_view(this RendererInstance&, DebugContext& context, vuk::Extent3D extent)
    -> vuk::Value<vuk::ImageAttachment>;
  auto draw_bounding_boxes(
    this RendererInstance&,
    vuk::Value<vuk::ImageAttachment>&& depth_attachment,
    vuk::Value<vuk::ImageAttachment>&& dst_attachment
  ) -> vuk::Value<vuk::ImageAttachment>;

  auto update_vbgtao_info(this RendererInstance&, const RendererCVar& cvar) -> void;

private:
  bool update_ran_this_frame = false; // Sanity Check

  SharedResources shared_resources = {};
  std::vector<RenderStageCallback> stage_callbacks;
  std::vector<std::vector<usize>> before_callbacks;
  std::vector<std::vector<usize>> after_callbacks;

  auto rebuild_execution_order(this RendererInstance& self) -> void;
  auto execute_stages_before(this const RendererInstance& self, RenderStage stage, RenderStageContext& ctx) -> void;
  auto execute_stages_after(this const RendererInstance& self, RenderStage stage, RenderStageContext& ctx) -> void;

  Scene& scene;
  Renderer& renderer;
  GPU::RenderQueue2D render_queue_2d = {};
  bool saved_camera = false;

  glm::uvec2 viewport_size_ = {};
  glm::uvec2 viewport_origin_ = {};
  glm::uvec2 surface_size_ = {};

  vuk::Extent3D sky_view_lut_extent = {.width = 312, .height = 192, .depth = 1};
  vuk::Extent3D sky_aerial_perspective_lut_extent = {.width = 32, .height = 32, .depth = 32};

  PreparedFrame prepared_frame = {};
  GPU::CameraData camera_data = {};
  GPU::CameraData previous_camera_data = {};

  GPU::SceneFlags gpu_scene_flags = {};

  GPU::TonemapType tonemap_type = GPU::TonemapType::AgX;

  bool directional_light_cast_shadows = true;
  bool sun_direction_changed = false;
  glm::vec3 light_grid_origin = {};
  glm::vec3 previous_sun_direction = {};
  GPU::DirectionalLight directional_light = {};
  f32 first_clipmap_width = 1.0f;
  f32 clipmap_selection_bias = 2.0f;
  GPU::Atmosphere atmosphere = {};
  GPU::SkyData sky_data = {};
  GPU::EyeAdaptationSettings eye_adaptation = {};
  GPU::VBGTAOSettings vbgtao_info = {};
  GPU::PostProcessSettings post_proces_settings = {};

  Texture sky_transmittance_lut = {};
  Texture sky_multiscatter_lut = {};
  Texture hilbert_noise_lut = {};
  Texture sky_cubemap = {};

  vuk::Unique<vuk::Buffer> transforms_world_buffer{};
  vuk::Unique<vuk::Buffer> transforms_previous_buffer{};
  vuk::Unique<vuk::Buffer> mesh_instances_buffer{};
  vuk::Unique<vuk::Buffer> meshes_buffer{};
  vuk::Unique<vuk::Buffer> debug_renderer_verticies_buffer{};
  vuk::Unique<vuk::Buffer> lights_buffer{};
  vuk::Unique<vuk::Buffer> meshlet_instance_visibility_mask_buffer{};
  vuk::Unique<vuk::Buffer> terrain_patch_visibility_mask_buffer{};
  u32 terrain_patch_visibility_patch_count = 0;
  vuk::Unique<vuk::Buffer> exposure_buffer{};

  Texture vsm_virtual_page_table = {};
  Texture vsm_pointspot_virtual_page_table = {};
  Texture vsm_hpb = {};
  Texture vsm_pointspot_hpb = {};
  std::array<ShadowSlotState, GPU::MAX_SHADOW_POINT_LIGHTS> shadow_point_slots = {};
  std::array<ShadowSlotState, GPU::MAX_SHADOW_SPOT_LIGHTS> shadow_spot_slots = {};
  vuk::Unique<vuk::Image> vsm_physical_page_table{};
  vuk::Unique<vuk::ImageView> vsm_physical_page_table_f32_view{};
  vuk::Unique<vuk::ImageView> vsm_physical_page_table_u32_view{};
  vuk::ImageAttachment vsm_physical_page_table_attachment = {};
};
} // namespace ox
