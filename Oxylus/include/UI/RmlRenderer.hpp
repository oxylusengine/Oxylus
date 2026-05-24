#pragma once

#include <RmlUi/Core/RenderInterface.h>
#include <vuk/Value.hpp>

#include "Render/Vulkan/VkContext.hpp"

namespace ox {
class RmlRenderer : public Rml::RenderInterface {
public:
  auto begin_frame() -> void;
  auto end_frame(VkContext& context, vuk::Value<vuk::ImageAttachment> target) -> vuk::Value<vuk::ImageAttachment>;

  // --- Derived functions ---
  auto CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices)
    -> Rml::CompiledGeometryHandle override;
  auto RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture)
    -> void override;
  auto ReleaseGeometry(Rml::CompiledGeometryHandle geometry) -> void override;
  auto LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) -> Rml::TextureHandle override;
  auto GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions)
    -> Rml::TextureHandle override;
  auto ReleaseTexture(Rml::TextureHandle texture) -> void override;
  auto EnableScissorRegion(bool enable) -> void override;
  auto SetScissorRegion(Rml::Rectanglei region) -> void override;
};
} // namespace ox
