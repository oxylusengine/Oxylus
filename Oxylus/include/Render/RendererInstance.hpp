#pragma once

#include "Render/Renderer.hpp"
#include "Scene/SceneGPU.hpp"

namespace ox {
enum class RenderStage {
  Initialization,
  Culling,
  VisBufferEncode,
  VisBufferDecode,
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
};

struct RenderStageContext {
  RendererInstance& renderer_instance;
  SharedResources& shared_resources;
  RenderStage current_stage;
  VkContext& vk_context;

  glm::uvec2 viewport_size;

  ankerl::unordered_dense::map<std::string, vuk::Value<vuk::Buffer>> buffer_resources = {};
  ankerl::unordered_dense::map<std::string, vuk::Value<vuk::ImageAttachment>> image_resources = {};

  RenderStageContext(RendererInstance& instance, SharedResources& shared_r, RenderStage stage, VkContext& vkctx)
      : renderer_instance(instance),
        shared_resources(shared_r),
        current_stage(stage),
        vk_context(vkctx) {}

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
  vuk::Value<vuk::Buffer> directional_light_buffer{};
  vuk::Value<vuk::Buffer> directional_light_cascades_buffer{};
  vuk::Value<vuk::Buffer> point_lights_buffer{};
  vuk::Value<vuk::Buffer> spot_lights_buffer{};

  u32 line_index_count = 0;
  u32 triangle_index_count = 0;
  vuk::Value<vuk::Buffer> debug_renderer_verticies_buffer = {};
};

struct MainGeometryContext {
  u32 mesh_instance_count = 0;
  u32 max_meshlet_instance_count = 0;
  bool late = false;

  vuk::PersistentDescriptorSet* bindless_set = nullptr;
  vuk::Value<vuk::ImageAttachment> depth_attachment = {};
  vuk::Value<vuk::ImageAttachment> hiz_attachment = {};
  vuk::Value<vuk::ImageAttachment> visbuffer_attachment = {};
  vuk::Value<vuk::ImageAttachment> overdraw_attachment = {};
  vuk::Value<vuk::ImageAttachment> albedo_attachment = {};
  vuk::Value<vuk::ImageAttachment> normal_attachment = {};
  vuk::Value<vuk::ImageAttachment> emissive_attachment = {};
  vuk::Value<vuk::ImageAttachment> metallic_roughness_occlusion_attachment = {};

  vuk::Value<vuk::Buffer> visibility_buffer = {};
  vuk::Value<vuk::Buffer> cull_meshlets_cmd_buffer = {};
  vuk::Value<vuk::Buffer> draw_geometry_cmd_buffer = {};
};

struct ShadowGeometryContext {
  u32 mesh_instance_count = 0;
  u32 max_meshlet_instance_count = 0;

  vuk::Value<vuk::ImageAttachment> shadowmap_attachment = {};

  vuk::Value<vuk::Buffer> visibility_buffer = {};
  vuk::Value<vuk::Buffer> cull_meshlets_cmd_buffer = {};
  vuk::Value<vuk::Buffer> draw_geometry_cmd_buffer = {};
};

struct AmbientOcclusionContext {
  GPU::VBGTAOSettings settings = {};

  vuk::Value<vuk::ImageAttachment> noise_attachment = {};
  vuk::Value<vuk::ImageAttachment> normal_attachment = {};
  vuk::Value<vuk::ImageAttachment> depth_attachment = {};
  vuk::Value<vuk::ImageAttachment> depth_differences_attachment = {};
  vuk::Value<vuk::ImageAttachment> ambient_occlusion_attachment = {};
};

struct PBRContext {
  GPU::SceneFlags scene_flags = {};
  vuk::PersistentDescriptorSet* bindless_set = nullptr;

