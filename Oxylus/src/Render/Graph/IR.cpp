#include "Render/Graph/IR.hpp"

#include <algorithm>
#include <cmath>
#include <fmt/format.h>

#include "Render/Graph/Types.hpp"
#include "Utils/Log.hpp"

namespace ox::vir {
auto Subresource::overlaps(this const Subresource& self, const Subresource& other) -> bool {
  const auto levels = self.base_level < other.level_end() && other.base_level < self.level_end();
  const auto layers = self.base_layer < other.layer_end() && other.base_layer < self.layer_end();
  return levels && layers;
}

auto Subresource::intersect(this const Subresource& self, const Subresource& other) -> Subresource {
  const auto base_level = std::max(self.base_level, other.base_level);
  const auto level_end = std::min(self.level_end(), other.level_end());
  const auto base_layer = std::max(self.base_layer, other.base_layer);
  const auto layer_end = std::min(self.layer_end(), other.layer_end());
  return {
    .base_level = base_level,
    .level_count = level_end == REMAINING ? REMAINING : static_cast<u32>(level_end - base_level),
    .base_layer = base_layer,
    .layer_count = layer_end == REMAINING ? REMAINING : static_cast<u32>(layer_end - base_layer),
  };
}

auto Subresource::contains(this const Subresource& self, const Subresource& other) -> bool {
  return self.base_level <= other.base_level && self.level_end() >= other.level_end() &&
         self.base_layer <= other.base_layer && self.layer_end() >= other.layer_end();
}

auto Subresource::apply(this const Subresource& self, const VkImageSubresourceRange& range) -> VkImageSubresourceRange {
  const auto clamp_count = [](u32 base, u32 count, u32 available) -> u32 {
    if (base >= available) {
      return 0;
    }

    return count == REMAINING ? available - base : std::min(count, available - base);
  };

  return {
    .aspectMask = range.aspectMask,
    .baseMipLevel = range.baseMipLevel + self.base_level,
    .levelCount = clamp_count(self.base_level, self.level_count, range.levelCount),
    .baseArrayLayer = range.baseArrayLayer + self.base_layer,
    .layerCount = clamp_count(self.base_layer, self.layer_count, range.layerCount),
  };
}

namespace ir {
auto Constant::from_i32(i32 value) -> Constant {
  return {.kind = Kind::I32, .words = {std::bit_cast<u32>(value), 0, 0, 0}};
}

auto Constant::from_u32(u32 value) -> Constant { return {.kind = Kind::U32, .words = {value, 0, 0, 0}}; }

auto Constant::from_u64(u64 value) -> Constant {
  return {.kind = Kind::U64, .words = {static_cast<u32>(value), static_cast<u32>(value >> 32), 0, 0}};
}

auto Constant::from_extent(Extent2D value) -> Constant {
  return {.kind = Kind::Extent2D, .words = {value.width, value.height, 0, 0}};
}

auto Constant::from_extent(Extent3D value) -> Constant {
  return {.kind = Kind::Extent3D, .words = {value.width, value.height, value.depth, 0}};
}

auto Constant::from_string(std::string value) -> Constant { return {.kind = Kind::String, .string = std::move(value)}; }

auto Constant::from_access(Access value) -> Constant {
  const auto bits = std::to_underlying(value);
  return {.kind = Kind::Access, .words = {static_cast<u32>(bits), static_cast<u32>(bits >> 32), 0, 0}};
}

auto Constant::from_clear(const ClearValue& value) -> Constant {
  auto constant = Constant{.kind = Kind::ClearValue};
  std::memcpy(constant.words.data(), &value.value, sizeof(VkClearValue));
  return constant;
}

auto Constant::as_clear(this const Constant& self) -> ClearValue {
  auto value = ClearValue{};
  std::memcpy(&value.value, self.words.data(), sizeof(VkClearValue));
  return value;
}

auto Constant::hash(this const Constant& self) -> u64 {
  auto hash = ankerl::unordered_dense::detail::wyhash::hash(self.words.data(), sizeof(self.words));
  hash ^= static_cast<u64>(self.kind) * 0x9e3779b97f4a7c15_u64;
  if (!self.string.empty()) {
    hash ^= ankerl::unordered_dense::detail::wyhash::hash(self.string.data(), self.string.size());
  }

  return hash;
}

auto TypeHash::operator()(const Type& type) const noexcept -> u64 {
  const u32 words[] = {static_cast<u32>(type.kind), static_cast<u32>(type.format), static_cast<u32>(type.samples)};
  return ankerl::unordered_dense::detail::wyhash::hash(words, sizeof(words));
}

auto to_string(VariableKind kind) -> std::string {
  switch (kind.tag) {
    case VariableKindTag::Bool                       : return "bool";
    case VariableKindTag::I32                        : return "i32";
    case VariableKindTag::U32                        : return "u32";
    case VariableKindTag::Extent2D                   : return "extent2d";
    case VariableKindTag::Extent3D                   : return "extent3d";
    case VariableKindTag::ClearValue                 : return "clear";
    case VariableKindTag::Bytes                      : return fmt::format("bytes[{}]", kind.size);
    case VariableKindTag::Swapchain                  : return "swapchain";
    case VariableKindTag::Buffer                     : return "buffer";
    case VariableKindTag::ImageAttachment            : return "image";
    case VariableKindTag::Callback                   : return "callback";
    case VariableKindTag::AccelerationStructureBuilds: return "acceleration structure builds";
    case VariableKindTag::QueryPool                  : return "query pool";
  }

  return "unknown";
}

auto Descriptor::image_value(this const Descriptor& self) -> option<ValueID> {
  if (self.kind == Kind::Image || self.kind == Kind::CombinedImageSampler) {
    return self.image;
  }

  return nullopt;
}

auto Descriptor::buffer_value(this const Descriptor& self) -> option<ValueID> {
  if (self.kind == Kind::TexelBuffer || self.kind == Kind::Buffer || self.kind == Kind::AccelerationStructure) {
    return self.buffer;
  }

  return nullopt;
}

auto Descriptor::resource(this const Descriptor& self) -> option<ValueID> {
  if (const auto image = self.image_value(); image.has_value()) {
    return image;
  }

  return self.buffer_value();
}

auto Descriptor::supports_type(this const Descriptor& self, VkDescriptorType type) -> bool {
  switch (self.kind) {
    case Kind::Sampler: return type == VK_DESCRIPTOR_TYPE_SAMPLER;
    case Kind::Image:
      return type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
             type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    case Kind::CombinedImageSampler: return type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    case Kind::TexelBuffer:
      return type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    case Kind::Buffer: return type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case Kind::AccelerationStructure: return type == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
  }

  return false;
}

auto Descriptor::image_usage(this const Descriptor& self, VkDescriptorType type) -> ImageUsageFlags {
  if (!self.supports_type(type)) {
    return ImageUsageFlags::None;
  }

  switch (type) {
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE         :
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: return ImageUsageFlags::Sampled;
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE         : return ImageUsageFlags::Storage;
    case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT      : return ImageUsageFlags::InputAttachment;
    default                                       : return ImageUsageFlags::None;
  }
}

auto Descriptor::buffer_usage(this const Descriptor& self, VkDescriptorType type) -> VkBufferUsageFlags {
  if (!self.supports_type(type)) {
    return 0;
  }

  switch (type) {
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER      : return VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER      : return VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER            : return VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER            : return VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR: return VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR;
    default                                           : return 0;
  }
}

auto scaled(u64 count, f32 scale) -> u32 {
  const auto result = std::ceil(static_cast<f64>(count) * static_cast<f64>(scale));
  if (!std::isfinite(result)) {
    OX_LOG_WARN("invocation count {} scaled by {} is not a number, it is taken to be none", count, scale);
    return 0;
  }

  return static_cast<u32>(std::clamp(result, 0.0, static_cast<f64>(~0_u32)));
}
} // namespace ir

auto underlying_object(const IR& ir) -> UnderlyingObject {
  return std::visit(
    [](const auto& node) -> UnderlyingObject {
      using T = std::decay_t<decltype(node)>;
      if constexpr (
        std::is_same_v<T, ir::ConstructImage> || std::is_same_v<T, ir::ConstructBuffer> ||
        std::is_same_v<T, ir::SwapchainImage>
      ) {
        return {.kind = UnderlyingKind::Base};
      } else if constexpr (std::is_same_v<T, ir::Array>) {
        if (node.elements.empty()) {
          return {};
        }

        return {.kind = UnderlyingKind::Element, .next = node.elements.front()};
      } else if constexpr (std::is_same_v<T, ir::Index>) {
        return {.kind = UnderlyingKind::Element, .next = node.array};
      } else if constexpr (std::is_same_v<T, ir::View>) {
        return {.kind = UnderlyingKind::Forwards, .next = node.image};
      } else if constexpr (std::is_same_v<T, ir::Clear>) {
        return {.kind = UnderlyingKind::Forwards, .next = node.attachment};
      } else if constexpr (
        std::is_same_v<T, ir::Blit> || std::is_same_v<T, ir::CopyBuffer> || std::is_same_v<T, ir::CopyImage> ||
        std::is_same_v<T, ir::ResolveImage> || std::is_same_v<T, ir::CopyAccelerationStructure>
      ) {
        return {.kind = UnderlyingKind::Forwards, .next = node.dst};
      } else if constexpr (std::is_same_v<T, ir::WriteAccelerationStructureProperties>) {
        return {.kind = UnderlyingKind::Forwards, .next = node.source};
      } else if constexpr (std::is_same_v<T, ir::CopyBufferToImage>) {
        return {.kind = UnderlyingKind::Forwards, .next = node.image};
      } else if constexpr (
        std::is_same_v<T, ir::CopyImageToBuffer> || std::is_same_v<T, ir::FillBuffer> ||
        std::is_same_v<T, ir::UpdateBuffer>
      ) {
        return {.kind = UnderlyingKind::Forwards, .next = node.buffer};
      } else if constexpr (std::is_same_v<T, ir::Export>) {
        return {.kind = UnderlyingKind::Forwards, .next = node.resource};
      } else if constexpr (std::is_same_v<T, ir::PassResult>) {
        return {.kind = UnderlyingKind::Forwards, .next = node.resource};
      } else if constexpr (std::is_same_v<T, ir::Phi>) {
        if (node.incoming.empty()) {
          return {};
        }

        return {.kind = UnderlyingKind::Forwards, .next = node.incoming.front().first};
      } else {
        return {};
      }
    },
    ir
  );
}

auto side_effects(const IR& ir) -> SideEffect {
  return std::visit(
    [](const auto& node) -> SideEffect {
      using T = std::decay_t<decltype(node)>;
      if constexpr (
        std::is_same_v<T, ir::Type> || std::is_same_v<T, ir::Constant> || std::is_same_v<T, ir::Variable> ||
        std::is_same_v<T, ir::Array> || std::is_same_v<T, ir::Index> || std::is_same_v<T, ir::PassResult> ||
        std::is_same_v<T, ir::View>
      ) {
        return SideEffect::Empty;
      } else if constexpr (
        std::is_same_v<T, ir::ConstructBuffer> || std::is_same_v<T, ir::ConstructImage> ||
        std::is_same_v<T, ir::SwapchainImage> || std::is_same_v<T, ir::ConstructQueryPool>
      ) {
        return SideEffect::Defines;
      } else if constexpr (std::is_same_v<T, ir::AcquireNextImage>) {
        return SideEffect::Defines | SideEffect::External;
      } else if constexpr (std::is_same_v<T, ir::Export>) {
        return SideEffect::Memory | SideEffect::Sync | SideEffect::External;
      } else if constexpr (
        std::is_same_v<T, ir::Clear> || std::is_same_v<T, ir::FillBuffer> || std::is_same_v<T, ir::UpdateBuffer>
      ) {
        return SideEffect::Write | SideEffect::Command;
      } else if constexpr (
        std::is_same_v<T, ir::Blit> || std::is_same_v<T, ir::CopyBufferToImage> ||
        std::is_same_v<T, ir::CopyImageToBuffer> || std::is_same_v<T, ir::CopyBuffer> ||
        std::is_same_v<T, ir::CopyImage> || std::is_same_v<T, ir::ResolveImage> ||
        std::is_same_v<T, ir::BuildAccelerationStructures> || std::is_same_v<T, ir::CopyAccelerationStructure>
      ) {
        return SideEffect::Memory | SideEffect::Command;
      } else if constexpr (std::is_same_v<T, ir::WriteAccelerationStructureProperties>) {
        return SideEffect::Read | SideEffect::Command | SideEffect::External;
      } else if constexpr (std::is_same_v<T, ir::BeginRendering>) {
        return SideEffect::Memory | SideEffect::Command | SideEffect::State;
      } else if constexpr (std::is_same_v<T, ir::BeginCompute>) {
        return SideEffect::Memory | SideEffect::Sync | SideEffect::Command | SideEffect::State;
      } else if constexpr (std::is_same_v<T, ir::EndRendering> || std::is_same_v<T, ir::EndCompute>) {
        return SideEffect::Command;
      } else if constexpr (std::is_same_v<T, ir::BindPipeline> || std::is_same_v<T, ir::SetState>) {
        return SideEffect::State;
      } else if constexpr (std::is_same_v<T, ir::BindVertexBuffers> || std::is_same_v<T, ir::BindIndexBuffer>) {
        return SideEffect::Read | SideEffect::State | SideEffect::Command;
      } else if constexpr (std::is_same_v<T, ir::WriteDescriptor>) {
        return SideEffect::Read | SideEffect::Sync | SideEffect::State;
      } else if constexpr (
        std::is_same_v<T, ir::Draw> || std::is_same_v<T, ir::DrawIndexed> || std::is_same_v<T, ir::DrawMeshTasks> ||
        std::is_same_v<T, ir::TraceRays>
      ) {
        return SideEffect::Command | SideEffect::ReadsState;
      } else if constexpr (std::is_same_v<T, ir::DrawIndirect> || std::is_same_v<T, ir::DrawMeshTasksIndirect>) {
        return SideEffect::Command | SideEffect::ReadsState | SideEffect::Read | SideEffect::Sync;
      } else if constexpr (std::is_same_v<T, ir::CallOpaque>) {
        return SideEffect::Command | SideEffect::ReadsState | SideEffect::Host;
      } else if constexpr (std::is_same_v<T, ir::Dispatch>) {
        const auto base = SideEffect::Command | SideEffect::ReadsState;
        return node.size.kind == ir::DispatchSize::Kind::Indirect ? base | SideEffect::Read | SideEffect::Sync : base;
      } else if constexpr (
        std::is_same_v<T, ir::Label> || std::is_same_v<T, ir::SelectionMerge> || std::is_same_v<T, ir::Phi>
      ) {
        return SideEffect::Control;
      } else if constexpr (
        std::is_same_v<T, ir::Branch> || std::is_same_v<T, ir::BranchConditional> || std::is_same_v<T, ir::Return>
      ) {
        return SideEffect::Control | SideEffect::Terminator;
      } else if constexpr (std::is_same_v<T, ir::MemoryBarrier> || std::is_same_v<T, ir::ImageBarrier>) {
        return SideEffect::Sync | SideEffect::Command;
      } else {
        static_assert(sizeof(T) == 0, "every instruction needs its side effects spelled out");
      }
    },
    ir
  );
}

auto is_terminator(const IR& ir) -> bool { return contains(side_effects(ir), SideEffect::Terminator); }

auto opens_region(const IR& ir) -> bool {
  return std::holds_alternative<ir::BeginRendering>(ir) || std::holds_alternative<ir::BeginCompute>(ir);
}

auto closes_region(const IR& ir) -> bool {
  return std::holds_alternative<ir::EndRendering>(ir) || std::holds_alternative<ir::EndCompute>(ir);
}

auto is_barrier(const IR& ir) -> bool {
  return std::holds_alternative<ir::MemoryBarrier>(ir) || std::holds_alternative<ir::ImageBarrier>(ir);
}

auto mutable_pipeline_of(IR& ir) -> PipelineID* {
  return std::visit(
    [](auto& node) -> PipelineID* {
      using T = std::decay_t<decltype(node)>;
      if constexpr (requires { node.pipeline; } && !std::is_same_v<T, ir::BindPipeline>) {
        return &node.pipeline;
      } else {
        return nullptr;
      }
    },
    ir
  );
}

auto pipeline_of(const IR& ir) -> const PipelineID* { return mutable_pipeline_of(const_cast<IR&>(ir)); }

auto operands_of(const IR& ir) -> std::vector<ValueID> {
  auto operands = std::vector<ValueID>{};
  visit_operands(ir, [&](ValueID id) { operands.push_back(id); });
  return operands;
}

auto resource_effects(const IR& ir) -> std::vector<ResourceSideEffect> {
  auto effects = std::vector<ResourceSideEffect>{};
  visit_resource_side_effects(ir, [&](const ResourceSideEffect& effect) { effects.push_back(effect); });
  return effects;
}

auto resolution_for(ValueID id, const IR& ir, const ResourceResolution* next) -> ResourceResolution {
  const auto object = underlying_object(ir);
  switch (object.kind) {
    case UnderlyingKind::Base    : return {.root = id, .base = id};
    case UnderlyingKind::Forwards: {
      auto resolution = next ? *next : ResourceResolution{.root = object.next};
      if (const auto* view = std::get_if<ir::View>(&ir)) {
        resolution.range = view->range;
      }

      return resolution;
    }
    case UnderlyingKind::Element:
      return {.root = id, .base = next ? next->base : ValueID::Invalid, .is_through_array = true};
    case UnderlyingKind::None: return {.root = id};
  }

  return {.root = id};
}

ResourceIndex::ResourceIndex(std::span<const Instr> instructions) {
  for (const auto& instruction : instructions) {
    values.emplace(instruction.id, &instruction.ir);
  }

  for (const auto& [start, start_ir] : values) {
    auto current = start;
    auto pending = std::vector<std::pair<ValueID, const IR*>>{};
    auto seen = ankerl::unordered_dense::set<ValueID>{};
    while (true) {
      if (resolutions.contains(current)) {
        break;
      }

      if (!seen.insert(current).second) {
        resolutions.emplace(current, ResourceResolution{.root = current});
        break;
      }

      const auto it = values.find(current);
      if (it == values.end()) {
        resolutions.emplace(current, ResourceResolution{.root = current});
        break;
      }

      const auto object = underlying_object(*it->second);
      if (object.kind == UnderlyingKind::Forwards || object.kind == UnderlyingKind::Element) {
        pending.emplace_back(current, it->second);
        current = object.next;
        continue;
      }

      resolutions.emplace(current, resolution_for(current, *it->second, nullptr));
      break;
    }

    for (auto it = pending.rbegin(); it != pending.rend(); ++it) {
      const auto object = underlying_object(*it->second);
      const auto next = resolutions.find(object.next);
      resolutions.insert_or_assign(
        it->first,
        resolution_for(it->first, *it->second, next != resolutions.end() ? &next->second : nullptr)
      );
    }
  }
}

auto ResourceIndex::get(this const ResourceIndex& self, ValueID id) -> const IR* {
  const auto it = self.values.find(id);
  return it == self.values.end() ? nullptr : it->second;
}

auto ResourceIndex::resolve(this const ResourceIndex& self, ValueID id)
  -> std::pair<option<ResourceResolution>, const IR*> {
  const auto it = self.resolutions.find(id);
  if (it == self.resolutions.end() || !is_valid(it->second.base)) {
    return {nullopt, nullptr};
  }

  const auto* base = self.get(it->second.base);
  if (!base) {
    return {nullopt, nullptr};
  }

  return {it->second, base};
}
} // namespace ox::vir
