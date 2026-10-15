#pragma once

#include <array>
#include <cstring>
#include <span>
#include <string>
#include <variant>
#include <vector>
#include <vulkan/vulkan_core.h>

#include "Core/Option.hpp"
#include "Render/Graph/Fwd.hpp"
#include "Render/Graph/Types.hpp"

namespace ox::vir {
enum class DynamicStateFlags : u32 {
  None = 0,
  Viewport = 1 << 0,
  Scissor = 1 << 1,
  DepthBias = 1 << 2,
};
OX_VIR_FLAGS(DynamicStateFlags)

struct Rect2D {
  enum class Kind : u8 { Framebuffer, Absolute, Relative };

  Kind kind = Kind::Framebuffer;
  VkRect2D absolute = {};
  f32 x = 0.0f;
  f32 y = 0.0f;
  f32 width = 1.0f;
  f32 height = 1.0f;

  static constexpr auto framebuffer() -> Rect2D { return {}; }
  static constexpr auto from_absolute(i32 x, i32 y, u32 width, u32 height) -> Rect2D {
    return {.kind = Kind::Absolute, .absolute = {.offset = {x, y}, .extent = {width, height}}};
  }
  static constexpr auto relative(f32 x, f32 y, f32 width, f32 height) -> Rect2D {
    return {.kind = Kind::Relative, .x = x, .y = y, .width = width, .height = height};
  }

  auto resolve(this const Rect2D& self, const VkRect2D& area) -> VkRect2D;
  auto operator==(const Rect2D& other) const -> bool;
};

struct ResolvedViewport {
  f32 x = 0.0f;
  f32 y = 0.0f;
  f32 width = 0.0f;
  f32 height = 0.0f;
  f32 min_depth = 0.0f;
  f32 max_depth = 1.0f;

  auto to_vk(this const ResolvedViewport& self) -> VkViewport;
  auto operator==(const ResolvedViewport& other) const -> bool;
};

struct Viewport {
  Rect2D rect = {};
  f32 min_depth = 0.0f;
  f32 max_depth = 1.0f;

  static constexpr auto framebuffer() -> Viewport { return {}; }

  auto resolve(this const Viewport& self, const VkRect2D& area) -> ResolvedViewport;
  auto operator==(const Viewport& other) const -> bool;
};

struct RenderingState {
  std::vector<Format> color_formats = {};
  Format depth_format = Format::Undefined;
  Format stencil_format = Format::Undefined;
  Samples samples = Samples::e1;
  u32 view_mask = 0;

  auto operator==(const RenderingState&) const -> bool = default;
};

struct DepthState {
  bool is_test_enabled = false;
  bool is_write_enabled = false;
  VkCompareOp compare_op = VK_COMPARE_OP_ALWAYS;

  static constexpr auto testing(VkCompareOp op) -> DepthState {
    return {.is_test_enabled = true, .is_write_enabled = true, .compare_op = op};
  }
  static constexpr auto less() -> DepthState { return testing(VK_COMPARE_OP_LESS); }
  static constexpr auto greater() -> DepthState { return testing(VK_COMPARE_OP_GREATER); }
  static constexpr auto greater_or_equal() -> DepthState { return testing(VK_COMPARE_OP_GREATER_OR_EQUAL); }
  static constexpr auto read_only(VkCompareOp op) -> DepthState {
    return {.is_test_enabled = true, .is_write_enabled = false, .compare_op = op};
  }

  auto operator==(const DepthState&) const -> bool = default;
};

struct StencilState {
  bool is_test_enabled = false;
  VkStencilOpState front = {};
  VkStencilOpState back = {};

  auto operator==(const StencilState& other) const -> bool;
};

struct RasterizationState {
  VkPolygonMode polygon_mode = VK_POLYGON_MODE_FILL;
  VkCullModeFlags cull_mode = VK_CULL_MODE_NONE;
  VkFrontFace front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  f32 line_width = 1.0f;
  bool is_depth_clamp_enabled = false;
  bool is_rasterizer_discard_enabled = false;
  bool is_depth_bias_enabled = false;
  f32 depth_bias_constant = 0.0f;
  f32 depth_bias_clamp = 0.0f;
  f32 depth_bias_slope = 0.0f;
  bool is_conservative = false;

