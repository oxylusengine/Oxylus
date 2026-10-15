#pragma once

#include <algorithm>
#include <array>
#include <expected>
#include <initializer_list>
#include <span>
#include <string_view>

#include "Render/Graph/Program.hpp"
#include "Render/Graph/ShaderLayout.hpp"
#include "Render/Graph/Types.hpp"

namespace ox::vir {
// what a module is compiled against, the reflected bindings of the pipelines it binds
struct PipelineBindings {
  const void* user = nullptr;
  auto (*bindings_fn)(const void* user, PipelineID pipeline) -> option<std::span<const DescriptorBinding>> = nullptr;
  auto (*bindless_set_fn)(const void* user, PipelineID pipeline) -> option<u32> = nullptr;

  // only for modules that write no descriptors
  static auto unchecked() -> PipelineBindings;

  auto bindings(this const PipelineBindings& self, PipelineID pipeline) -> option<std::span<const DescriptorBinding>>;
  auto bindless_set(this const PipelineBindings& self, PipelineID pipeline) -> option<u32>;
};

// a draw or dispatch count, known now or held by a value when the module runs
struct Count {
  ValueID value = ValueID::Invalid;
  u32 literal = 0;

  Count(u32 count) : literal(count) {}
  Count(ValueID id) : value(id) {}
};

struct ResourceAccess {
  ValueID resource = ValueID::Invalid;
  Access access = Access::Empty;
};

constexpr u32 DRAW_INDIRECT_STRIDE = sizeof(VkDrawIndirectCommand);
constexpr u32 DRAW_INDEXED_INDIRECT_STRIDE = sizeof(VkDrawIndexedIndirectCommand);
constexpr u32 DRAW_MESH_TASKS_INDIRECT_STRIDE = sizeof(VkDrawMeshTasksIndirectCommandEXT);

enum class PassKind : u8 { Rendering, Compute };

struct Module {
  enum class TerminatorKind : u8 { Return, Branch, BranchConditional };

  struct Terminator {
    TerminatorKind kind = TerminatorKind::Return;
    LabelID target = LabelID::Invalid;
    ValueID condition = ValueID::Invalid;
    LabelID merge = LabelID::Invalid;
    LabelID true_label = LabelID::Invalid;
    LabelID false_label = LabelID::Invalid;
  };

  struct Block {
    LabelID label = LabelID::Invalid;
    Terminator terminator = {};
  };

  struct OngoingPass {
    PassKind kind = PassKind::Compute;
    ValueID begin = ValueID::Invalid;
    ValueID tip = ValueID::Invalid;
    std::vector<ValueID> resources = {};
  };

  ankerl::unordered_dense::map<ir::Type, ValueID, ir::TypeHash> types = {};
  ankerl::unordered_dense::map<ir::Constant, ValueID, ir::ConstantHash> constants = {};
  std::vector<IR> instructions = {};
  std::vector<ResourceResolution> resolutions = {};
  std::vector<Variable> variables = {};
  std::vector<ValueID> variable_ids = {};
  std::vector<Block> blocks = {Block{.label = LabelID{0}}};
  std::vector<LabelID> instruction_block = {};
  u32 label_count = 1;
  LabelID current_block = LabelID{0};
  option<OngoingPass> ongoing_pass = nullopt;
  // values compiled no matter whether a root reaches them, for passes whose only output is a side
  // effect the module can't see, like a write through a device address
  std::vector<ValueID> kept = {};

