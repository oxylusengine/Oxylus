#pragma once

#include <expected>
#include <glm/ext/vector_uint2.hpp>
#include <vuk/Value.hpp>
#include <vuk/runtime/vk/Descriptor.hpp>

#include "Core/Option.hpp"
#include "Scene/SceneGPU.hpp"
#include "Utils/Timestep.hpp"

namespace ox {
class RendererInstance;
class Scene;
class RenderContext;

class Renderer {
public:
  constexpr static auto MODULE_NAME = "Renderer";

  vuk::Unique<vuk::Buffer> quad_vertex_buffer = vuk::Unique<vuk::Buffer>();
  vuk::Unique<vuk::Buffer> quad_index_buffer = vuk::Unique<vuk::Buffer>();

  auto init(this Renderer& self) -> std::expected<void, std::string>;
  auto deinit(this Renderer& self) -> std::expected<void, std::string>;
  auto update(this Renderer& self, const Timestep& delta_time) -> void;

  auto new_instance(Scene& scene) -> std::unique_ptr<RendererInstance>;

  auto get_materials_buffer(this Renderer& self) -> vuk::Value<vuk::Buffer>;

  auto sync_materials(this Renderer& self) -> void;

private:
  friend RendererInstance;

  RenderContext* render_context = nullptr;
  bool initalized = false;

  std::vector<GPU::Material> gpu_materials = {};
  std::vector<usize> pending_material_indices = {};
  vuk::Unique<vuk::Buffer> materials_buffer = vuk::Unique<vuk::Buffer>();
};
} // namespace ox
