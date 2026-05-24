#include "UI/RmlRenderer.hpp"

namespace ox {

auto RmlRenderer::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices)
  -> Rml::CompiledGeometryHandle {

  return Rml::CompiledGeometryHandle{};
}

auto RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture)
  -> void {}

auto ReleaseGeometry(Rml::CompiledGeometryHandle geometry) -> void {}

auto LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) -> Rml::TextureHandle {}

auto GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions) -> Rml::TextureHandle {}

auto ReleaseTexture(Rml::TextureHandle texture) -> void {}

auto EnableScissorRegion(bool enable) -> void {}

auto SetScissorRegion(Rml::Rectanglei region) -> void {}
} // namespace ox
