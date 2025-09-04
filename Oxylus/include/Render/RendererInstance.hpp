#pragma once

#include "Render/Renderer.hpp"
#include "Scene/SceneGPU.hpp"

namespace ox {
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
  vuk::Value<vuk::Buffer> transforms_buffer = {};
  vuk::Value<vuk::Buffer> meshes_buffer = {};
  vuk::Value<vuk::Buffer> mesh_instances_buffer = {};
  vuk::Value<vuk::Buffer> meshlet_instances_buffer = {};
  vuk::Value<vuk::Buffer> visible_meshlet_instances_indices_buffer = {};
  vuk::Value<vuk::Buffer> reordered_indices_buffer = {};
  vuk::Value<vuk::Buffer> materials_buffer = {};
  vuk::Value<vuk::Buffer> point_lights_buffer = {};
  vuk::Value<vuk::Buffer> spot_lights_buffer = {};

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

  auto render(this RendererInstance& self, const Renderer::RenderInfo& render_info) -> vuk::Value<vuk::ImageAttachment>;
  auto update(this RendererInstance& self, RendererInstanceUpdateInfo& info) -> void;

  auto get_viewport_offset(this const RendererInstance& self) -> glm::uvec2 { return self.viewport_offset; }

private:
  Scene* scene = nullptr;
  Renderer& renderer;
  Renderer::RenderQueue2D render_queue_2d = {};
  bool saved_camera = false;

  glm::uvec2 viewport_offset = {};

  PreparedFrame prepared_frame = {};
  GPU::CameraData camera_data = {};
  GPU::CameraData previous_camera_data = {};

  GPU::Scene gpu_scene = {};

  option<GPU::Atmosphere> atmosphere = nullopt;
  option<GPU::Sun> sun = nullopt;

  option<GPU::HistogramInfo> histogram_info = nullopt;

  Texture hiz_view;
  vuk::Unique<vuk::Buffer> transforms_buffer{};
  vuk::Unique<vuk::Buffer> mesh_instances_buffer{};
  vuk::Unique<vuk::Buffer> meshes_buffer{};
  vuk::Unique<vuk::Buffer> materials_buffer{};
  vuk::Unique<vuk::Buffer> debug_renderer_verticies_buffer{};
  vuk::Unique<vuk::Buffer> point_lights_buffer{};
  vuk::Unique<vuk::Buffer> spot_lights_buffer{};
};
} // namespace ox
