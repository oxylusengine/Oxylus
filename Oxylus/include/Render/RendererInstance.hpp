#pragma once

#include <ankerl/unordered_dense.h>

#include "Render/Renderer.hpp"
#include "Scene/SceneGPU.hpp"

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

  std::span<u32> dirty_material_indices = {};
  std::span<GPU::Material> gpu_materials = {};

  std::span<GPU::Mesh> gpu_meshes = {};
  std::span<GPU::MeshInstance> gpu_mesh_instances = {};
};

struct PreparedFrame {
  u32 mesh_instance_count = 0;
  u32 max_meshlet_instance_count = 0;
  vuk::Value<vuk::Buffer> transforms_buffer = {};
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
  vuk::Value<vuk::Buffer> directional_light_buffer = {};
  vuk::Value<vuk::Buffer> directional_light_cascades_buffer = {};
  vuk::Value<vuk::Buffer> point_lights_buffer = {};
  vuk::Value<vuk::Buffer> spot_lights_buffer = {};
  vuk::Value<vuk::Buffer> exposure_buffer = {};

  u32 line_index_count = 0;
  u32 triangle_index_count = 0;
  vuk::Value<vuk::Buffer> debug_renderer_verticies_buffer = {};
};

struct CullGeometryContext {
  // When true, uses `cull_meshlets_hiz` (VisBuffer path with occlusion culling);
  // when false, uses `cull_meshlets` (shadowmap path, frustum only).
  bool use_hiz = false;
  // HiZ late-pass occlusion culling flag (only meaningful when `use_hiz` is true).
  bool late = false;
  // When true, runs the `cull_meshes` pre-pass (allocates/zeroes the visibility
  // and dispatch buffers). Run once per sequence: visbuffer early pass or
  // shadowmap cascade 0; subsequent culls in the sequence set this to false.
  bool init_cull_meshes = false;

  GPU::CullCamera cull_camera = {};

  // HiZ pyramid attachment consumed by `cull_meshlets_hiz` (only when `use_hiz`).
  vuk::Value<vuk::ImageAttachment> hiz_attachment = {};

  vuk::Value<vuk::Buffer> visibility_buffer = {};
  vuk::Value<vuk::Buffer> cull_meshlets_cmd_buffer = {};
  vuk::Value<vuk::Buffer> draw_geometry_cmd_buffer = {};
};

struct MainGeometryContext {
  bool late = false;
  bool draw_overdraw = false;

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
};

struct ShadowGeometryContext {
  vuk::Value<vuk::ImageAttachment> shadowmap_attachment = {};

  vuk::Value<vuk::Buffer> draw_geometry_cmd_buffer = {};
};

struct RMVSMContext {
  constexpr static u32 PAGE_SIZE = 128;
  constexpr static u32 MAX_DIRECTIONAL_CLIPMAP_COUNT = 10;
  constexpr static u32 DIRECTIONAL_IMAGE_SIZE = 1 << 12;
  constexpr static u32 DIRECTIONAL_PAGE_TABLE_SIZE = DIRECTIONAL_IMAGE_SIZE / PAGE_SIZE;
  constexpr static u32 DIRECTIONAL_MAX_PAGE_COUNT = DIRECTIONAL_PAGE_TABLE_SIZE * DIRECTIONAL_PAGE_TABLE_SIZE;
  constexpr static u32 DIRECTIONAL_PAGE_MASK_COUNT = (DIRECTIONAL_MAX_PAGE_COUNT + 31) / 32;

  bool sun_moved = false;
  vuk::Extent3D depth_extent = {};
  f32 max_shadow_dist = 1000.0f;

  vuk::Value<vuk::Buffer> directional_clipmaps_buffer = {};

  vuk::Value<vuk::ImageAttachment> depth_attachment = {};
  vuk::Value<vuk::ImageAttachment> virtual_page_table_attachment = {};
  vuk::Value<vuk::ImageAttachment> physical_page_table_attachment = {};
};

struct ShadowResolveContext {
  // TODO: Add shadowmap kind enum
  f32 max_shadow_dist = 1000.0f;

