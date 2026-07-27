#pragma once

#include <expected>
#include <glm/ext/vector_uint2.hpp>
#include <vuk/Value.hpp>
#include <vuk/runtime/vk/Descriptor.hpp>

namespace ox {
class RendererInstance;
class Scene;
class RenderContext;

class Renderer {
public:
  constexpr static auto MODULE_NAME = "Renderer";

  vuk::Unique<vuk::Buffer> quad_vertex_buffer = vuk::Unique<vuk::Buffer>();
  vuk::Unique<vuk::Buffer> quad_index_buffer = vuk::Unique<vuk::Buffer>();

  struct RenderInfo {
    glm::uvec2 viewport_offset = {};
  };

  auto init(this Renderer& self) -> std::expected<void, std::string>;
  auto deinit(this Renderer& self) -> std::expected<void, std::string>;

  auto new_instance(Scene& scene) -> std::unique_ptr<RendererInstance>;

private:
  friend RendererInstance;

  RenderContext* render_context = nullptr;
  bool initalized = false;
};
} // namespace ox