  // values
  auto constant_u32(this Module& self, u32 value) -> ValueID;
  auto declare_bool_var(this Module& self, std::string_view name, bool initial) -> ValueID;
  auto declare_i32_var(this Module& self, std::string_view name, i32 initial) -> ValueID;
  auto declare_u32_var(this Module& self, std::string_view name, u32 initial) -> ValueID;
  auto declare_extent_2d_var(this Module& self, std::string_view name, Extent2D initial) -> ValueID;
  auto declare_extent_3d_var(this Module& self, std::string_view name, Extent3D initial) -> ValueID;
  auto declare_clear_var(this Module& self, std::string_view name, const ClearValue& initial) -> ValueID;
  auto declare_bytes_var(this Module& self, std::string_view name, u32 size) -> ValueID;
  auto declare_callback_var(this Module& self, std::string_view name) -> ValueID;
  // a body set once here, it reads whatever state it needs when the program runs
  auto declare_callback_var(this Module& self, std::string_view name, std::function<auto(Recorder&)->void> body)
    -> ValueID;
  auto declare_acceleration_structure_builds_var(this Module& self, std::string_view name) -> ValueID;
  auto declare_query_pool_var(this Module& self, std::string_view name) -> ValueID;
  auto declare_swapchain_var(this Module& self, std::string_view name) -> ValueID;

  // resources a slot fills in each run, resting at `access` before and after it
  auto declare_buffer_var(this Module& self, std::string_view name, Access access) -> ValueID;
  auto declare_image_var(
    this Module& self, std::string_view name, Format format, Access access, Samples samples = Samples::e1
  ) -> ValueID;

  auto transient_image(this Module& self, const ImageInfo& info) -> ValueID;
  auto transient_image_sized(this Module& self, const ImageInfo& info, ValueID extent) -> ValueID;
  auto import_image(this Module& self, const Image& image, VkImageLayout layout, Access access) -> ValueID;
  auto import_attachment(this Module& self, const ImageAttachment& attachment, Access access) -> ValueID;
  auto transient_buffer(this Module& self, const BufferInfo& info) -> ValueID;
  auto import_buffer(this Module& self, const Buffer& buffer, Access access) -> ValueID;
  auto set_name(this Module& self, ValueID id, std::string_view name) -> ValueID;

  // a range of the root image `image` versions, which depends on `image` itself
  auto view(
    this Module& self,
    ValueID image,
    const Subresource& range,
    Format format = Format::Undefined,
    ImageViewType view_type = ImageViewType::Auto
  ) -> ValueID;
  auto mip(this Module& self, ValueID image, u32 level) -> ValueID;
  auto subrange(this Module& self, ValueID image, u32 base_mip, u32 mip_count, u32 base_layer, u32 layer_count)
    -> ValueID;
  auto whole(this Module& self, ValueID image) -> ValueID;
  // fills mips [base + 1, base + count) by blitting each from the one above, returns the whole image
  auto generate_mips(this Module& self, ValueID image, u32 base_level, u32 level_count) -> ValueID;

  auto acquire_next_image(this Module& self, const SwapChain& swapchain) -> ValueID;
  auto acquire_next_image_from(this Module& self, ValueID swapchain, Format format, Samples samples) -> ValueID;
  auto export_to(this Module& self, ValueID resource, Access access, DomainFlag dst_domain = DomainFlag::Graphics)
    -> ValueID;
  auto present(this Module& self, ValueID attachment) -> ValueID;
  auto keep(this Module& self, ValueID value) -> ValueID;

  // transfer commands outside of a pass
  auto clear(this Module& self, ValueID attachment, const ClearValue& color) -> ValueID;
  auto clear_from(this Module& self, ValueID attachment, ValueID color) -> ValueID;
  auto blit(this Module& self, ValueID src, ValueID dst, VkFilter filter = VK_FILTER_LINEAR) -> ValueID;
  auto copy_buffer_to_image(this Module& self, ValueID buffer, ValueID image) -> ValueID;
  auto copy_buffer_to_image_region(this Module& self, ValueID buffer, ValueID image, const BufferImageCopy& region)
    -> ValueID;
  auto copy_image_to_buffer(this Module& self, ValueID image, ValueID buffer) -> ValueID;
  auto copy_image_to_buffer_region(this Module& self, ValueID image, ValueID buffer, const BufferImageCopy& region)
    -> ValueID;
  auto copy_buffer(this Module& self, ValueID src, ValueID dst, const BufferCopy& region = {}) -> ValueID;
  auto fill_buffer(this Module& self, ValueID buffer, u32 data, u64 offset = 0, u64 size = VK_WHOLE_SIZE) -> ValueID;
  auto update_buffer(this Module& self, ValueID buffer, std::span<const u8> data, u64 offset = 0) -> ValueID;
  auto update_buffer_from(this Module& self, ValueID buffer, ValueID source, u64 offset = 0) -> ValueID;
  auto copy_image(this Module& self, ValueID src, ValueID dst) -> ValueID;
  auto copy_image_region(this Module& self, ValueID src, ValueID dst, const ImageCopy& region) -> ValueID;
  auto resolve(this Module& self, ValueID src, ValueID dst) -> ValueID;
  auto resolve_region(this Module& self, ValueID src, ValueID dst, const ImageCopy& region) -> ValueID;

