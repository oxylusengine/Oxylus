#pragma once

#include <map>
#include <memory>
#include <shared_mutex>

#include "Render/Graph/Allocator.hpp"
#include "Render/Graph/Module.hpp"
#include "Render/Graph/Pipeline.hpp"

namespace ox::vir {
// hooks a profiler hangs GPU zones on
struct RenderGraphCallbacks {
  void* user = nullptr;
  auto (*on_begin_command_buffer)(void* user, VkCommandBuffer command_buffer) -> void = nullptr;
  auto (*on_begin_pass)(void* user, std::string_view name, VkCommandBuffer command_buffer) -> void* = nullptr;
  auto (*on_end_pass)(void* user, void* pass_data, VkCommandBuffer command_buffer) -> void = nullptr;
};

enum class PipelineKind : u8 { Graphics, Compute, RayTracing };

struct ShaderBindingTableRegion {
  u64 offset = 0;
  u64 stride = 0;
  u64 size = 0;
};

struct ShaderBindingTableLayout {
  // raygen, miss, hit, callable
  std::array<ShaderBindingTableRegion, 4> regions = {};
  u64 size = 0;
};

auto shader_binding_table_layout(const RayTracingProperties& properties, std::span<const u32, 4> counts)
  -> std::expected<ShaderBindingTableLayout, VkResult>;

struct RayTracingVariant {
  VkPipeline pipeline = VK_NULL_HANDLE;
  Buffer shader_binding_table = {};
  VkStridedDeviceAddressRegionKHR raygen = {};
  VkStridedDeviceAddressRegionKHR miss = {};
  VkStridedDeviceAddressRegionKHR hit = {};
  VkStridedDeviceAddressRegionKHR callable = {};
};

struct DeclaredPipeline {
  PipelineKind kind = PipelineKind::Graphics;
  std::string name = {};
  std::vector<ShaderLayout> reflections = {};
  std::vector<DescriptorBinding> bindings = {};
  option<BindlessDescriptorSet> bindless = nullopt;
  option<PipelineLayout> layout = nullopt;

  GraphicsPipelineInfo graphics = {};
  VertexLayout vertex = {};
  ankerl::unordered_dense::map<PipelineState, VkPipeline, PipelineStateHash> graphics_variants = {};

  ComputePipelineInfo compute = {};
  ankerl::unordered_dense::map<Specialization, VkPipeline, SpecializationHash> compute_variants = {};

  RayTracingPipelineInfo ray_tracing = {};
  std::vector<RayTracingShaderGroup> groups = {};
  ankerl::unordered_dense::map<Specialization, RayTracingVariant, SpecializationHash> ray_tracing_variants = {};

  auto bind_point(this const DeclaredPipeline& self) -> VkPipelineBindPoint;
};

struct ResolvedDescriptor {
  enum class Kind : u8 { Sampler, Image, CombinedImageSampler, TexelBuffer, Buffer, AccelerationStructure };

  u32 set = 0;
  u32 binding = 0;
  option<VkDescriptorType> descriptor_type = nullopt;
  Kind kind = Kind::Image;
  VkSampler sampler = VK_NULL_HANDLE;
  VkImageView image_view = VK_NULL_HANDLE;
  VkImageLayout image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkBufferView view = VK_NULL_HANDLE;
  u64 buffer_size = 0;
  u64 base_offset = 0;
  u64 offset = 0;
  u64 range = VK_WHOLE_SIZE;
  VkAccelerationStructureKHR acceleration_structure = VK_NULL_HANDLE;

  auto validate(this const ResolvedDescriptor& self) -> std::expected<void, VkResult>;
  auto supports_type(this const ResolvedDescriptor& self, VkDescriptorType type) -> bool;
  auto operator==(const ResolvedDescriptor& other) const -> bool;
};

struct Execution;

// the command buffer a callback records into, with the descriptors and state of the pass it sits in
struct Recorder {
  Execution* execution = nullptr;
  PipelineID pipeline = PipelineID::Invalid;
  std::map<std::pair<u32, u32>, ResolvedDescriptor> descriptors = {};
  std::expected<void, VkResult> result = {};
  bool has_transfer_command = false;

  auto command_buffer(this const Recorder& self) -> VkCommandBuffer;
  auto functions(this const Recorder& self) -> const VulkanFunctions&;
  auto render_area(this const Recorder& self) -> VkRect2D;
  auto buffer(this const Recorder& self, ValueID id) -> Buffer;
  auto image(this const Recorder& self, ValueID id) -> ImageAttachment;

  auto set_viewport(this Recorder& self, u32 index, const Viewport& viewport) -> Recorder&;
  auto set_scissor(this Recorder& self, u32 index, const Rect2D& rect) -> Recorder&;
  auto set_scissor_absolute(this Recorder& self, const VkRect2D& rect) -> Recorder&;
  auto push_constant_bytes(this Recorder& self, u32 offset, std::span<const u8> data) -> Recorder&;
  template <typename T>
  auto push_constants(this Recorder& self, const T& value, u32 offset = 0) -> Recorder& {
    static_assert(std::is_trivially_copyable_v<T>);
    return self.push_constant_bytes(offset, std::span(reinterpret_cast<const u8*>(&value), sizeof(T)));
  }

