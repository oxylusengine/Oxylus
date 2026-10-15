#pragma once

#include <RmlUi/Core/RenderInterface.h>
#include <ankerl/unordered_dense.h>
#include <glm/vec4.hpp>

#include "Asset/Texture.hpp"
#include "Memory/SlotMap.hpp"
#include "Render/Fwd.hpp"
#include "Render/Graph/Fwd.hpp"
#include "Render/Graph/Resource.hpp"

namespace ox {
struct RmlDrawCmd {
  u32 index_count;
  u32 index_offset;
  u32 vertex_offset;
  Rml::TextureHandle texture;
  Rml::Vector2f translation;
  Rml::Matrix4f transform;
  bool scissor_enabled;
  glm::ivec4 scissor; // x, y, w, h
};

struct RmlCompiledGeometry {
  std::vector<Rml::Vertex> vertices;
  std::vector<int> indices;
};

// One per context, the geometry it collects is drawn by the program of whatever owns the context.
class RmlRenderer : public Rml::RenderInterface {
public:
  RmlRenderer();
  ~RmlRenderer() override;

  auto begin_frame(this RmlRenderer& self) -> void;
  // stages the geometry collected since `begin_frame` into this frame's allocator
  auto upload(this RmlRenderer& self, RenderContext& context) -> void;
  // draws whatever was last uploaded over `target`. Recorded once, it reads the uploads every frame
  auto record(this RmlRenderer& self, vir::Module& module, RenderContext& render_context, vir::ValueID target)
    -> vir::ValueID;

  auto render_geometry(
    this RmlRenderer& self,
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
  auto SetTransform(const Rml::Matrix4f* transform) -> void override;

private:
  enum class RmlTextureID : u64 {};
  enum class RmlGeometryID : u64 {};

  std::vector<Rml::Vertex> frame_vertices = {};
  std::vector<i32> frame_indices = {};
  SlotMap<RmlCompiledGeometry, RmlGeometryID> compiled_geometries;
  std::vector<RmlDrawCmd> draw_commands = {};

  SlotMap<Texture, RmlTextureID> loaded_textures = {};
  Texture white_texture = {};
  vir::Buffer vertex_buffer = {};
  vir::Buffer index_buffer = {};
  // what the last upload staged, the draws of a frame that staged nothing are dropped
  std::vector<RmlDrawCmd> uploaded_commands = {};

  bool current_scissor_enabled = false;
  glm::ivec4 current_scissor; // x, y, w, h
  Rml::Matrix4f current_transform = Rml::Matrix4f::Identity();
};
} // namespace ox
