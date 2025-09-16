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

  auto
  set_image_resource(this RenderStageContext& self, const std::string& name, vuk::Value<vuk::ImageAttachment> value)
    -> RenderStageContext& {
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
  vuk::Value<vuk::Buffer> meshlet_instance_visibility_mask_buffer = {};
  vuk::Value<vuk::Buffer> materials_buffer = {};
  vuk::Value<vuk::Buffer> lights_buffer{};
  // We still need them to ensure correct sync after we update lights
  // even if we are not explicitly using just them as descriptors
  // (they are BDA in lights_buffer struct)
  vuk::Value<vuk::Buffer> point_lights_buffer{};
  vuk::Value<vuk::Buffer> spot_lights_buffer{};

  u32 line_index_count = 0;
  u32 triangle_index_count = 0;
  vuk::Value<vuk::Buffer> debug_renderer_verticies_buffer = {};
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

private:
  SharedResources shared_resources = {};
  std::vector<RenderStageCallback> stage_callbacks_;
  std::vector<std::vector<usize>> before_callbacks_;
  std::vector<std::vector<usize>> after_callbacks_;

  auto rebuild_execution_order(this RendererInstance& self) -> void;
  auto execute_stages_before(this const RendererInstance& self, RenderStage stage, RenderStageContext& ctx) -> void;
  auto execute_stages_after(this const RendererInstance& self, RenderStage stage, RenderStageContext& ctx) -> void;

  Scene* scene = nullptr;
  Renderer& renderer;
  Renderer::RenderQueue2D render_queue_2d = {};
  bool saved_camera = false;

  glm::uvec2 viewport_size = {};
  glm::uvec2 viewport_offset = {};

  PreparedFrame prepared_frame = {};
  GPU::CameraData camera_data = {};
  GPU::CameraData previous_camera_data = {};

  GPU::Scene gpu_scene = {};

  std::vector<GPU::CameraData> directional_light_cameras = {};

  option<GPU::Atmosphere> atmosphere = nullopt;

  option<GPU::HistogramInfo> histogram_info = nullopt;

  option<GPU::VBGTAOSettings> vbgtao_info = nullopt;

  GPU::PostProcessSettings post_proces_settings = {};

  Texture hiz_view;
  Texture directional_shadow_hiz_view;
  vuk::Unique<vuk::Buffer> transforms_buffer{};
  vuk::Unique<vuk::Buffer> mesh_instances_buffer{};
  vuk::Unique<vuk::Buffer> meshes_buffer{};
  vuk::Unique<vuk::Buffer> meshlet_instance_visibility_mask_buffer{};
  vuk::Unique<vuk::Buffer> materials_buffer{};
  vuk::Unique<vuk::Buffer> debug_renderer_verticies_buffer{};
  vuk::Unique<vuk::Buffer> point_lights_buffer{};
  vuk::Unique<vuk::Buffer> spot_lights_buffer{};
};
} // namespace ox
