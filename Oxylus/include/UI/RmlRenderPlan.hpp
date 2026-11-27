#pragma once

#include <RmlUi/Core/RenderInterface.h>
#include <ankerl/svector.h>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <span>
#include <vector>

#include "Core/Option.hpp"
#include "Core/Types.hpp"

namespace ox {
// every PushLayer gets a fresh one, so a frame's layers never share an image
enum class RmlLayerID : u16 { Base = 0, Invalid = 0xFFFF_u16 };
enum class RmlSavedTextureID : u16 { Invalid = 0xFFFF_u16 };
enum class RmlMaskID : u16 { Invalid = 0xFFFF_u16 };

enum class RmlPassKind : u8 { Draw, ClipClear, ClipDraw, PushLayer, Composite, SaveTexture, SaveMask };
enum class RmlClipWrite : u8 { Replace, Increment };
enum class RmlFilterKind : u8 { Opacity, Blur, DropShadow, ColorMatrix, MaskImage };

struct RmlDrawCmd {
  // device addresses of the compiled geometry, which stays on the GPU until RmlUi releases it
  u64 vertices = 0;
  u64 indices = 0;
  u32 index_count = 0;
  Rml::TextureHandle texture = 0;
  // into the frame's gradients
  option<u32> gradient = nullopt;
  Rml::Vector2f translation = {};
  Rml::Matrix4f transform = Rml::Matrix4f::Identity();
  // x, y, w, h, already clamped to the context
  glm::ivec4 scissor = {};
};

struct RmlFilter {
  RmlFilterKind kind = RmlFilterKind::Opacity;
  // opacity: the factor, blur and drop shadow: sigma in pixels
  f32 value = 1.0f;
  glm::vec2 offset = {};
  // premultiplied
  glm::vec4 color = {};
  Rml::Matrix4f color_matrix = Rml::Matrix4f::Identity();
  RmlMaskID mask = RmlMaskID::Invalid;

  // a drop shadow under half a pixel of sigma skips its blur
  auto is_blurred(this const RmlFilter& self) -> bool;
};

struct RmlPass {
  RmlPassKind kind = RmlPassKind::Draw;
  // written by Draw, PushLayer and Composite, read by SaveTexture and SaveMask
  RmlLayerID layer = RmlLayerID::Base;
  RmlLayerID source = RmlLayerID::Invalid;
  Rml::BlendMode blend_mode = Rml::BlendMode::Blend;
  RmlClipWrite clip_write = RmlClipWrite::Replace;
  // Draw and Composite: tested for equality when nonzero. ClipClear: the clear value. ClipDraw: written by
  // Replace, tested by Increment
  u8 stencil = 0;
  RmlSavedTextureID saved_texture = RmlSavedTextureID::Invalid;
  RmlMaskID mask = RmlMaskID::Invalid;
  u32 first_draw = 0;
  u32 draw_count = 0;
  u32 first_filter = 0;
  u32 filter_count = 0;
  // the textures saved earlier this frame that a Draw samples, into `RmlRenderPlan::texture_reads`
  u32 first_read = 0;
  u32 read_count = 0;
  // Composite, SaveTexture: x, y, w, h
  glm::ivec4 scissor = {};
};

// what RmlUi asked for in one frame. its structure, everything `signature` writes, picks the program that draws
// it, the rest is read while that program runs
struct RmlRenderPlan {
  glm::ivec2 extent = {};
  std::vector<RmlPass> passes = {};
  std::vector<RmlDrawCmd> draws = {};
  std::vector<RmlFilter> filters = {};
  std::vector<RmlSavedTextureID> texture_reads = {};
  // by RmlSavedTextureID
  std::vector<Rml::TextureHandle> saved_textures = {};
  u16 layer_count = 1;
  u16 mask_count = 0;
  bool uses_stencil = false;

  auto signature(this const RmlRenderPlan& self, std::vector<u32>& out) -> void;
};

class RmlPlanBuilder {
public:
  auto begin(this RmlPlanBuilder& self, glm::ivec2 extent) -> void;

  auto set_scissor_enabled(this RmlPlanBuilder& self, bool is_enabled) -> void;
  auto set_scissor(this RmlPlanBuilder& self, glm::ivec4 region) -> void;
  // the context when scissoring is off, clamped to it either way
  auto scissor(this const RmlPlanBuilder& self) -> glm::ivec4;

  // `cmd.scissor` is filled in here
  auto draw(this RmlPlanBuilder& self, RmlDrawCmd cmd) -> void;
  auto set_clip_mask_enabled(this RmlPlanBuilder& self, bool is_enabled) -> void;
  auto clip(this RmlPlanBuilder& self, Rml::ClipMaskOperation operation, RmlDrawCmd cmd) -> void;

  auto push_layer(this RmlPlanBuilder& self) -> RmlLayerID;
  auto composite(
    this RmlPlanBuilder& self,
    RmlLayerID source,
    RmlLayerID destination,
    Rml::BlendMode blend_mode,
    std::span<const RmlFilter> filters
  ) -> void;
  auto pop_layer(this RmlPlanBuilder& self) -> void;
  auto top_layer(this const RmlPlanBuilder& self) -> RmlLayerID;

  // `texture` is the persistent texture the pass copies the top layer's scissor region into
  auto save_texture(this RmlPlanBuilder& self, Rml::TextureHandle texture) -> RmlSavedTextureID;
  auto save_mask(this RmlPlanBuilder& self) -> RmlMaskID;

  RmlRenderPlan plan = {};

private:
  auto stencil_test(this const RmlPlanBuilder& self) -> u8;

  ankerl::svector<RmlLayerID, 8> layer_stack = {};
  glm::ivec4 scissor_region = {};
  bool is_scissor_enabled = false;
  bool is_clip_enabled = false;
  // what the clip mask holds inside its area, so what draws compare against
  u8 clip_reference = 0;
};
} // namespace ox