  vuk::Value<vuk::ImageAttachment> depth_attachment = {};
  vuk::Value<vuk::ImageAttachment> normal_attachment = {};

  // CSM:
  vuk::Value<vuk::ImageAttachment> directional_shadowmap_attachment = {};

  // VSM:
  vuk::Value<vuk::Buffer> directional_clipmaps_buffer = {};
  vuk::Value<vuk::ImageAttachment> virtual_page_table_attachment = {};
  vuk::Value<vuk::ImageAttachment> physical_page_table_attachment = {};

  vuk::Value<vuk::ImageAttachment> resolved_shadows_attachment = {};
};

struct AtmosphereContext {
  vuk::Value<vuk::ImageAttachment> sky_transmittance_lut_attachment = {};
  vuk::Value<vuk::ImageAttachment> sky_multiscatter_lut_attachment = {};
  vuk::Value<vuk::ImageAttachment> sky_view_lut_attachment = {};
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
  // vuk::Value<vuk::ImageAttachment> sky_cubemap_attachment = {};
  vuk::Value<vuk::ImageAttachment> depth_attachment = {};
  vuk::Value<vuk::ImageAttachment> albedo_attachment = {};
  vuk::Value<vuk::ImageAttachment> normal_attachment = {};
  vuk::Value<vuk::ImageAttachment> emissive_attachment = {};
  vuk::Value<vuk::ImageAttachment> metallic_roughness_occlusion_attachment = {};
  vuk::Value<vuk::ImageAttachment> ambient_occlusion_attachment = {};
  vuk::Value<vuk::ImageAttachment> contact_shadows_attachment = {};
  vuk::Value<vuk::ImageAttachment> resolved_shadows_attachment = {};
};

struct DebugContext {
  f32 overdraw_heatmap_scale = 0.0f;
  GPU::DebugView debug_view = GPU::DebugView::None;

  vuk::Value<vuk::ImageAttachment> visbuffer_attachment = {};
  vuk::Value<vuk::ImageAttachment> depth_attachment = {};
  vuk::Value<vuk::ImageAttachment> overdraw_attachment = {};
  vuk::Value<vuk::ImageAttachment> albedo_attachment = {};
  vuk::Value<vuk::ImageAttachment> normal_attachment = {};
  vuk::Value<vuk::ImageAttachment> emissive_attachment = {};
  vuk::Value<vuk::ImageAttachment> metallic_roughness_occlusion_attachment = {};
  vuk::Value<vuk::ImageAttachment> ambient_occlusion_attachment = {};
};

struct PostProcessContext {
  f32 delta_time = 0.0f;
  vuk::Extent3D extent = {};

  vuk::Value<vuk::ImageAttachment> dst_attachment = {};
  vuk::Value<vuk::ImageAttachment> final_attachment = {};
  vuk::Value<vuk::ImageAttachment> bloom_upsampled_attachment = {};
};