  auto transient_query_pool(this Module& self, VkQueryType type, u32 count) -> ValueID;
  auto build_acceleration_structures(
    this Module& self,
    ValueID builds,
    std::span<const AccelerationStructureAddress> addresses,
    std::span<const ResourceAccess> resources
  ) -> std::vector<ValueID>;
  auto write_acceleration_structure_properties(
    this Module& self, ValueID source, VkAccelerationStructureKHR structure, ValueID queries, u32 first_query = 0
  ) -> ValueID;
  auto copy_acceleration_structure(
    this Module& self,
    ValueID src,
    ValueID dst,
    VkAccelerationStructureKHR source,
    VkAccelerationStructureKHR destination,
    VkCopyAccelerationStructureModeKHR mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR
  ) -> ValueID;

  // passes, which take every resource they touch with how, and give back a fresh value of each
  auto begin_rendering(this Module& self, std::span<const ResourceAccess> attachments) -> Module&;
  auto begin_rendering(this Module& self, std::initializer_list<ResourceAccess> attachments) -> Module&;
  auto begin_rendering_area(
    this Module& self,
    std::span<const ResourceAccess> attachments,
    Extent2D render_area,
    u32 layer_count = 1,
    u32 view_mask = 0
  ) -> Module&;
  auto begin_rendering_area(
    this Module& self,
    std::initializer_list<ResourceAccess> attachments,
    Extent2D render_area,
    u32 layer_count = 1,
    u32 view_mask = 0
  ) -> Module&;
  auto begin_compute(this Module& self, std::span<const ResourceAccess> attachments) -> Module&;
  auto begin_compute(this Module& self, std::initializer_list<ResourceAccess> attachments) -> Module&;
  auto with_name(this Module& self, std::string_view name) -> Module&;

  auto push_constant_bytes(this Module& self, u32 offset, std::span<const u8> data) -> Module&;
  template <typename T>
  auto push_constants(this Module& self, const T& value) -> Module& {
    return self.push_constants_at(0, value);
  }
  template <typename T>
  auto push_constants_at(this Module& self, u32 offset, const T& value) -> Module& {
    static_assert(std::is_trivially_copyable_v<T>);
    return self.push_constant_bytes(offset, std::span(reinterpret_cast<const u8*>(&value), sizeof(T)));
  }
  auto push_constants_from(this Module& self, ValueID variable) -> Module&;
  auto push_constants_from_at(this Module& self, u32 offset, ValueID variable) -> Module&;
  auto push_constant_address(this Module& self, u32 offset, ValueID buffer) -> Module&;
  auto specialize(this Module& self, u32 id, SpecValue value) -> Module&;
  template <typename T>
  auto specialize_constant(this Module& self, u32 id, T value) -> Module& {
    return self.specialize(id, SpecValue::from(value));
  }

  auto bind_graphics_pipeline(this Module& self, PipelineID pipeline) -> Module&;
  auto bind_compute_pipeline(this Module& self, PipelineID pipeline) -> Module&;
  auto bind_ray_tracing_pipeline(this Module& self, PipelineID pipeline) -> Module&;

