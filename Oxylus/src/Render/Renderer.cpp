#include "Render/Renderer.hpp"

#include <vuk/runtime/CommandBuffer.hpp>
#include <vuk/vsl/Core.hpp>

#include "Asset/AssetFile.hpp"
#include "Core/App.hpp"
#include "Core/VFS.hpp"
#include "Render/RenderContext.hpp"
#include "Render/RendererInstance.hpp"

namespace ox {
auto Renderer::new_instance(Scene& scene) -> std::unique_ptr<RendererInstance> {
  ZoneScoped;

  if (!initalized) {
    OX_LOG_ERROR("Renderer must be initialized before creating instances!");
    return nullptr;
  }

  auto instance = std::make_unique<RendererInstance>(scene, *this);
  return instance;
}

auto Renderer::init(this Renderer& self) -> std::expected<void, std::string> {
  if (self.initalized)
    return std::unexpected("Renderer already initialized!");

  self.initalized = true;

  self.render_context = &App::get_rendercontext();

  auto& bindless_set = self.render_context->get_descriptor_set();

  self.render_context->wait();

  // --- Shaders ---
  auto& vfs = App::get_vfs();
  auto shaders_dir = vfs.resolve_physical_dir(VFS::APP_DIR, "Shaders");
  auto shader_file = AssetFile::unpack(shaders_dir / "engine.oxpack");
  if (!shader_file.has_value()) {
    return std::unexpected("Cannot initialize renderer shaders!");
  }

  for (const auto& entry : shader_file->entries) {
    const auto* pipeline_data = std::get_if<ShaderPipelineData>(&entry.data);
    if (!pipeline_data) {
      continue;
    }

    self.render_context->create_pipeline(*pipeline_data);
  }

  struct Vertex {
    alignas(4) glm::vec3 position = {};
    alignas(4) glm::vec2 uv = {};
  };
  std::vector<Vertex> vertices(4);
  vertices[0].position = glm::vec3{-1.f, -1.f, 0.f};
  vertices[0].uv = glm::vec2{0.f, 0.f};

  vertices[1].position = glm::vec3{1.f, -1.f, 0.f};
  vertices[1].uv = glm::vec2{1.f, 0.f};

  vertices[2].position = glm::vec3{1.f, 1.f, 0.f};
  vertices[2].uv = glm::vec2{1.f, 1.f};

  vertices[3].position = glm::vec3{-1.f, 1.f, 0.f};
  vertices[3].uv = glm::vec2{0.f, 1.f};

  auto indices = std::vector<uint32_t>{0, 1, 2, 2, 3, 0};

  self.quad_vertex_buffer = self.render_context->resize_buffer(
    std::move(self.quad_vertex_buffer),
    vuk::MemoryUsage::eGPUonly,
    ox::size_bytes(vertices)
  );
  auto upload_quad_vertex_buffer = self.render_context->upload_staging(std::span(vertices), *self.quad_vertex_buffer);

  self.quad_index_buffer = self.render_context->resize_buffer(
    std::move(self.quad_index_buffer),
    vuk::MemoryUsage::eGPUonly,
    ox::size_bytes(indices)
  );
  auto upload_quad_index_buffer = self.render_context->upload_staging(std::span(indices), *self.quad_index_buffer);

  self.render_context->wait_on(std::move(upload_quad_vertex_buffer));
  self.render_context->wait_on(std::move(upload_quad_index_buffer));

  return {};
}

auto Renderer::deinit(this Renderer& self) -> std::expected<void, std::string> {
  ZoneScoped;

  return {};
}

} // namespace ox
