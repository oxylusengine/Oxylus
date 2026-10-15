#pragma once

#include <expected>
#include <glm/ext/vector_uint2.hpp>
#include <memory>
#include <vector>

#include "Render/Fwd.hpp"
#include "Render/GPU/Shared.hpp"
#include "Utils/Timestep.hpp"

namespace ox {
class RendererInstance;
class Scene;
class RenderContext;

class Renderer {
public:
  constexpr static auto MODULE_NAME = "Renderer";

  BufferID quad_vertex_buffer = BufferID::Invalid;
  BufferID quad_index_buffer = BufferID::Invalid;

  auto init(this Renderer& self) -> std::expected<void, std::string>;
  auto deinit(this Renderer& self) -> std::expected<void, std::string>;
  auto update(this Renderer& self, const Timestep& delta_time) -> void;

  auto new_instance(Scene& scene) -> std::unique_ptr<RendererInstance>;

  // every material the scenes draw with, resting at `vir::Access::MemoryRead` between uploads
  BufferID materials_buffer = BufferID::Invalid;

  auto sync_materials(this Renderer& self) -> void;

private:
  friend RendererInstance;

  RenderContext* render_context = nullptr;
  bool initalized = false;

  std::vector<GPU::Material> gpu_materials = {};
  std::vector<usize> pending_material_indices = {};
};
} // namespace ox