  auto bind_image(this Module& self, u32 set, u32 binding, ValueID image) -> Module&;
  auto bind_image(this Module& self, u32 set, u32 binding, ValueID image, Format view_format) -> Module&;
  auto bind_texture(this Module& self, u32 set, u32 binding, ValueID image, VkSampler sampler) -> Module&;
  auto bind_sampler(this Module& self, u32 set, u32 binding, VkSampler sampler) -> Module&;
  auto bind_buffer(this Module& self, u32 set, u32 binding, ValueID buffer) -> Module&;
  auto bind_buffer_range(this Module& self, u32 set, u32 binding, ValueID buffer, u64 offset, u64 range) -> Module&;
  auto bind_texel_buffer(this Module& self, u32 set, u32 binding, ValueID buffer, VkBufferView view) -> Module&;
  auto bind_acceleration_structure(
    this Module& self, u32 set, u32 binding, ValueID backing_buffer, VkAccelerationStructureKHR acceleration_structure
  ) -> Module&;

  auto bind_vertex_buffer(this Module& self, u32 binding, ValueID buffer, u64 offset = 0) -> Module&;
  auto bind_vertex_buffers(
    this Module& self, u32 first_binding, std::span<const ValueID> buffers, std::span<const u64> offsets
  ) -> Module&;
  auto bind_index_buffer(this Module& self, ValueID buffer, VkIndexType index_type, u64 offset = 0) -> Module&;

  auto set_primitive_topology(this Module& self, VkPrimitiveTopology topology) -> Module&;
  auto set_patch_control_points(this Module& self, u32 count) -> Module&;
  auto set_depth(this Module& self, const DepthState& depth) -> Module&;
  auto set_stencil(this Module& self, const StencilState& stencil) -> Module&;
  auto set_rasterization(this Module& self, const RasterizationState& rasterization) -> Module&;
  auto set_dynamic_state(this Module& self, DynamicStateFlags dynamic) -> Module&;
  auto set_viewport(this Module& self, u32 index, const Viewport& viewport) -> Module&;
  auto set_viewport(this Module& self, u32 index, const Rect2D& rect) -> Module&;
  auto set_scissor(this Module& self, u32 index, const Rect2D& rect) -> Module&;
  auto set_color_blend(this Module& self, u32 index, const ColorBlendAttachmentState& blend) -> Module&;
  auto set_color_blend(this Module& self, u32 index, BlendPreset preset) -> Module&;
  auto broadcast_color_blend(this Module& self, const ColorBlendAttachmentState& blend) -> Module&;
  auto broadcast_color_blend(this Module& self, BlendPreset preset) -> Module&;

  auto draw(this Module& self, Count vertex_count, Count instance_count) -> Module&;
  auto draw_range(this Module& self, Count vertex_count, Count instance_count, Count first_vertex, Count first_instance)
    -> Module&;
  auto draw_indexed(this Module& self, Count index_count, Count instance_count) -> Module&;
  auto draw_indexed_range(
    this Module& self,
    Count index_count,
    Count instance_count,
    Count first_index,
    i32 vertex_offset,
    Count first_instance
  ) -> Module&;
  auto draw_indirect(
    this Module& self, ValueID buffer, Count draw_count, u64 offset = 0, u32 stride = DRAW_INDIRECT_STRIDE
  ) -> Module&;
  auto draw_indexed_indirect(
    this Module& self, ValueID buffer, Count draw_count, u64 offset = 0, u32 stride = DRAW_INDEXED_INDIRECT_STRIDE
  ) -> Module&;
  auto draw_indirect_count(
    this Module& self,
    ValueID buffer,
    u64 offset,
    ValueID count_buffer,
    u64 count_offset,
    Count max_draw_count,
    u32 stride = DRAW_INDIRECT_STRIDE
  ) -> Module&;
  auto draw_indexed_indirect_count(
    this Module& self,
    ValueID buffer,
    u64 offset,
    ValueID count_buffer,
    u64 count_offset,
    Count max_draw_count,
    u32 stride = DRAW_INDEXED_INDIRECT_STRIDE
  ) -> Module&;
  auto draw_mesh_tasks(this Module& self, Count x, Count y, Count z) -> Module&;
  auto draw_mesh_tasks_indirect(
    this Module& self, ValueID buffer, Count draw_count, u64 offset = 0, u32 stride = DRAW_MESH_TASKS_INDIRECT_STRIDE
  ) -> Module&;
  auto draw_mesh_tasks_indirect_count(
    this Module& self,
    ValueID buffer,
    u64 offset,
    ValueID count_buffer,
    u64 count_offset,
    Count max_draw_count,
    u32 stride = DRAW_MESH_TASKS_INDIRECT_STRIDE
  ) -> Module&;
  auto record_from(this Module& self, ValueID body) -> Module&;

