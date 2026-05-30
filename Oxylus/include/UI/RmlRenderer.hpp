#pragma once

#include <RmlUi/Core/RenderInterface.h>
#include <ankerl/unordered_dense.h>
#include <vuk/Value.hpp>

#include "Asset/Texture.hpp"
#include "Render/Vulkan/VkContext.hpp"

namespace ox {
struct RmlDrawCmd {
  u32 index_count;
  u32 index_offset;
  u32 vertex_offset;
  Rml::TextureHandle texture;
  Rml::TextureHandle texture_array_index{};
  Rml::Vector2f translation;
  bool scissor_enabled;
  glm::ivec4 scissor; // x, y, w, h
};

struct RmlCompiledGeometry {
  std::vector<Rml::Vertex> vertices;
  std::vector<int> indices;
};

class RmlRenderer : public Rml::RenderInterface {
public:
  auto begin_frame(this RmlRenderer& self) -> void;
  auto end_frame(this RmlRenderer& self, VkContext& context, vuk::Value<vuk::ImageAttachment> target)
    -> vuk::Value<vuk::ImageAttachment>;

  auto render_geometry(
    Rml::Vertex* vertices,
    int num_vertices,
    int* indices,
    int num_indices,
    Rml::TextureHandle texture,
    const Rml::Vector2f& translation
  ) -> void;

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

private:
  enum class RmlTextureID : u64 {};
  enum class RmlGeometryID : u64 {};

  std::vector<Rml::Vertex> frame_vertices = {};
  std::vector<i32> frame_indices = {};
  SlotMap<RmlCompiledGeometry, RmlGeometryID> compiled_geometries;
  std::vector<RmlDrawCmd> draw_commands = {};

  SlotMap<std::unique_ptr<Texture>, RmlTextureID> loaded_textures = {};

  bool current_scissor_enabled = false;
  glm::ivec4 current_scissor; // x, y, w, h
};
} // namespace ox