  auto operator==(const RasterizationState& other) const -> bool;
};

struct ColorBlendAttachmentState {
  bool is_blend_enabled = false;
  VkBlendFactor src_color_blend_factor = VK_BLEND_FACTOR_ONE;
  VkBlendFactor dst_color_blend_factor = VK_BLEND_FACTOR_ZERO;
  VkBlendOp color_blend_op = VK_BLEND_OP_ADD;
  VkBlendFactor src_alpha_blend_factor = VK_BLEND_FACTOR_ONE;
  VkBlendFactor dst_alpha_blend_factor = VK_BLEND_FACTOR_ZERO;
  VkBlendOp alpha_blend_op = VK_BLEND_OP_ADD;
  VkColorComponentFlags color_write_mask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

  auto to_vk(this const ColorBlendAttachmentState& self) -> VkPipelineColorBlendAttachmentState;
  auto operator==(const ColorBlendAttachmentState&) const -> bool = default;
};

enum class BlendPreset : u8 { Off, AlphaBlend, PremultipliedAlphaBlend, Additive };

auto blend_preset(BlendPreset preset) -> ColorBlendAttachmentState;

struct PushConstants {
  struct Variable {
    u32 offset = 0;
    u32 size = 0;
    ValueID source = ValueID::Invalid;

    auto operator==(const Variable&) const -> bool = default;
  };

  u32 offset = 0;
  std::vector<u8> data = {};
  std::vector<Variable> variables = {};
  // device addresses of these buffers are patched over `data` when the module runs, a transient
  // buffer has no address until then
  std::vector<std::pair<u32, ValueID>> addresses = {};

  auto is_empty(this const PushConstants& self) -> bool { return self.data.empty(); }
  auto size(this const PushConstants& self) -> u32 { return static_cast<u32>(self.data.size()); }
  auto end(this const PushConstants& self) -> u32 { return self.offset + self.size(); }
  auto reserve(this PushConstants& self, u32 at, u32 byte_size) -> void;
  auto address(this PushConstants& self, u32 at, ValueID buffer) -> void;
  auto from_variable(this PushConstants& self, u32 at, u32 byte_size, ValueID variable) -> void;
  auto write(this PushConstants& self, u32 at, std::span<const u8> bytes) -> void;

  auto operator==(const PushConstants&) const -> bool = default;
};

struct SpecValue {
  enum class Kind : u8 { Bool, U32, I32, F32, F64 };

  Kind kind = Kind::U32;
  u64 bits = 0;

  static constexpr auto from(bool value) -> SpecValue { return {.kind = Kind::Bool, .bits = value ? 1_u64 : 0_u64}; }
  static constexpr auto from(u32 value) -> SpecValue { return {.kind = Kind::U32, .bits = value}; }
  static constexpr auto from(i32 value) -> SpecValue { return {.kind = Kind::I32, .bits = static_cast<u32>(value)}; }
  static auto from(f32 value) -> SpecValue;
  static auto from(f64 value) -> SpecValue;

  auto size(this const SpecValue& self) -> u32;
  // little-endian bytes, the first `size()` of them are what the shader reads
  auto bytes(this const SpecValue& self) -> std::array<u8, 8>;
  auto as_workgroup_axis(this const SpecValue& self) -> option<u32>;
  auto to_string(this const SpecValue& self) -> std::string;

  auto operator==(const SpecValue&) const -> bool = default;
};

// sorted by constant id, a pipeline reached with different constants is a different pipeline
struct Specialization {
  std::vector<std::pair<u32, SpecValue>> values = {};

  auto is_empty(this const Specialization& self) -> bool { return self.values.empty(); }
  auto get(this const Specialization& self, u32 id) -> option<SpecValue>;
  auto set(this Specialization& self, u32 id, SpecValue value) -> void;