  auto dispatch(this Module& self, Count groups_x, Count groups_y, Count groups_z) -> Module&;
  auto dispatch_invocations(this Module& self, Count x, Count y, Count z) -> Module&;
  auto dispatch_invocations_per_pixel(this Module& self, ValueID image, std::array<f32, 3> scale = {1.0f, 1.0f, 1.0f})
    -> Module&;
  auto dispatch_invocations_per_element(this Module& self, ValueID buffer, u64 element_size, f32 scale = 1.0f)
    -> Module&;
  auto dispatch_indirect(this Module& self, ValueID buffer, u64 offset = 0) -> Module&;
  auto trace_rays(this Module& self, Count width, Count height, Count depth) -> Module&;

  auto end_rendering_values(this Module& self) -> std::vector<ValueID>;
  auto end_compute_values(this Module& self) -> std::vector<ValueID>;

  template <usize N>
  auto end_rendering(this Module& self) -> std::array<ValueID, N> {
    return to_array<N>(self.end_rendering_values());
  }

  template <usize N>
  auto end_compute(this Module& self) -> std::array<ValueID, N> {
    return to_array<N>(self.end_compute_values());
  }

  // selection on a bool value, both arms give back the same shape and the result is the phi of them
  template <typename ThenF, typename ElseF>
  auto set_condition(this Module& self, ValueID condition, ThenF&& then, ElseF&& otherwise) {
    const auto [merge, true_label, false_label] = self.open_selection(condition);
    auto taken = then(self);
    const auto taken_block = self.current_block;
    self.close_arm(merge);
    self.place_label(false_label);
    auto skipped = otherwise(self);
    const auto skipped_block = self.current_block;
    self.close_arm(merge);
    self.place_label(merge);
    if constexpr (std::is_same_v<decltype(taken), ValueID>) {
      return self.phi({{taken, taken_block}, {skipped, skipped_block}});
    } else {
      auto merged = decltype(taken){};
      for (usize index = 0; index < taken.size(); index++) {
        merged[index] = self.phi({{taken[index], taken_block}, {skipped[index], skipped_block}});
      }

      return merged;
    }
  }

  auto compile(this const Module& self, const PipelineBindings& pipelines, ValueID root)
    -> std::expected<Program, VkResult>;
  auto compile_all(this const Module& self, const PipelineBindings& pipelines, std::span<const ValueID> roots)
    -> std::expected<Program, VkResult>;

  // internals the lowering passes share
  static auto check_result_count(usize declared, usize expected) -> void;

  template <usize N>
  static auto to_array(const std::vector<ValueID>& values) -> std::array<ValueID, N> {
    check_result_count(values.size(), N);
    auto result = std::array<ValueID, N>{};
    std::ranges::copy(values, result.begin());
    return result;
  }

