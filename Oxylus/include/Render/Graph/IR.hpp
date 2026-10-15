#pragma once

#include <ankerl/unordered_dense.h>
#include <array>
#include <bit>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "Render/Graph/Access.hpp"
#include "Render/Graph/PipelineState.hpp"
#include "Render/Graph/Resource.hpp"
#include "Render/Graph/Types.hpp"

namespace ox::vir {
constexpr u32 REMAINING = ~0_u32;

// a mip and layer range relative to the range of the image it is taken from, REMAINING counts run
// to the end of it
struct Subresource {
  u32 base_level = 0;
  u32 level_count = REMAINING;
  u32 base_layer = 0;
  u32 layer_count = REMAINING;

  static constexpr auto whole() -> Subresource { return {}; }
  static constexpr auto mip(u32 level) -> Subresource { return {.base_level = level, .level_count = 1}; }
  static constexpr auto layer(u32 layer) -> Subresource { return {.base_layer = layer, .layer_count = 1}; }

  auto level_end(this const Subresource& self) -> u64 {
    return self.level_count == REMAINING ? REMAINING : u64(self.base_level) + self.level_count;
  }
  auto layer_end(this const Subresource& self) -> u64 {
    return self.layer_count == REMAINING ? REMAINING : u64(self.base_layer) + self.layer_count;
  }
  auto is_whole(this const Subresource& self) -> bool { return self == Subresource{}; }
  auto overlaps(this const Subresource& self, const Subresource& other) -> bool;
  auto intersect(this const Subresource& self, const Subresource& other) -> Subresource;
  auto contains(this const Subresource& self, const Subresource& other) -> bool;
  // the absolute range inside a view that covers `range`
  auto apply(this const Subresource& self, const VkImageSubresourceRange& range) -> VkImageSubresourceRange;

  auto operator==(const Subresource&) const -> bool = default;
};

struct AccelerationStructureBuild {
  VkAccelerationStructureBuildGeometryInfoKHR info = {};
  std::vector<VkAccelerationStructureGeometryKHR> geometries = {};
  std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges = {};
};

enum class AccelerationStructureAddressKind : u8 { Scratch, Vertices, Indices, Transform, Aabbs, Instances };

struct AccelerationStructureAddress {
  u32 build_index = 0;
  u32 geometry_index = 0;
  AccelerationStructureAddressKind kind = AccelerationStructureAddressKind::Vertices;
  ValueID buffer = ValueID::Invalid;
  u64 offset = 0;
};

namespace ir {
enum class TypeKind : u8 { U64, Image, Buffer };

struct Type {
  TypeKind kind = TypeKind::Buffer;
  Format format = Format::Undefined;
  Samples samples = Samples::e1;

  auto operator==(const Type&) const -> bool = default;
};

struct Constant {
  enum class Kind : u8 { I32, U32, U64, Extent2D, Extent3D, String, Access, ClearValue };

  Kind kind = Kind::U32;
  std::array<u32, 4> words = {};
  std::string string = {};

  static auto from_i32(i32 value) -> Constant;
  static auto from_u32(u32 value) -> Constant;
  static auto from_u64(u64 value) -> Constant;
  static auto from_extent(Extent2D value) -> Constant;
  static auto from_extent(Extent3D value) -> Constant;
  static auto from_string(std::string value) -> Constant;
  static auto from_access(Access value) -> Constant;
  static auto from_clear(const ClearValue& value) -> Constant;

  auto as_i32(this const Constant& self) -> i32 { return std::bit_cast<i32>(self.words[0]); }
  auto as_u32(this const Constant& self) -> u32 { return self.words[0]; }
  auto as_u64(this const Constant& self) -> u64 { return u64(self.words[0]) | (u64(self.words[1]) << 32); }
  auto as_extent_2d(this const Constant& self) -> Extent2D { return {self.words[0], self.words[1]}; }
  auto as_extent_3d(this const Constant& self) -> Extent3D { return {self.words[0], self.words[1], self.words[2]}; }
  auto as_access(this const Constant& self) -> Access { return static_cast<Access>(self.as_u64()); }
  auto as_clear(this const Constant& self) -> ClearValue;
  auto hash(this const Constant& self) -> u64;