class RendererInstance {
public:
  template <typename T>
  struct BufferTraits;

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
    const Renderer::RenderInfo& render_info
  ) -> vuk::Value<vuk::ImageAttachment>;
  auto update(this RendererInstance& self, RendererInstanceUpdateInfo& info) -> void;

  auto get_viewport_offset(this const RendererInstance& self) -> glm::uvec2 { return self.viewport_offset; }

  auto generate_hiz(this RendererInstance&, MainGeometryContext& context) -> void;
  auto cull_geometry(this RendererInstance& self, CullGeometryContext& context) -> void;
  auto draw_for_visbuffer(this RendererInstance&, MainGeometryContext& context) -> void;
  auto decode_visbuffer(this RendererInstance&, MainGeometryContext& context) -> void;
  auto draw_for_shadowmap(
    this RendererInstance&, ShadowGeometryContext& context, glm::mat4& projection_view, u32 cascade_index
  ) -> void;
  auto draw_virtual_shadowmap(this RendererInstance&, RMVSMContext& context) -> void;
  auto resolve_shadowmap(this RendererInstance&, ShadowResolveContext& context) -> void;
  auto draw_atmosphere(this RendererInstance&, AtmosphereContext& context) -> void;
  auto generate_ambient_occlusion(this RendererInstance&, AmbientOcclusionContext& context) -> void;
  auto apply_pbr(this RendererInstance&, PBRContext& context, vuk::Value<vuk::ImageAttachment>&& dst_attachment)
    -> vuk::Value<vuk::ImageAttachment>;
  auto apply_eye_adaptation(this RendererInstance&, PostProcessContext& context) -> void;
  auto apply_bloom(this RendererInstance&, PostProcessContext& context) -> void;
  auto apply_tonemap(this RendererInstance&, PostProcessContext& context) -> vuk::Value<vuk::ImageAttachment>;
  auto apply_debug_view(this RendererInstance&, DebugContext& context, vuk::Extent3D extent)
    -> vuk::Value<vuk::ImageAttachment>;
  auto draw_for_debug(this RendererInstance&, DebugContext& context, vuk::Value<vuk::ImageAttachment>&& dst_attachment)
    -> vuk::Value<vuk::ImageAttachment>;

  auto update_vbgtao_info(this RendererInstance&) -> void;

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
  Renderer::RenderQueue2D render_queue_2d = {};
  bool saved_camera = false;

  glm::uvec2 viewport_size = {};
  glm::uvec2 viewport_offset = {};

  vuk::Extent3D sky_view_lut_extent = {.width = 312, .height = 192, .depth = 1};
  vuk::Extent3D sky_aerial_perspective_lut_extent = {.width = 32, .height = 32, .depth = 32};

  PreparedFrame prepared_frame = {};
  GPU::CameraData camera_data = {};
  GPU::CameraData previous_camera_data = {};

  GPU::SceneFlags gpu_scene_flags = {};
  bool occlusion_cull = true;

  GPU::TonemapType tonemap_type = GPU::TonemapType::AgX;

  bool directional_light_cast_shadows = true;
  bool sun_direction_changed = false;
  glm::vec3 previous_sun_direction = {};
  GPU::DirectionalLight directional_light = {};
  f32 first_clipmap_width = 1.0f;
  f32 clipmap_selection_bias = 2.0f;
  std::array<GPU::DirectionalLightCascade, MAX_DIRECTIONAL_SHADOW_CASCADES> directional_light_cascades = {};
  GPU::Atmosphere atmosphere = {};
  GPU::EyeAdaptationSettings eye_adaptation = {};
  GPU::VBGTAOSettings vbgtao_info = {};
  GPU::PostProcessSettings post_proces_settings = {};

  vuk::Unique<vuk::Buffer> transforms_buffer{};
  vuk::Unique<vuk::Buffer> mesh_instances_buffer{};
  vuk::Unique<vuk::Buffer> meshes_buffer{};
  vuk::Unique<vuk::Buffer> materials_buffer{};
  vuk::Unique<vuk::Buffer> debug_renderer_verticies_buffer{};
  vuk::Unique<vuk::Buffer> point_lights_buffer{};
  vuk::Unique<vuk::Buffer> spot_lights_buffer{};
  vuk::Unique<vuk::Buffer> meshlet_instance_visibility_mask_buffer{};
  vuk::Unique<vuk::Buffer> exposure_buffer{};

  vuk::Unique<vuk::Image> vsm_virtual_page_table{};
  vuk::Unique<vuk::ImageView> vsm_virtual_page_table_view{};
  vuk::ImageAttachment vsm_virtual_page_table_attachment = {};
  vuk::Unique<vuk::Image> vsm_hpb{};
  vuk::Unique<vuk::ImageView> vsm_hpb_view{};
  vuk::ImageAttachment vsm_hpb_attachment = {};
  vuk::Unique<vuk::Image> vsm_physical_page_table{};
  vuk::Unique<vuk::ImageView> vsm_physical_page_table_f32_view{};
  vuk::Unique<vuk::ImageView> vsm_physical_page_table_u32_view{};
  vuk::ImageAttachment vsm_physical_page_table_attachment = {};
};
} // namespace ox