  auto get(this const Module& self, ValueID id) -> const IR&;
  auto emit(this Module& self, IR ir) -> ValueID;
  auto lower_type(this Module& self, const ir::Type& type) -> ValueID;
  auto lower_constant(this Module& self, const ir::Constant& constant) -> ValueID;
  auto lower_u32(this Module& self, u32 value) -> ValueID;
  auto lower_access(this Module& self, Access access) -> ValueID;
  auto lower_name(this Module& self, std::string_view name) -> ValueID;
  auto lower_count(this Module& self, Count count) -> ValueID;
  auto resolve_access(this const Module& self, ValueID id) -> Access;
  auto resource_root(this const Module& self, ValueID id) -> ValueID;
  auto resource_range(this const Module& self, ValueID id) -> Subresource;
  auto resource_elements(this const Module& self, ValueID id) -> std::vector<ValueID>;
  auto resolve_resource(this const Module& self, ValueID id) -> const IR*;
  auto is_buffer(this const Module& self, ValueID id) -> bool;
  auto is_image(this const Module& self, ValueID id) -> bool;
  auto image_format(this const Module& self, ValueID id) -> option<std::pair<Format, Samples>>;
  auto image_extent(this const Module& self, ValueID id) -> option<Extent3D>;
  auto buffer_size(this const Module& self, ValueID id) -> option<u64>;
  auto extent_2d(this const Module& self, ValueID id) -> option<Extent2D>;
  auto block_of(this const Module& self, ValueID id) -> LabelID;
  auto array_elements(this const Module& self, ValueID id) -> std::span<const ValueID>;
  auto resource_uses(this const Module& self, const IR& ir, std::vector<ValueID>& uses) -> void;

  auto declare_label(this Module& self) -> LabelID;
  auto place_label(this Module& self, LabelID label) -> void;
  auto terminate(this Module& self, const Terminator& terminator) -> void;
  auto open_selection(this Module& self, ValueID condition) -> std::array<LabelID, 3>;
  auto close_arm(this Module& self, LabelID merge) -> void;
  auto phi(this Module& self, std::initializer_list<std::pair<ValueID, LabelID>> incoming) -> ValueID;
  auto declare_var(this Module& self, ir::VariableKind kind, std::string_view name, Value initial) -> ValueID;

  auto open_pass(this Module& self, PassKind kind, ValueID begin, std::vector<ValueID> resources) -> Module&;
  auto expect_no_open_pass(this Module& self, std::string_view when) -> void;
  auto chain(this Module& self, option<PassKind> wanted, std::string_view what, IR ir) -> Module&;
  auto set_state(this Module& self, StateChange change) -> Module&;
  auto pass_state(this Module& self, std::string_view what, StateChange change) -> Module&;
  auto write_descriptor(this Module& self, u32 set, u32 binding, const ir::Descriptor& descriptor) -> Module&;
  auto end_pass(this Module& self, PassKind kind) -> std::vector<ValueID>;
  auto lower_attachments(this Module& self, std::span<const ResourceAccess> attachments)
    -> std::vector<std::pair<ValueID, ValueID>>;

  auto lower(this const Module& self, const PipelineBindings& pipelines, std::span<const ValueID> roots)
    -> std::expected<std::vector<Instr>, VkResult>;
  auto topo_sort(this const Module& self, std::span<const ValueID> roots) -> std::vector<Instr>;
  auto layout_blocks(this const Module& self, std::vector<Instr> nodes, u32& next_id) -> std::vector<Instr>;
  auto infer(this const Module& self, std::vector<Instr> nodes) -> std::vector<Instr>;
  auto sync(this const Module& self, std::vector<Instr> nodes, u32& next_id) -> std::vector<Instr>;
  auto fold_clears(this const Module& self, std::vector<Instr> nodes) -> std::vector<Instr>;
  auto schedule_barriers(this const Module& self, std::vector<Instr> nodes) -> std::vector<Instr>;
  auto infer_usage(this const Module& self, std::vector<Instr>& nodes) -> void;
};

auto resolve_descriptors(std::vector<Instr> nodes, const PipelineBindings& pipelines, u32& next_id)
  -> std::expected<std::vector<Instr>, VkResult>;
auto simplify_cfg(std::vector<Instr> nodes) -> std::vector<Instr>;
auto fold_barriers(std::vector<Instr> nodes, u32& next_id) -> std::vector<Instr>;
auto normalize_barriers(std::vector<Instr> nodes) -> std::vector<Instr>;
auto globals_first(std::vector<Instr> nodes) -> std::vector<Instr>;
auto analyze_descriptors(const Program& program, const PipelineBindings& pipelines) -> std::expected<void, VkResult>;
auto dump(std::span<const Instr> instructions, std::span<const ValueID> bound, bool is_highlighted) -> std::string;
} // namespace ox::vir