  auto operator==(const Constant&) const -> bool = default;
};

struct ConstantHash {
  using is_avalanching = void;
  auto operator()(const Constant& constant) const noexcept -> u64 { return constant.hash(); }
};

struct TypeHash {
  using is_avalanching = void;
  auto operator()(const Type& type) const noexcept -> u64;
};

enum class VariableKindTag : u8 {
  Bool,
  I32,
  U32,
  Extent2D,
  Extent3D,
  ClearValue,
  Bytes,
  Swapchain,
  Buffer,
  ImageAttachment,
  Callback,
  AccelerationStructureBuilds,
  QueryPool,
};

struct VariableKind {
  VariableKindTag tag = VariableKindTag::U32;
  u32 size = 0;

  auto operator==(const VariableKind&) const -> bool = default;
};

auto to_string(VariableKind kind) -> std::string;

struct Descriptor {
  enum class Kind : u8 { Sampler, Image, CombinedImageSampler, TexelBuffer, Buffer, AccelerationStructure };

  Kind kind = Kind::Image;
  ValueID image = ValueID::Invalid;
  ValueID buffer = ValueID::Invalid;
  VkSampler sampler = VK_NULL_HANDLE;
  VkBufferView view = VK_NULL_HANDLE;
  u64 offset = 0;
  u64 range = VK_WHOLE_SIZE;
  VkAccelerationStructureKHR acceleration_structure = VK_NULL_HANDLE;

  auto image_value(this const Descriptor& self) -> option<ValueID>;
  auto buffer_value(this const Descriptor& self) -> option<ValueID>;
  auto resource(this const Descriptor& self) -> option<ValueID>;
  auto supports_type(this const Descriptor& self, VkDescriptorType type) -> bool;
  auto image_usage(this const Descriptor& self, VkDescriptorType type) -> ImageUsageFlags;
  auto buffer_usage(this const Descriptor& self, VkDescriptorType type) -> VkBufferUsageFlags;

  auto operator==(const Descriptor&) const -> bool = default;
};

struct DispatchSize {
  enum class Kind : u8 { Groups, Invocations, InvocationsPerPixel, InvocationsPerElement, Indirect };

  Kind kind = Kind::Groups;
  ValueID x = ValueID::Invalid;
  ValueID y = ValueID::Invalid;
  ValueID z = ValueID::Invalid;
  ValueID resource = ValueID::Invalid;
  std::array<f32, 3> scale = {1.0f, 1.0f, 1.0f};
  u64 element_size = 0;
  u64 offset = 0;

  auto operator==(const DispatchSize&) const -> bool = default;
};

auto scaled(u64 count, f32 scale) -> u32;

struct DrawCount {
  ValueID fixed = ValueID::Invalid;
  ValueID buffer = ValueID::Invalid;
  u64 offset = 0;
  ValueID max = ValueID::Invalid;