  auto bind_image(this Recorder& self, u32 set, u32 binding, const ImageAttachment& image) -> Recorder&;
  auto bind_texture(this Recorder& self, u32 set, u32 binding, const ImageAttachment& image, VkSampler sampler)
    -> Recorder&;
  auto bind_sampler(this Recorder& self, u32 set, u32 binding, VkSampler sampler) -> Recorder&;
  auto bind_buffer(
    this Recorder& self, u32 set, u32 binding, const Buffer& buffer, u64 offset = 0, u64 range = VK_WHOLE_SIZE
  ) -> Recorder&;
  auto bind_vertex_buffer(this Recorder& self, u32 binding, const Buffer& buffer, u64 offset = 0) -> Recorder&;
  auto bind_index_buffer(this Recorder& self, const Buffer& buffer, VkIndexType index_type, u64 offset = 0)
    -> Recorder&;

  // transfer callbacks run in compute regions with their resource accesses declared on the pass
  auto copy_buffer(this Recorder& self, const Buffer& src, const Buffer& dst, const BufferCopy& region = {})
    -> Recorder&;
  auto fill_buffer(this Recorder& self, const Buffer& buffer, u32 data, u64 offset = 0, u64 size = VK_WHOLE_SIZE)
    -> Recorder&;
  auto update_buffer(this Recorder& self, const Buffer& buffer, std::span<const u8> data, u64 offset = 0) -> Recorder&;
  auto copy_image(
    this Recorder& self, const ImageAttachment& src, const ImageAttachment& dst, option<ImageCopy> region = nullopt
  ) -> Recorder&;
  auto resolve(
    this Recorder& self, const ImageAttachment& src, const ImageAttachment& dst, option<ImageCopy> region = nullopt
  ) -> Recorder&;
  auto copy_buffer_to_image(
    this Recorder& self, const Buffer& buffer, const ImageAttachment& image, option<BufferImageCopy> region = nullopt
  ) -> Recorder&;
  auto copy_image_to_buffer(
    this Recorder& self, const ImageAttachment& image, const Buffer& buffer, option<BufferImageCopy> region = nullopt
  ) -> Recorder&;
  auto transfer_command(this Recorder& self) -> option<CommandBuffer>;

  auto draw(this Recorder& self, u32 vertex_count, u32 instance_count, u32 first_vertex = 0, u32 first_instance = 0)
    -> Recorder&;
  auto dispatch(this Recorder& self, u32 groups_x, u32 groups_y, u32 groups_z) -> Recorder&;
  auto draw_indexed(
    this Recorder& self,
    u32 index_count,
    u32 instance_count,
    u32 first_index = 0,
    i32 vertex_offset = 0,
    u32 first_instance = 0
  ) -> Recorder&;
  auto draw_indexed_indirect(
    this Recorder& self, const Buffer& buffer, u64 offset, u32 draw_count, u32 stride = DRAW_INDEXED_INDIRECT_STRIDE
  ) -> Recorder&;

  auto bind_descriptors(this Recorder& self) -> std::expected<void, VkResult>;
  auto write_descriptor(this Recorder& self, ResolvedDescriptor descriptor) -> Recorder&;
};

struct RetiredDescriptorArena {
  std::vector<VkDescriptorPool> pools = {};
  SubmitTicket ticket = {};
};

struct RenderGraph {
  Context* context = nullptr;
  PersistentAllocator* persistent = nullptr;
  RenderGraphCallbacks callbacks = {};
  // declared pipelines and their variants, the variants grow while programs execute
  std::shared_mutex pipelines_mutex = {};
  std::vector<std::unique_ptr<DeclaredPipeline>> pipelines = {};
  std::mutex retired_mutex = {};
  std::vector<RetiredDescriptorArena> retired_arenas = {};
  std::mutex warned_mutex = {};
  ankerl::unordered_dense::set<PipelineID> warned_push_constants = {};

  RenderGraph(Context& ctx, PersistentAllocator& allocator);
  RenderGraph(const RenderGraph&) = delete;
  RenderGraph(RenderGraph&&) = delete;
  ~RenderGraph();

  auto operator=(const RenderGraph&) -> RenderGraph& = delete;
  auto operator=(RenderGraph&&) -> RenderGraph& = delete;

  auto declare_graphics_pipeline(this RenderGraph& self, GraphicsPipelineInfo info)
    -> std::expected<PipelineID, VkResult>;
  auto declare_compute_pipeline(this RenderGraph& self, ComputePipelineInfo info)
    -> std::expected<PipelineID, VkResult>;
  auto declare_ray_tracing_pipeline(this RenderGraph& self, RayTracingPipelineInfo info)
    -> std::expected<PipelineID, VkResult>;
  auto pipeline_bindings(this RenderGraph& self) -> PipelineBindings;
  auto local_size(this RenderGraph& self, PipelineID pipeline) -> std::array<u32, 3>;

  // records and submits the program, its transients come out of `allocator` and live until the
  // allocator retires, later submissions wait for query writers outside the queue mutex
  auto execute(this RenderGraph& self, const Program& program, FrameAllocator& allocator)
    -> std::expected<SubmitTicket, VkResult>;
  // runs a program on an allocator of its own and waits for it to finish
  auto execute_blocking(this RenderGraph& self, const Program& program) -> std::expected<void, VkResult>;

  auto construct_pipelines(this RenderGraph& self, std::span<const Instr> instructions)
    -> std::expected<void, VkResult>;
  auto collect_descriptor_arenas(this RenderGraph& self) -> void;
  auto destroy_pools(this RenderGraph& self, std::span<const VkDescriptorPool> pools) -> void;
};
} // namespace ox::vir
