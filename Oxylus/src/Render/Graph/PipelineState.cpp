#include "Render/Graph/PipelineState.hpp"

#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <bit>
#include <fmt/format.h>

#include "Render/Graph/Types.hpp"
#include "Utils/Log.hpp"

namespace ox::vir {
struct HashBuilder {
  u64 value = 0x9e3779b97f4a7c15_u64;

  auto add(this HashBuilder& self, u64 word) -> HashBuilder& {
    self.value = ankerl::unordered_dense::detail::wyhash::mix(self.value ^ word, 0xe7037ed1a0b428db_u64);
    return self;
  }

  auto add_f32(this HashBuilder& self, f32 word) -> HashBuilder& { return self.add(std::bit_cast<u32>(word)); }

  auto add_bytes(this HashBuilder& self, const void* data, usize size) -> HashBuilder& {
    return self.add(ankerl::unordered_dense::detail::wyhash::hash(data, size));
  }
};

static auto bits_equal(f32 lhs, f32 rhs) -> bool { return std::bit_cast<u32>(lhs) == std::bit_cast<u32>(rhs); }

auto Rect2D::resolve(this const Rect2D& self, const VkRect2D& area) -> VkRect2D {
  switch (self.kind) {
    case Kind::Framebuffer: return area;
    case Kind::Absolute   : return self.absolute;
    case Kind::Relative:
      return {
        .offset =
          {
            .x = area.offset.x + static_cast<i32>(static_cast<f32>(area.extent.width) * self.x),
            .y = area.offset.y + static_cast<i32>(static_cast<f32>(area.extent.height) * self.y),
          },
        .extent = {
          .width = static_cast<u32>(static_cast<f32>(area.extent.width) * self.width),
          .height = static_cast<u32>(static_cast<f32>(area.extent.height) * self.height),
        },
      };
  }

  return area;
}

auto Rect2D::operator==(const Rect2D& other) const -> bool {
  if (kind != other.kind) {
    return false;
  }

  switch (kind) {
    case Kind::Framebuffer: return true;
    case Kind::Absolute:
      return absolute.offset.x == other.absolute.offset.x && absolute.offset.y == other.absolute.offset.y &&
             absolute.extent.width == other.absolute.extent.width &&
             absolute.extent.height == other.absolute.extent.height;
    case Kind::Relative:
      return bits_equal(x, other.x) && bits_equal(y, other.y) && bits_equal(width, other.width) &&
             bits_equal(height, other.height);
  }

  return false;
}

auto ResolvedViewport::to_vk(this const ResolvedViewport& self) -> VkViewport {
  return {
    .x = self.x,
    .y = self.y,
    .width = self.width,
    .height = self.height,
    .minDepth = self.min_depth,
    .maxDepth = self.max_depth,
  };
}

auto ResolvedViewport::operator==(const ResolvedViewport& other) const -> bool {
  return bits_equal(x, other.x) && bits_equal(y, other.y) && bits_equal(width, other.width) &&
         bits_equal(height, other.height) && bits_equal(min_depth, other.min_depth) &&
         bits_equal(max_depth, other.max_depth);
}

auto Viewport::resolve(this const Viewport& self, const VkRect2D& area) -> ResolvedViewport {
  const auto rect = self.rect.resolve(area);
  return {
    .x = static_cast<f32>(rect.offset.x),
    .y = static_cast<f32>(rect.offset.y),
    .width = static_cast<f32>(rect.extent.width),
    .height = static_cast<f32>(rect.extent.height),
    .min_depth = self.min_depth,
    .max_depth = self.max_depth,
  };
}

auto Viewport::operator==(const Viewport& other) const -> bool {
  return rect == other.rect && bits_equal(min_depth, other.min_depth) && bits_equal(max_depth, other.max_depth);
}

auto StencilState::operator==(const StencilState& other) const -> bool {
  return is_test_enabled == other.is_test_enabled && std::memcmp(&front, &other.front, sizeof(front)) == 0 &&
         std::memcmp(&back, &other.back, sizeof(back)) == 0;
}

auto RasterizationState::operator==(const RasterizationState& other) const -> bool {
  return polygon_mode == other.polygon_mode && cull_mode == other.cull_mode && front_face == other.front_face &&
         bits_equal(line_width, other.line_width) && is_depth_clamp_enabled == other.is_depth_clamp_enabled &&
         is_rasterizer_discard_enabled == other.is_rasterizer_discard_enabled &&
         is_depth_bias_enabled == other.is_depth_bias_enabled &&
         bits_equal(depth_bias_constant, other.depth_bias_constant) &&
         bits_equal(depth_bias_clamp, other.depth_bias_clamp) && bits_equal(depth_bias_slope, other.depth_bias_slope) &&
         is_conservative == other.is_conservative;
}

auto ColorBlendAttachmentState::to_vk(this const ColorBlendAttachmentState& self)
  -> VkPipelineColorBlendAttachmentState {
  return {
    .blendEnable = self.is_blend_enabled ? VK_TRUE : VK_FALSE,
    .srcColorBlendFactor = self.src_color_blend_factor,
    .dstColorBlendFactor = self.dst_color_blend_factor,
    .colorBlendOp = self.color_blend_op,
    .srcAlphaBlendFactor = self.src_alpha_blend_factor,
    .dstAlphaBlendFactor = self.dst_alpha_blend_factor,
    .alphaBlendOp = self.alpha_blend_op,
    .colorWriteMask = self.color_write_mask,
  };
}

auto blend_preset(BlendPreset preset) -> ColorBlendAttachmentState {
  auto state = ColorBlendAttachmentState{.is_blend_enabled = preset != BlendPreset::Off};
  switch (preset) {
    case BlendPreset::Off: break;
    case BlendPreset::AlphaBlend:
      state.src_color_blend_factor = VK_BLEND_FACTOR_SRC_ALPHA;
      state.dst_color_blend_factor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      state.src_alpha_blend_factor = VK_BLEND_FACTOR_ONE;
      state.dst_alpha_blend_factor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      break;
    case BlendPreset::PremultipliedAlphaBlend:
      state.src_color_blend_factor = VK_BLEND_FACTOR_ONE;
      state.dst_color_blend_factor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      state.src_alpha_blend_factor = VK_BLEND_FACTOR_ONE;
      state.dst_alpha_blend_factor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      break;
    case BlendPreset::Additive:
      state.dst_color_blend_factor = VK_BLEND_FACTOR_ONE;
      state.dst_alpha_blend_factor = VK_BLEND_FACTOR_ONE;
      break;
  }

  return state;
}

auto PushConstants::reserve(this PushConstants& self, u32 at, u32 byte_size) -> void {
  OX_CHECK_LE(at, ~0_u32 - byte_size);
  if (self.is_empty()) {
    self.offset = at;
    self.data.resize(byte_size, 0);
    return;
  }

  const auto start = std::min(self.offset, at);
  const auto end = std::max(self.end(), at + byte_size);
  if (start != self.offset || end != self.end()) {
    auto widened = std::vector<u8>(end - start, 0);
    std::ranges::copy(self.data, widened.begin() + (self.offset - start));
    self.offset = start;
    self.data = std::move(widened);
  }
}

auto PushConstants::address(this PushConstants& self, u32 at, ValueID buffer) -> void {
  self.reserve(at, sizeof(VkDeviceAddress));
  std::erase_if(self.addresses, [&](const auto& entry) { return entry.first == at; });
  self.addresses.emplace_back(at, buffer);
}

auto PushConstants::from_variable(this PushConstants& self, u32 at, u32 byte_size, ValueID variable) -> void {
  // one block can have an address prefix and several independently updated byte ranges
  self.reserve(at, byte_size);
  std::erase_if(self.variables, [&](const Variable& entry) -> bool {
    return at <= entry.offset && at + byte_size >= entry.offset + entry.size;
  });
  self.variables.push_back({.offset = at, .size = byte_size, .source = variable});
}

auto PushConstants::write(this PushConstants& self, u32 at, std::span<const u8> bytes) -> void {
  if (bytes.empty()) {
    return;
  }

  if (!self.variables.empty()) {
    self.variables.clear();
    self.data.clear();
    for (const auto& [address_offset, buffer] : self.addresses) {
      self.reserve(address_offset, sizeof(VkDeviceAddress));
    }
  }

  self.reserve(at, static_cast<u32>(bytes.size()));
  std::ranges::copy(bytes, self.data.begin() + (at - self.offset));
}

auto SpecValue::from(f32 value) -> SpecValue { return {.kind = Kind::F32, .bits = std::bit_cast<u32>(value)}; }

auto SpecValue::from(f64 value) -> SpecValue { return {.kind = Kind::F64, .bits = std::bit_cast<u64>(value)}; }

auto SpecValue::size(this const SpecValue& self) -> u32 { return self.kind == Kind::F64 ? 8 : 4; }

auto SpecValue::bytes(this const SpecValue& self) -> std::array<u8, 8> {
  auto result = std::array<u8, 8>{};
  std::memcpy(result.data(), &self.bits, sizeof(self.bits));
  return result;
}

auto SpecValue::as_workgroup_axis(this const SpecValue& self) -> option<u32> {
  switch (self.kind) {
    case Kind::U32: return static_cast<u32>(self.bits);
    case Kind::I32: {
      const auto value = static_cast<i32>(static_cast<u32>(self.bits));
      if (value < 0) {
        return nullopt;
      }

      return static_cast<u32>(value);
    }
    default: return nullopt;
  }
}

auto SpecValue::to_string(this const SpecValue& self) -> std::string {
  switch (self.kind) {
    case Kind::Bool: return self.bits != 0 ? "true" : "false";
    case Kind::U32 : return fmt::format("{}", static_cast<u32>(self.bits));
    case Kind::I32 : return fmt::format("{}", static_cast<i32>(static_cast<u32>(self.bits)));
    case Kind::F32 : return fmt::format("{}", std::bit_cast<f32>(static_cast<u32>(self.bits)));
    case Kind::F64 : return fmt::format("{}", std::bit_cast<f64>(self.bits));
  }

  return {};
}

auto Specialization::get(this const Specialization& self, u32 id) -> option<SpecValue> {
  const auto it = std::ranges::lower_bound(self.values, id, {}, &std::pair<u32, SpecValue>::first);
  if (it == self.values.end() || it->first != id) {
    return nullopt;
  }

  return it->second;
}

auto Specialization::set(this Specialization& self, u32 id, SpecValue value) -> void {
  const auto it = std::ranges::lower_bound(self.values, id, {}, &std::pair<u32, SpecValue>::first);
  if (it != self.values.end() && it->first == id) {
    it->second = value;
    return;
  }

  self.values.insert(it, {id, value});
}

auto SpecializationHash::operator()(const Specialization& specialization) const noexcept -> u64 {
  auto builder = HashBuilder{};
  for (const auto& [id, value] : specialization.values) {
    builder.add(id).add(static_cast<u64>(value.kind)).add(value.bits);
  }

  return builder.value;
}

auto PipelineState::dynamic_states(this const PipelineState& self) -> std::vector<VkDynamicState> {
  auto states = std::vector<VkDynamicState>{};
  if (contains(self.dynamic, DynamicStateFlags::Viewport)) {
    states.push_back(VK_DYNAMIC_STATE_VIEWPORT);
  }

  if (contains(self.dynamic, DynamicStateFlags::Scissor)) {
    states.push_back(VK_DYNAMIC_STATE_SCISSOR);
  }

  if (contains(self.dynamic, DynamicStateFlags::DepthBias)) {
    states.push_back(VK_DYNAMIC_STATE_DEPTH_BIAS);
  }

  return states;
}

auto PipelineState::hash(this const PipelineState& self) -> u64 {
  auto builder = HashBuilder{};
  for (const auto format : self.rendering.color_formats) {
    builder.add(std::to_underlying(format));
  }

  builder.add(std::to_underlying(self.rendering.depth_format))
    .add(std::to_underlying(self.rendering.stencil_format))
    .add(std::to_underlying(self.rendering.samples))
    .add(self.rendering.view_mask)
    .add(self.topology)
    .add(self.patch_control_points)
    .add(self.rasterization.polygon_mode)
    .add(self.rasterization.cull_mode)
    .add(self.rasterization.front_face)
    .add_f32(self.rasterization.line_width)
    .add(self.rasterization.is_depth_clamp_enabled)
    .add(self.rasterization.is_rasterizer_discard_enabled)
    .add(self.rasterization.is_depth_bias_enabled)
    .add_f32(self.rasterization.depth_bias_constant)
    .add_f32(self.rasterization.depth_bias_slope)
    .add(self.rasterization.is_conservative);
  for (const auto& blend : self.blend) {
    builder.add(blend.is_blend_enabled)
      .add(blend.src_color_blend_factor)
      .add(blend.dst_color_blend_factor)
      .add(blend.color_blend_op)
      .add(blend.src_alpha_blend_factor)
      .add(blend.dst_alpha_blend_factor)
      .add(blend.alpha_blend_op)
      .add(blend.color_write_mask);
  }

  builder.add(self.depth.is_test_enabled).add(self.depth.is_write_enabled).add(self.depth.compare_op);
  builder.add(self.stencil.is_test_enabled)
    .add_bytes(&self.stencil.front, sizeof(self.stencil.front))
    .add_bytes(&self.stencil.back, sizeof(self.stencil.back));
  builder.add(self.viewport_count).add(std::to_underlying(self.dynamic));
  for (const auto& viewport : self.viewports) {
    builder.add_f32(viewport.x).add_f32(viewport.y).add_f32(viewport.width).add_f32(viewport.height);
  }

  for (const auto& scissor : self.scissors) {
    builder.add(static_cast<u32>(scissor.offset.x))
      .add(static_cast<u32>(scissor.offset.y))
      .add(scissor.extent.width)
      .add(scissor.extent.height);
  }

  builder.add(SpecializationHash{}(self.specialization));
  return builder.value;
}

auto PipelineState::operator==(const PipelineState& other) const -> bool {
  if (viewports.size() != other.viewports.size() || scissors.size() != other.scissors.size()) {
    return false;
  }

  for (usize index = 0; index < scissors.size(); index++) {
    const auto& lhs = scissors[index];
    const auto& rhs = other.scissors[index];
    if (
      lhs.offset.x != rhs.offset.x || lhs.offset.y != rhs.offset.y || lhs.extent.width != rhs.extent.width ||
      lhs.extent.height != rhs.extent.height
    ) {
      return false;
    }
  }

  return rendering == other.rendering && topology == other.topology &&
         patch_control_points == other.patch_control_points && rasterization == other.rasterization &&
         blend == other.blend && depth == other.depth && stencil == other.stencil &&
         viewport_count == other.viewport_count && viewports == other.viewports && dynamic == other.dynamic &&
         specialization == other.specialization;
}

auto DynamicValues::resolved_viewports(this const DynamicValues& self, const VkRect2D& area)
  -> std::vector<ResolvedViewport> {
  auto resolved = std::vector<ResolvedViewport>{};
  resolved.reserve(self.viewports.size());
  for (const auto& viewport : self.viewports) {
    resolved.push_back(viewport.resolve(area));
  }

  return resolved;
}

auto DynamicValues::resolved_scissors(this const DynamicValues& self, const VkRect2D& area) -> std::vector<VkRect2D> {
  auto resolved = std::vector<VkRect2D>{};
  resolved.reserve(self.scissors.size());
  for (const auto& scissor : self.scissors) {
    resolved.push_back(scissor.resolve(area));
  }

  return resolved;
}

auto PassState::for_rendering(RenderingState rendering) -> PassState {
  auto state = PassState{};
  state.blend.resize(rendering.color_formats.size());
  state.rendering = std::move(rendering);
  return state;
}

template <typename T>
static auto slot(std::vector<T>& slots, u32 index) -> T& {
  if (index >= slots.size()) {
    slots.resize(index + 1);
  }

  return slots[index];
}

auto PassState::apply(this PassState& self, const StateChange& change) -> void {
  std::visit(
    [&](const auto& value) {
      using T = std::decay_t<decltype(value)>;
      if constexpr (std::is_same_v<T, state::PrimitiveTopology>) {
        self.topology = value.topology;
      } else if constexpr (std::is_same_v<T, state::Rasterization>) {
        self.rasterization = value.rasterization;
      } else if constexpr (std::is_same_v<T, state::Dynamic>) {
        self.dynamic = value.flags;
      } else if constexpr (std::is_same_v<T, state::SetViewport>) {
        slot(self.viewports, value.index) = value.viewport;
      } else if constexpr (std::is_same_v<T, state::SetScissor>) {
        slot(self.scissors, value.index) = value.rect;
      } else if constexpr (std::is_same_v<T, state::ColorBlend>) {
        if (value.index.has_value()) {
          slot(self.blend, *value.index) = value.blend;
        } else if (self.blend.empty()) {
          self.blend.push_back(value.blend);
        } else {
          std::ranges::fill(self.blend, value.blend);
        }
      } else if constexpr (std::is_same_v<T, state::Depth>) {
        self.depth = value.depth;
      } else if constexpr (std::is_same_v<T, state::Stencil>) {
        self.stencil = value.stencil;
      } else if constexpr (std::is_same_v<T, state::PatchControlPoints>) {
        self.patch_control_points = value.count;
      } else if constexpr (std::is_same_v<T, state::Push>) {
        self.push_constants.write(value.offset, value.data);
      } else if constexpr (std::is_same_v<T, state::PushFrom>) {
        self.push_constants.from_variable(value.offset, value.size, value.source);
      } else if constexpr (std::is_same_v<T, state::PushAddress>) {
        self.push_constants.address(value.offset, value.buffer);
      } else if constexpr (std::is_same_v<T, state::SpecializationConstant>) {
        self.specialization.set(value.id, value.value);
      }
    },
    change
  );
}

auto PassState::resolve(this const PassState& self, const VkRect2D& area) -> std::pair<PipelineState, DynamicValues> {
  const auto count = std::max({self.viewports.size(), self.scissors.size(), 1_sz});
  const auto viewport_at = [&](usize index) {
    return index < self.viewports.size() ? self.viewports[index] : Viewport{};
  };
  const auto scissor_at = [&](usize index) {
    return index < self.scissors.size() ? self.scissors[index] : Rect2D{};
  };

  auto pipeline_state = PipelineState{
    .rendering = self.rendering,
    .topology = self.topology,
    .patch_control_points = self.patch_control_points,
    .rasterization = self.rasterization,
    // a region without a depth attachment cannot test against one, so the state is dropped rather
    // than handed to a pipeline that would fail validation
    .depth = self.rendering.depth_format != Format::Undefined ? self.depth : DepthState{},
    .stencil = self.rendering.stencil_format != Format::Undefined ? self.stencil : StencilState{},
    .viewport_count = static_cast<u32>(count),
    .dynamic = self.dynamic,
    .specialization = self.specialization,
  };
  for (usize index = 0; index < self.rendering.color_formats.size(); index++) {
    pipeline_state.blend.push_back(index < self.blend.size() ? self.blend[index] : ColorBlendAttachmentState{});
  }

  auto values = DynamicValues{.push_constants = self.push_constants};
  for (usize index = 0; index < count; index++) {
    if (contains(self.dynamic, DynamicStateFlags::Viewport)) {
      values.viewports.push_back(viewport_at(index));
    } else {
      pipeline_state.viewports.push_back(viewport_at(index).resolve(area));
    }

    if (contains(self.dynamic, DynamicStateFlags::Scissor)) {
      values.scissors.push_back(scissor_at(index));
    } else {
      pipeline_state.scissors.push_back(scissor_at(index).resolve(area));
    }
  }

  return {std::move(pipeline_state), std::move(values)};
}
} // namespace ox::vir
