#pragma once

#include "Render/Renderer.hpp"
#include "Scene/SceneGPU.hpp"

namespace ox {
class RendererInstance {
public:
  explicit RendererInstance(Scene* owner_scene, Renderer& parent_renderer);
  ~RendererInstance();

  RendererInstance(const RendererInstance&) = delete;
  RendererInstance& operator=(const RendererInstance&) = delete;
  RendererInstance(RendererInstance&&) = delete;
  RendererInstance& operator=(RendererInstance&&) = delete;

  auto render(this RendererInstance& self, const Renderer::RenderInfo& render_info) -> vuk::Value<vuk::ImageAttachment>;
  auto update(this RendererInstance& self) -> void;

private:
  Scene* scene = nullptr;
  Renderer& renderer;
  Renderer::RenderQueue2D render_queue_2d = {};
  bool saved_camera = false;

  std::span<GPU::Transforms> transforms = {};
  std::vector<GPU::TransformID> dirty_transforms = {};
  vuk::Unique<vuk::Buffer> transforms_buffer = vuk::Unique<vuk::Buffer>();

  GPU::CameraData camera_data = {};
  GPU::CameraData previous_camera_data = {};

  bool meshes_dirty = false;
  std::vector<GPU::Mesh> gpu_meshes = {};
  std::vector<GPU::MeshletInstance> gpu_meshlet_instances = {};
  vuk::Unique<vuk::Buffer> meshes_buffer = vuk::Unique<vuk::Buffer>();
  vuk::Unique<vuk::Buffer> meshlet_instances_buffer = vuk::Unique<vuk::Buffer>();

  option<GPU::Atmosphere> atmosphere = nullopt;
  option<GPU::Sun> sun = nullopt;

  option<GPU::HistogramInfo> histogram_info = nullopt;

  Texture hiz_view;
};
} // namespace ox