  vuk::Value<vuk::ImageAttachment> sky_transmittance_lut_attachment = {};
  // vuk::Value<vuk::ImageAttachment> sky_cubemap_attachment = {};
  vuk::Value<vuk::ImageAttachment> depth_attachment = {};
  vuk::Value<vuk::ImageAttachment> albedo_attachment = {};
  vuk::Value<vuk::ImageAttachment> normal_attachment = {};
  vuk::Value<vuk::ImageAttachment> emissive_attachment = {};
  vuk::Value<vuk::ImageAttachment> metallic_roughness_occlusion_attachment = {};
  vuk::Value<vuk::ImageAttachment> ambient_occlusion_attachment = {};
  vuk::Value<vuk::ImageAttachment> contact_shadows_attachment = {};
  vuk::Value<vuk::ImageAttachment> directional_shadowmap_attachment = {};
};

class RendererInstance {
public:
  template <typename T>
  struct BufferTraits;

  explicit RendererInstance(Scene* owner_scene, Renderer& parent_renderer);
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

  auto render(this RendererInstance& self, const Renderer::RenderInfo& render_info) -> vuk::Value<vuk::ImageAttachment>;
  auto update(this RendererInstance& self, RendererInstanceUpdateInfo& info) -> void;

  auto get_viewport_offset(this const RendererInstance& self) -> glm::uvec2 { return self.viewport_offset; }

  auto generate_hiz(this RendererInstance&, MainGeometryContext& context) -> void;
  auto cull_for_visbuffer(this RendererInstance&, MainGeometryContext& context) -> void;
  auto draw_for_visbuffer(this RendererInstance&, MainGeometryContext& context) -> void;
  auto decode_visbuffer(this RendererInstance&, MainGeometryContext& context) -> void;
  auto cull_for_shadowmap(this RendererInstance&, ShadowGeometryContext& context, glm::mat4& projection_view) -> void;
  auto draw_for_shadowmap(
    this RendererInstance&, ShadowGeometryContext& context, glm::mat4& projection_view, u32 cascade_index
  ) -> void;
  auto generate_ambient_occlusion(this RendererInstance&, AmbientOcclusionContext& context) -> void;
  auto apply_pbr(this RendererInstance&, PBRContext& context, vuk::Value<vuk::ImageAttachment>&& dst_attachment)
    -> vuk::Value<vuk::ImageAttachment>;

private:
  SharedResources shared_resources = {};
  std::vector<RenderStageCallback> stage_callbacks;
  std::vector<std::vector<usize>> before_callbacks;
  std::vector<std::vector<usize>> after_callbacks;

  auto rebuild_execution_order(this RendererInstance& self) -> void;
  auto execute_stages_before(this const RendererInstance& self, RenderStage stage, RenderStageContext& ctx) -> void;
  auto execute_stages_after(this const RendererInstance& self, RenderStage stage, RenderStageContext& ctx) -> void;

  Scene* scene = nullptr;
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

  bool directional_light_cast_shadows = true;
  option<GPU::DirectionalLight> directional_light = nullopt;
  std::array<GPU::DirectionalLightCascade, MAX_DIRECTIONAL_SHADOW_CASCADES> directional_light_cascades = {};
  option<GPU::Atmosphere> atmosphere = nullopt;
  option<GPU::HistogramInfo> histogram_info = nullopt;
  option<GPU::VBGTAOSettings> vbgtao_info = nullopt;
  GPU::PostProcessSettings post_proces_settings = {};

  vuk::Unique<vuk::Buffer> transforms_buffer{};
  vuk::Unique<vuk::Buffer> mesh_instances_buffer{};
  vuk::Unique<vuk::Buffer> meshes_buffer{};
  vuk::Unique<vuk::Buffer> materials_buffer{};
  vuk::Unique<vuk::Buffer> debug_renderer_verticies_buffer{};
  vuk::Unique<vuk::Buffer> point_lights_buffer{};
  vuk::Unique<vuk::Buffer> spot_lights_buffer{};
  vuk::Unique<vuk::Buffer> meshlet_instance_visibility_mask_buffer{};
};
} // namespace ox
