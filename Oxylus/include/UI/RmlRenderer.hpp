#pragma once

#include <RmlUi/Core/RenderInterface.h>
#include <ankerl/unordered_dense.h>
#include <functional>
#include <glm/vec2.hpp>
#include <memory>
#include <vector>
#include <vir/Fwd.hpp>
#include <vir/Resource.hpp>

#include "Asset/Texture.hpp"
#include "Memory/SlotMap.hpp"
#include "Render/Fwd.hpp"
#include "Render/GPU/Shared.hpp"
#include "UI/RmlRenderPlan.hpp"

namespace ox {
// RmlUi's pixel space to Vulkan clip space, applied after the element's own transform so a perspective one divides
auto rml_projection(f32 width, f32 height) -> Rml::Matrix4f;

struct RmlCompiledGeometry {
  std::vector<Rml::Vertex> vertices;
  std::vector<int> indices;
};

struct RmlCompositeVariables {
  vir::ValueID image = vir::ValueID::Invalid;
  vir::ValueID is_visible = vir::ValueID::Invalid;
};

struct RmlSignatureHash {
  using is_avalanching = void;
  auto operator()(const std::vector<u32>& signature) const noexcept -> u64;
};

struct RmlProgram;

// one per context. RmlUi's calls build a plan each frame, and the plan's structure picks a small program of its own,
// compiled once and run every frame that has that structure, which draws the UI into `output`. the scene's
// program only composites `output`, so a UI that changes shape never recompiles the scene
class RmlRenderer : public Rml::RenderInterface {
public:
  RmlRenderer();
  ~RmlRenderer() override;

  // before RmlUi renders, which is before RenderContext::new_frame: resizing `output` clears it right away
  auto begin_frame(this RmlRenderer& self, glm::ivec2 extent) -> void;
  // runs the program for what was collected since `begin_frame`, after RenderContext::new_frame and before the
  // scene's program, which reads `output`
  auto render(this RmlRenderer& self, RenderContext& context) -> void;
  // composites `output` over `target` whenever the last `render` drew something. Recorded once into the scene's program
  static auto record_composite(
    vir::Module& module,
    RenderContext& render_context,
    vir::ValueID target,
    vir::Extent3D extent,
    RmlCompositeVariables& variables
  ) -> vir::ValueID;
  auto bind_composite(this const RmlRenderer& self, vir::Program& program, const RmlCompositeVariables& variables)
    -> void;

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

  auto EnableClipMask(bool enable) -> void override;
  auto RenderToClipMask(
    Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation
  ) -> void override;
  auto PushLayer() -> Rml::LayerHandle override;
  auto CompositeLayers(
    Rml::LayerHandle source,
    Rml::LayerHandle destination,
    Rml::BlendMode blend_mode,
    Rml::Span<const Rml::CompiledFilterHandle> filters
  ) -> void override;
  auto PopLayer() -> void override;
  auto SaveLayerAsTexture() -> Rml::TextureHandle override;
  auto SaveLayerAsMaskImage() -> Rml::CompiledFilterHandle override;
  auto CompileFilter(const Rml::String& name, const Rml::Dictionary& parameters) -> Rml::CompiledFilterHandle override;
  auto ReleaseFilter(Rml::CompiledFilterHandle filter) -> void override;
  auto CompileShader(const Rml::String& name, const Rml::Dictionary& parameters) -> Rml::CompiledShaderHandle override;
  auto RenderShader(
    Rml::CompiledShaderHandle shader,
    Rml::CompiledGeometryHandle geometry,
    Rml::Vector2f translation,
    Rml::TextureHandle texture
  ) -> void override;
  auto ReleaseShader(Rml::CompiledShaderHandle shader) -> void override;

private:
  enum class RmlTextureID : u64 { Invalid = ~0_u64 };
  enum class RmlGeometryID : u64 { Invalid = ~0_u64 };
  enum class RmlFilterID : u64 { Invalid = ~0_u64 };
  enum class RmlShaderID : u64 { Invalid = ~0_u64 };

  // stages the geometry into this frame's arrays
  auto stage_geometry(
    this RmlRenderer& self, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture
  ) -> option<RmlDrawCmd>;
  auto program_for(this RmlRenderer& self, RenderContext& context) -> RmlProgram*;
  auto record_program(this RmlRenderer& self, vir::Module& module, RenderContext& context, RmlProgram& program) -> void;
  auto draw_callback(this RmlRenderer& self, u32 pass_index, u32 sampler_index) -> std::function<void(vir::Recorder&)>;

  RmlPlanBuilder builder = {};
  // what the last `render` ran, read by the program's callbacks while it executes
  RmlRenderPlan uploaded = {};
  std::vector<Rml::Vertex> frame_vertices = {};
  std::vector<i32> frame_indices = {};
  std::vector<GPU::RmlGradient> frame_gradients = {};
  vir::Buffer vertex_buffer = {};
  vir::Buffer index_buffer = {};
  vir::Buffer gradient_buffer = {};

  SlotMap<RmlCompiledGeometry, RmlGeometryID> compiled_geometries = {};
  SlotMap<Texture, RmlTextureID> loaded_textures = {};
  SlotMap<RmlFilter, RmlFilterID> compiled_filters = {};
  SlotMap<GPU::RmlGradient, RmlShaderID> compiled_shaders = {};
  Texture white_texture = {};
  // premultiplied and gamma encoded, the size of the context
  Texture output = {};
  bool is_visible = false;

  ankerl::unordered_dense::map<std::vector<u32>, std::unique_ptr<RmlProgram>, RmlSignatureHash> programs = {};
  // least recently run first
  std::vector<std::vector<u32>> program_order = {};
  std::vector<u32> signature = {};

  Rml::Matrix4f current_transform = Rml::Matrix4f::Identity();
};
} // namespace ox