  auto operator==(const Specialization&) const -> bool = default;
};

namespace state {
struct PrimitiveTopology {
  VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  auto operator==(const PrimitiveTopology&) const -> bool = default;
};
struct Rasterization {
  RasterizationState rasterization = {};
  auto operator==(const Rasterization&) const -> bool = default;
};
struct Dynamic {
  DynamicStateFlags flags = DynamicStateFlags::None;
  auto operator==(const Dynamic&) const -> bool = default;
};
struct SetViewport {
  u32 index = 0;
  Viewport viewport = {};
  auto operator==(const SetViewport&) const -> bool = default;
};
struct SetScissor {
  u32 index = 0;
  Rect2D rect = {};
  auto operator==(const SetScissor&) const -> bool = default;
};
struct ColorBlend {
  option<u32> index = nullopt;
  ColorBlendAttachmentState blend = {};
  auto operator==(const ColorBlend& other) const -> bool {
    return index.has_value() == other.index.has_value() && (!index.has_value() || *index == *other.index) &&
           blend == other.blend;
  }
};
struct Depth {
  DepthState depth = {};
  auto operator==(const Depth&) const -> bool = default;
};
struct Stencil {
  StencilState stencil = {};
  auto operator==(const Stencil&) const -> bool = default;
};
struct PatchControlPoints {
  u32 count = 3;
  auto operator==(const PatchControlPoints&) const -> bool = default;
};
struct Push {
  u32 offset = 0;
  std::vector<u8> data = {};
  auto operator==(const Push&) const -> bool = default;
};
struct PushFrom {
  u32 offset = 0;
  u32 size = 0;
  ValueID source = ValueID::Invalid;
  auto operator==(const PushFrom&) const -> bool = default;
};
struct PushAddress {
  u32 offset = 0;
  ValueID buffer = ValueID::Invalid;
  auto operator==(const PushAddress&) const -> bool = default;
};
struct SpecializationConstant {
  u32 id = 0;
  SpecValue value = {};
  auto operator==(const SpecializationConstant&) const -> bool = default;
};
} // namespace state

using StateChange = std::variant<
  state::PrimitiveTopology,
  state::Rasterization,
  state::Dynamic,
  state::SetViewport,
  state::SetScissor,
  state::ColorBlend,
  state::Depth,
  state::Stencil,
  state::PatchControlPoints,
  state::Push,
  state::PushFrom,
  state::PushAddress,
  state::SpecializationConstant>;

struct PipelineState {
  RenderingState rendering = {};
  VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  u32 patch_control_points = 0;
  RasterizationState rasterization = {};
  std::vector<ColorBlendAttachmentState> blend = {};
  DepthState depth = {};
  StencilState stencil = {};
  u32 viewport_count = 1;
  std::vector<ResolvedViewport> viewports = {};
  std::vector<VkRect2D> scissors = {};
  DynamicStateFlags dynamic = DynamicStateFlags::Viewport | DynamicStateFlags::Scissor;
  Specialization specialization = {};

  auto dynamic_states(this const PipelineState& self) -> std::vector<VkDynamicState>;
  auto hash(this const PipelineState& self) -> u64;
  auto operator==(const PipelineState& other) const -> bool;
};

struct PipelineStateHash {
  using is_avalanching = void;
  auto operator()(const PipelineState& state) const noexcept -> u64 { return state.hash(); }
};

struct SpecializationHash {
  using is_avalanching = void;
  auto operator()(const Specialization& specialization) const noexcept -> u64;
};

struct DynamicValues {
  std::vector<Viewport> viewports = {};
  std::vector<Rect2D> scissors = {};
  PushConstants push_constants = {};

  auto resolved_viewports(this const DynamicValues& self, const VkRect2D& area) -> std::vector<ResolvedViewport>;
  auto resolved_scissors(this const DynamicValues& self, const VkRect2D& area) -> std::vector<VkRect2D>;
  auto operator==(const DynamicValues&) const -> bool = default;
};

// what draws and dispatches inherit from the state changes before them in a pass
struct PassState {
  RenderingState rendering = {};
  VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  u32 patch_control_points = 0;
  RasterizationState rasterization = {};
  std::vector<ColorBlendAttachmentState> blend = {};
  DepthState depth = {};
  StencilState stencil = {};
  std::vector<Viewport> viewports = {Viewport{}};
  std::vector<Rect2D> scissors = {Rect2D{}};
  DynamicStateFlags dynamic = DynamicStateFlags::Viewport | DynamicStateFlags::Scissor;
  PushConstants push_constants = {};
  Specialization specialization = {};

  static auto for_rendering(RenderingState rendering) -> PassState;

  auto apply(this PassState& self, const StateChange& change) -> void;
  auto resolve(this const PassState& self, const VkRect2D& area) -> std::pair<PipelineState, DynamicValues>;
};
} // namespace ox::vir