  auto count_buffer(this const DrawCount& self) -> option<ValueID> {
    if (!is_valid(self.buffer)) {
      return nullopt;
    }

    return self.buffer;
  }
  auto operator==(const DrawCount&) const -> bool = default;
};

struct Variable {
  u32 slot = 0;
  VariableKind kind = {};
  ValueID name = ValueID::Invalid;
};

struct Array {
  ValueID ty = ValueID::Invalid;
  std::vector<ValueID> elements = {};
};

struct Index {
  ValueID array = ValueID::Invalid;
  ValueID index = ValueID::Invalid;
};

struct ConstructBuffer {
  Buffer buffer = {};
  ValueID size = ValueID::Invalid;
  VkBufferUsageFlags usage = 0;
  MemoryLocation location = MemoryLocation::GpuOnly;
  ValueID initial_access = ValueID::Invalid;
  ValueID name = ValueID::Invalid;
};

struct ConstructImage {
  Image image = {};
  VkImageView image_view = VK_NULL_HANDLE;
  ImageViewType view_type = ImageViewType::e2D;
  ImageType image_type = ImageType::e2D;
  ImageCreateFlags flags = ImageCreateFlags::None;
  ValueID extent = ValueID::Invalid;
  Format format = Format::Undefined;
  Samples samples = Samples::e1;
  ValueID base_level = ValueID::Invalid;
  ValueID level_count = ValueID::Invalid;
  ValueID base_layer = ValueID::Invalid;
  ValueID layer_count = ValueID::Invalid;
  ImageUsageFlags usage = ImageUsageFlags::None;
  std::vector<Format> view_formats = {};
  VkImageLayout initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
  ValueID initial_access = ValueID::Invalid;
  ValueID name = ValueID::Invalid;
};

// a narrower view of an image, its range relative to the root image's own range
struct View {
  ValueID image = ValueID::Invalid;
  // the image the range is relative to
  ValueID root = ValueID::Invalid;
  Subresource range = {};
  Format format = Format::Undefined;
  ImageViewType view_type = ImageViewType::Auto;
};

struct AcquireNextImage {
  ValueID swapchain = ValueID::Invalid;
};

struct SwapchainImage {
  ValueID swapchain = ValueID::Invalid;
  ValueID acquire = ValueID::Invalid;
  Format format = Format::Undefined;
  Samples samples = Samples::e1;
};

struct Export {
  ValueID resource = ValueID::Invalid;
  ValueID access = ValueID::Invalid;
  DomainFlag dst_domain = DomainFlag::None;
};

struct Clear {
  ValueID attachment = ValueID::Invalid;
  ValueID color = ValueID::Invalid;
};

struct Blit {
  ValueID src = ValueID::Invalid;
  ValueID dst = ValueID::Invalid;
  VkFilter filter = VK_FILTER_LINEAR;
};

struct CopyBufferToImage {
  ValueID buffer = ValueID::Invalid;
  ValueID image = ValueID::Invalid;
  option<BufferImageCopy> region = nullopt;
};

struct CopyImageToBuffer {
  ValueID image = ValueID::Invalid;
  ValueID buffer = ValueID::Invalid;
  option<BufferImageCopy> region = nullopt;
};

struct CopyBuffer {
  ValueID src = ValueID::Invalid;
  ValueID dst = ValueID::Invalid;
  BufferCopy region = {};
};

struct FillBuffer {
  ValueID buffer = ValueID::Invalid;
  u64 offset = 0;
  u64 size = VK_WHOLE_SIZE;
  u32 data = 0;
};

struct UpdateBuffer {
  ValueID buffer = ValueID::Invalid;
  ValueID source = ValueID::Invalid;
  std::vector<u8> data = {};
  u64 offset = 0;
};

struct CopyImage {
  ValueID src = ValueID::Invalid;
  ValueID dst = ValueID::Invalid;
  option<ImageCopy> region = nullopt;
};

struct ConstructQueryPool {
  VkQueryType type = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
  u32 count = 1;
};

struct BuildAccelerationStructures {
  ValueID builds = ValueID::Invalid;
  std::vector<AccelerationStructureAddress> addresses = {};
  std::vector<std::pair<ValueID, ValueID>> resources = {};
};

struct WriteAccelerationStructureProperties {
  ValueID source = ValueID::Invalid;
  VkAccelerationStructureKHR structure = VK_NULL_HANDLE;
  ValueID queries = ValueID::Invalid;
  u32 first_query = 0;
};

struct CopyAccelerationStructure {
  ValueID src = ValueID::Invalid;
  ValueID dst = ValueID::Invalid;
  VkAccelerationStructureKHR source = VK_NULL_HANDLE;
  VkAccelerationStructureKHR destination = VK_NULL_HANDLE;
  VkCopyAccelerationStructureModeKHR mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
};

struct ResolveImage {
  ValueID src = ValueID::Invalid;
  ValueID dst = ValueID::Invalid;
  option<ImageCopy> region = nullopt;
};

struct BeginRendering {
  std::vector<std::pair<ValueID, ValueID>> attachments = {};
  std::vector<std::pair<ValueID, ValueID>> clears = {};
  ValueID render_area = ValueID::Invalid;
  ValueID name = ValueID::Invalid;
  u32 layer_count = 1;
  u32 view_mask = 0;
};

struct BindPipeline {
  ValueID pass = ValueID::Invalid;
  PipelineID pipeline = PipelineID::Invalid;
  VkPipelineBindPoint bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
};

struct SetState {
  ValueID pass = ValueID::Invalid;
  StateChange change = {};
};

struct BindVertexBuffers {
  ValueID pass = ValueID::Invalid;
  u32 first_binding = 0;
  ValueID buffers = ValueID::Invalid;
  ValueID offsets = ValueID::Invalid;
};

struct BindIndexBuffer {
  ValueID pass = ValueID::Invalid;
  ValueID buffer = ValueID::Invalid;
  u64 offset = 0;
  VkIndexType index_type = VK_INDEX_TYPE_UINT32;
};

struct WriteDescriptor {
  ValueID pass = ValueID::Invalid;
  u32 set = 0;
  u32 binding = 0;
  Descriptor descriptor = {};
  option<VkDescriptorType> descriptor_type = nullopt;
  ValueID access = ValueID::Invalid;
  // what the image sits in while the pass runs, filled in by sync
  VkImageLayout image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct Draw {
  ValueID pass = ValueID::Invalid;
  ValueID vertex_count = ValueID::Invalid;
  ValueID instance_count = ValueID::Invalid;
  ValueID first_vertex = ValueID::Invalid;
  ValueID first_instance = ValueID::Invalid;
  PipelineID pipeline = PipelineID::Invalid;
  PipelineState state = {};
  DynamicValues dynamic = {};
};

struct DrawIndexed {
  ValueID pass = ValueID::Invalid;
  ValueID index_count = ValueID::Invalid;
  ValueID instance_count = ValueID::Invalid;
  ValueID first_index = ValueID::Invalid;
  ValueID vertex_offset = ValueID::Invalid;
  ValueID first_instance = ValueID::Invalid;
  PipelineID pipeline = PipelineID::Invalid;
  PipelineState state = {};
  DynamicValues dynamic = {};
};

struct DrawIndirect {
  ValueID pass = ValueID::Invalid;
  bool is_indexed = false;
  ValueID buffer = ValueID::Invalid;
  u64 offset = 0;
  u32 stride = 0;
  DrawCount count = {};
  PipelineID pipeline = PipelineID::Invalid;
  PipelineState state = {};
  DynamicValues dynamic = {};
};

struct DrawMeshTasks {
  ValueID pass = ValueID::Invalid;
  ValueID x = ValueID::Invalid;
  ValueID y = ValueID::Invalid;
  ValueID z = ValueID::Invalid;
  PipelineID pipeline = PipelineID::Invalid;
  PipelineState state = {};
  DynamicValues dynamic = {};
};

struct DrawMeshTasksIndirect {
  ValueID pass = ValueID::Invalid;
  ValueID buffer = ValueID::Invalid;
  u64 offset = 0;
  u32 stride = 0;
  DrawCount count = {};
  PipelineID pipeline = PipelineID::Invalid;
  PipelineState state = {};
  DynamicValues dynamic = {};
};

struct CallOpaque {
  ValueID pass = ValueID::Invalid;
  ValueID body = ValueID::Invalid;
  PipelineID pipeline = PipelineID::Invalid;
  PipelineState state = {};
  DynamicValues dynamic = {};
};

struct EndRendering {
  ValueID pass = ValueID::Invalid;
};

struct BeginCompute {
  std::vector<std::pair<ValueID, ValueID>> attachments = {};
  ValueID name = ValueID::Invalid;
};

struct Dispatch {
  ValueID pass = ValueID::Invalid;
  DispatchSize size = {};
  PipelineID pipeline = PipelineID::Invalid;
  PushConstants push_constants = {};
  Specialization specialization = {};
};

struct TraceRays {
  ValueID pass = ValueID::Invalid;
  ValueID width = ValueID::Invalid;
  ValueID height = ValueID::Invalid;
  ValueID depth = ValueID::Invalid;
  PipelineID pipeline = PipelineID::Invalid;
  PushConstants push_constants = {};
  Specialization specialization = {};
};

struct EndCompute {
  ValueID pass = ValueID::Invalid;
};

struct PassResult {
  ValueID completion = ValueID::Invalid;
  ValueID resource = ValueID::Invalid;
};

struct Label {
  LabelID label = LabelID::Invalid;
};

struct SelectionMerge {
  LabelID merge = LabelID::Invalid;
};

struct Branch {
  LabelID target = LabelID::Invalid;
};

struct BranchConditional {
  ValueID condition = ValueID::Invalid;
  LabelID true_label = LabelID::Invalid;
  LabelID false_label = LabelID::Invalid;
};

struct Return {};

struct Phi {
  std::vector<std::pair<ValueID, LabelID>> incoming = {};
};

struct MemoryBarrier {
  ValueID src_access = ValueID::Invalid;
  ValueID dst_access = ValueID::Invalid;
};

struct ImageBarrier {
  ValueID src_access = ValueID::Invalid;
  ValueID dst_access = ValueID::Invalid;
  VkImageLayout old_layout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageLayout new_layout = VK_IMAGE_LAYOUT_UNDEFINED;
  // the root image, its range relative to the root's own
  ValueID value = ValueID::Invalid;
  Subresource range = {};
};

} // namespace ir

using IR = std::variant<
  ir::Type,
  ir::Constant,
  ir::Variable,
  ir::Array,
  ir::Index,
  ir::ConstructBuffer,
  ir::ConstructImage,
  ir::View,
  ir::AcquireNextImage,
  ir::SwapchainImage,
  ir::Export,
  ir::Clear,
  ir::Blit,
  ir::CopyBufferToImage,
  ir::CopyImageToBuffer,
  ir::CopyBuffer,
  ir::FillBuffer,
  ir::UpdateBuffer,
  ir::CopyImage,
  ir::ResolveImage,
  ir::ConstructQueryPool,
  ir::BuildAccelerationStructures,
  ir::WriteAccelerationStructureProperties,
  ir::CopyAccelerationStructure,
  ir::BeginRendering,
  ir::BindPipeline,
  ir::SetState,
  ir::BindVertexBuffers,
  ir::BindIndexBuffer,
  ir::WriteDescriptor,
  ir::Draw,
  ir::DrawIndexed,
  ir::DrawIndirect,
  ir::DrawMeshTasks,
  ir::DrawMeshTasksIndirect,
  ir::CallOpaque,
  ir::EndRendering,
  ir::BeginCompute,
  ir::Dispatch,
  ir::TraceRays,
  ir::EndCompute,
  ir::PassResult,
  ir::Label,
  ir::SelectionMerge,
  ir::Branch,
  ir::BranchConditional,
  ir::Return,
  ir::Phi,
  ir::MemoryBarrier,
  ir::ImageBarrier>;

struct Instr {
  ValueID id = ValueID::Invalid;
  IR ir = {};
};

enum class SideEffect : u32 {
  Empty = 0,
  // reads the memory of a resource it names
  Read = 1 << 0,
  Write = 1 << 1,
  // exists to move a resource into another synchronization state
  Sync = 1 << 2,
  // records a command, so where it sits among other commands is observable
  Command = 1 << 3,
  // writes pass state the draws and dispatches after it read
  State = 1 << 4,
  ReadsState = 1 << 5,
  // produces an identity of its own, two of them are never the same value
  Defines = 1 << 6,
  // runs host code the graph cannot see into
  Host = 1 << 7,
  External = 1 << 8,
  // belongs to the block it sits in
  Control = 1 << 9,
  Terminator = 1 << 10,
  Memory = Read | Write,
  // what keeps an instruction alive when nothing uses its value
  Observable = Write | Sync | Command | State | Host | External | Control | Terminator,
};
OX_VIR_FLAGS(SideEffect)

enum class UnderlyingKind : u8 { Base, Forwards, Element, None };

struct UnderlyingObject {
  UnderlyingKind kind = UnderlyingKind::None;
  ValueID next = ValueID::Invalid;
};

struct ResourceSideEffect {
  ValueID resource = ValueID::Invalid;
  // either fixed by the instruction or the access constant at `access_operand`
  Access access = Access::Empty;
  ValueID access_operand = ValueID::Invalid;
};

auto underlying_object(const IR& ir) -> UnderlyingObject;
auto side_effects(const IR& ir) -> SideEffect;
auto is_terminator(const IR& ir) -> bool;
auto opens_region(const IR& ir) -> bool;
auto closes_region(const IR& ir) -> bool;
auto is_barrier(const IR& ir) -> bool;
// the pipeline a draw, dispatch or trace runs, null for everything else
auto pipeline_of(const IR& ir) -> const PipelineID*;
auto mutable_pipeline_of(IR& ir) -> PipelineID*;

// resource operands precede their pass chain so producers finish before a region opens
template <typename F>
auto visit_operands(const IR& ir, F&& visit) -> void {
  const auto visit_valid = [&](ValueID id) {
    if (is_valid(id)) {
      visit(id);
    }
  };
  const auto visit_count = [&](const ir::DrawCount& count) {
    visit_valid(count.fixed);
    visit_valid(count.buffer);
    visit_valid(count.max);
  };

  std::visit(
    [&](const auto& node) {
      using T = std::decay_t<decltype(node)>;
      if constexpr (std::is_same_v<T, ir::Variable>) {
        visit_valid(node.name);
      } else if constexpr (std::is_same_v<T, ir::Array>) {
        visit_valid(node.ty);
        for (const auto element : node.elements) {
          visit(element);
        }
      } else if constexpr (std::is_same_v<T, ir::Index>) {
        visit(node.array);
        visit(node.index);
      } else if constexpr (std::is_same_v<T, ir::ConstructBuffer>) {
        visit_valid(node.size);
        visit_valid(node.initial_access);
        visit_valid(node.name);
      } else if constexpr (std::is_same_v<T, ir::ConstructImage>) {
        visit_valid(node.extent);
        visit_valid(node.base_level);
        visit_valid(node.level_count);
        visit_valid(node.base_layer);
        visit_valid(node.layer_count);
        visit_valid(node.initial_access);
        visit_valid(node.name);
      } else if constexpr (std::is_same_v<T, ir::View>) {
        visit(node.image);
      } else if constexpr (std::is_same_v<T, ir::AcquireNextImage>) {
        visit(node.swapchain);
      } else if constexpr (std::is_same_v<T, ir::SwapchainImage>) {
        visit(node.swapchain);
        visit(node.acquire);
      } else if constexpr (std::is_same_v<T, ir::Export>) {
        visit(node.resource);
        visit(node.access);
      } else if constexpr (std::is_same_v<T, ir::Clear>) {
        visit(node.attachment);
        visit(node.color);
      } else if constexpr (
        std::is_same_v<T, ir::Blit> || std::is_same_v<T, ir::CopyBuffer> || std::is_same_v<T, ir::CopyImage> ||
        std::is_same_v<T, ir::ResolveImage>
      ) {
        visit(node.src);
        visit(node.dst);
      } else if constexpr (std::is_same_v<T, ir::CopyBufferToImage>) {
        visit(node.buffer);
        visit(node.image);
      } else if constexpr (std::is_same_v<T, ir::CopyImageToBuffer>) {
        visit(node.image);
        visit(node.buffer);
      } else if constexpr (std::is_same_v<T, ir::FillBuffer>) {
        visit(node.buffer);
      } else if constexpr (std::is_same_v<T, ir::UpdateBuffer>) {
        visit(node.buffer);
        visit_valid(node.source);
      } else if constexpr (std::is_same_v<T, ir::BuildAccelerationStructures>) {
        visit(node.builds);
        for (const auto& [resource, access] : node.resources) {
          visit(resource);
          visit(access);
        }
      } else if constexpr (std::is_same_v<T, ir::WriteAccelerationStructureProperties>) {
        visit(node.source);
        visit(node.queries);
      } else if constexpr (std::is_same_v<T, ir::CopyAccelerationStructure>) {
        visit(node.src);
        visit(node.dst);
      } else if constexpr (std::is_same_v<T, ir::BeginRendering>) {
        for (const auto& [resource, access] : node.attachments) {
          visit(resource);
          visit(access);
        }

        for (const auto& [cleared, color] : node.clears) {
          visit(color);
        }

        visit_valid(node.render_area);
        visit_valid(node.name);
      } else if constexpr (std::is_same_v<T, ir::BeginCompute>) {
        for (const auto& [resource, access] : node.attachments) {
          visit(resource);
          visit(access);
        }

        visit_valid(node.name);
      } else if constexpr (
        std::is_same_v<T, ir::BindPipeline> || std::is_same_v<T, ir::EndRendering> || std::is_same_v<T, ir::EndCompute>
      ) {
        visit(node.pass);
      } else if constexpr (std::is_same_v<T, ir::PassResult>) {
        visit(node.completion);
        visit(node.resource);
      } else if constexpr (std::is_same_v<T, ir::SetState>) {
        if (const auto* from = std::get_if<state::PushFrom>(&node.change)) {
          visit(from->source);
        } else if (const auto* address = std::get_if<state::PushAddress>(&node.change)) {
          visit(address->buffer);
        }
        visit(node.pass);
      } else if constexpr (std::is_same_v<T, ir::BindVertexBuffers>) {
        visit(node.buffers);
        visit(node.offsets);
        visit(node.pass);
      } else if constexpr (std::is_same_v<T, ir::BindIndexBuffer>) {
        visit(node.buffer);
        visit(node.pass);
      } else if constexpr (std::is_same_v<T, ir::WriteDescriptor>) {
        if (const auto resource = node.descriptor.resource(); resource.has_value()) {
          visit(*resource);
        }

        visit(node.access);
        visit(node.pass);
      } else if constexpr (std::is_same_v<T, ir::Draw>) {
        visit(node.vertex_count);
        visit(node.instance_count);
        visit(node.first_vertex);
        visit(node.first_instance);
        visit(node.pass);
      } else if constexpr (std::is_same_v<T, ir::DrawIndexed>) {
        visit(node.index_count);
        visit(node.instance_count);
        visit(node.first_index);
        visit(node.vertex_offset);
        visit(node.first_instance);
        visit(node.pass);
      } else if constexpr (std::is_same_v<T, ir::DrawIndirect> || std::is_same_v<T, ir::DrawMeshTasksIndirect>) {
        visit(node.buffer);
        visit_count(node.count);
        visit(node.pass);
      } else if constexpr (std::is_same_v<T, ir::DrawMeshTasks>) {
        visit(node.x);
        visit(node.y);
        visit(node.z);
        visit(node.pass);
      } else if constexpr (std::is_same_v<T, ir::CallOpaque>) {
        visit(node.body);
        visit(node.pass);
      } else if constexpr (std::is_same_v<T, ir::Dispatch>) {
        visit_valid(node.size.x);
        visit_valid(node.size.y);
        visit_valid(node.size.z);
        visit_valid(node.size.resource);
        visit(node.pass);
      } else if constexpr (std::is_same_v<T, ir::TraceRays>) {
        visit(node.width);
        visit(node.height);
        visit(node.depth);
        visit(node.pass);
      } else if constexpr (std::is_same_v<T, ir::BranchConditional>) {
        visit(node.condition);
      } else if constexpr (std::is_same_v<T, ir::Phi>) {
        for (const auto& [value, label] : node.incoming) {
          visit(value);
        }
      } else if constexpr (std::is_same_v<T, ir::MemoryBarrier>) {
        visit(node.src_access);
        visit(node.dst_access);
      } else if constexpr (std::is_same_v<T, ir::ImageBarrier>) {
        visit(node.src_access);
        visit(node.dst_access);
        visit(node.value);
      }
    },
    ir
  );
}

// what an instruction does to each resource it names, in operand order
template <typename F>
auto visit_resource_side_effects(const IR& ir, F&& visit) -> void {
  const auto fixed = [&](ValueID resource, Access access) {
    visit(ResourceSideEffect{.resource = resource, .access = access});
  };
  const auto operand = [&](ValueID resource, ValueID access) {
    visit(ResourceSideEffect{.resource = resource, .access_operand = access});
  };

  std::visit(
    [&](const auto& node) {
      using T = std::decay_t<decltype(node)>;
      if constexpr (std::is_same_v<T, ir::Export>) {
        operand(node.resource, node.access);
      } else if constexpr (std::is_same_v<T, ir::Clear>) {
        fixed(node.attachment, Access::Clear);
      } else if constexpr (std::is_same_v<T, ir::Blit>) {
        fixed(node.src, Access::BlitRead);
        fixed(node.dst, Access::BlitWrite);
      } else if constexpr (std::is_same_v<T, ir::CopyBufferToImage>) {
        fixed(node.buffer, Access::CopyRead);
        fixed(node.image, Access::CopyWrite);
      } else if constexpr (std::is_same_v<T, ir::CopyImageToBuffer>) {
        fixed(node.image, Access::CopyRead);
        fixed(node.buffer, Access::CopyWrite);
      } else if constexpr (std::is_same_v<T, ir::CopyBuffer> || std::is_same_v<T, ir::CopyImage>) {
        fixed(node.src, Access::CopyRead);
        fixed(node.dst, Access::CopyWrite);
      } else if constexpr (std::is_same_v<T, ir::FillBuffer> || std::is_same_v<T, ir::UpdateBuffer>) {
        fixed(node.buffer, Access::Clear);
      } else if constexpr (std::is_same_v<T, ir::ResolveImage>) {
        fixed(node.src, Access::ResolveRead);
        fixed(node.dst, Access::ResolveWrite);
      } else if constexpr (std::is_same_v<T, ir::BuildAccelerationStructures>) {
        for (const auto& [resource, access] : node.resources) {
          operand(resource, access);
        }
      } else if constexpr (std::is_same_v<T, ir::WriteAccelerationStructureProperties>) {
        fixed(node.source, Access::AccelerationStructureBuildRead);
      } else if constexpr (std::is_same_v<T, ir::CopyAccelerationStructure>) {
        fixed(node.src, Access::AccelerationStructureBuildRead);
        fixed(node.dst, Access::AccelerationStructureBuildWrite);
      } else if constexpr (std::is_same_v<T, ir::BeginRendering> || std::is_same_v<T, ir::BeginCompute>) {
        for (const auto& [resource, access] : node.attachments) {
          operand(resource, access);
        }
      } else if constexpr (std::is_same_v<T, ir::WriteDescriptor>) {
        if (const auto resource = node.descriptor.resource(); resource.has_value()) {
          operand(*resource, node.access);
        }
      } else if constexpr (std::is_same_v<T, ir::BindVertexBuffers>) {
        fixed(node.buffers, Access::AttributeRead);
      } else if constexpr (std::is_same_v<T, ir::BindIndexBuffer>) {
        fixed(node.buffer, Access::IndexRead);
      } else if constexpr (std::is_same_v<T, ir::Dispatch>) {
        if (node.size.kind == ir::DispatchSize::Kind::Indirect) {
          fixed(node.size.resource, Access::IndirectRead);
        }
      } else if constexpr (std::is_same_v<T, ir::DrawIndirect> || std::is_same_v<T, ir::DrawMeshTasksIndirect>) {
        fixed(node.buffer, Access::IndirectRead);
        if (const auto counts = node.count.count_buffer(); counts.has_value()) {
          fixed(*counts, Access::IndirectRead);
        }
      }
    },
    ir
  );
}

auto operands_of(const IR& ir) -> std::vector<ValueID>;
auto resource_effects(const IR& ir) -> std::vector<ResourceSideEffect>;

struct ResourceResolution {
  ValueID root = ValueID::Invalid;
  ValueID base = ValueID::Invalid;
  bool is_through_array = false;
  // the range of the root the value covers
  Subresource range = {};
};

// resolves any value to the resource it versions, over a compiled instruction list
struct ResourceIndex {
  ankerl::unordered_dense::map<ValueID, const IR*> values = {};
  ankerl::unordered_dense::map<ValueID, ResourceResolution> resolutions = {};

  explicit ResourceIndex(std::span<const Instr> instructions);

  auto get(this const ResourceIndex& self, ValueID id) -> const IR*;
  auto resolve(this const ResourceIndex& self, ValueID id) -> std::pair<option<ResourceResolution>, const IR*>;
};

auto resolution_for(ValueID id, const IR& ir, const ResourceResolution* next) -> ResourceResolution;
} // namespace ox::vir
