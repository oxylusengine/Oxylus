#pragma once

#include <expected>
#include <memory>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <vir/Types.hpp>
#include <vulkan/vulkan_core.h>

#include "Render/ContextCVar.hpp"
#include "Render/Fwd.hpp"

namespace ox {
struct AssetFile;
struct Window;
struct ShaderPipelineData;
struct DescriptorSet;
struct RenderDevice;
struct UploadBatch;
class TracyProfiler;

struct SwapchainInfo {
  vir::Format format = vir::Format::Undefined;
  vir::Extent2D extent = {};
  u64 generation = 0;
};

struct GPUAccelerationStructure {
  VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
  BufferID buffer = BufferID::Invalid;
  VkDeviceAddress device_address = 0;
  u64 size = 0;
};

enum : u32 {
  DescriptorTable_SamplerIndex = 0,
  DescriptorTable_SampledImageIndex,
  DescriptorTable_StorageImageIndex,
};

class RenderContext {
public:
  enum class Feature : u32 {
    None = 0,
    MeshShaders = 1 << 0,
    RayTracing = 1 << 1,
    RayTracingPipeline = 1 << 2,
  };

  Feature features = {};
  ContextCVar context_cvar = {};
  VkDevice device = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device = VK_NULL_HANDLE;
  VkQueue graphics_queue = VK_NULL_HANDLE;
  VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
  u32 num_inflight_frames = 3;
  u64 num_frames = 0;
  u32 current_frame = 0;
  std::string device_name = {};
  u32 device_api_version = 0;

  std::unique_ptr<vir::Context> context = {};
  std::unique_ptr<vir::PersistentAllocator> persistent = {};
  std::unique_ptr<vir::SuperFrameAllocator> super_frame = {};
  std::unique_ptr<vir::RenderGraph> graph = {};
  std::unique_ptr<RenderResources> resources = {};
  vir::FrameAllocator* frame = nullptr;
  std::shared_mutex frame_mutex = {};
  std::shared_mutex descriptor_mutex = {};
  std::unique_ptr<TracyProfiler> tracy_profiler = {};
  std::unique_ptr<RenderDevice> render_device = {};

  RenderContext();
  ~RenderContext();
  RenderContext(const RenderContext&) = delete;
  auto operator=(const RenderContext&) -> RenderContext& = delete;

  auto create_context(this RenderContext& self, const Window& window, bool vulkan_validation_layers) -> void;
  // also supports a borrowed device, its graphics queue must already exist
  auto initialize_graph(this RenderContext& self, std::unique_ptr<vir::Context> context)
    -> std::expected<void, VkResult>;
  auto destroy_context(this RenderContext& self) -> void;

  auto new_frame(this RenderContext& self) -> std::expected<SwapchainInfo, VkResult>;
  auto handle_resize(this RenderContext& self) -> void;
  auto recreate_swapchain(this RenderContext& self, vir::Extent2D extent) -> std::expected<void, VkResult>;
  auto swapchain_info(this const RenderContext& self) -> SwapchainInfo;
  auto acquire_next_image(this RenderContext& self, vir::Module& module) -> vir::ValueID;
  auto set_vsync(this RenderContext& self, bool enable) -> void;
  auto is_vsync(this const RenderContext& self) -> bool;

  auto compile(this RenderContext& self, vir::Module&& module) -> std::expected<vir::Program, VkResult>;
  auto execute(this RenderContext& self, const vir::Program& program) -> std::expected<vir::SubmitTicket, VkResult>;
  auto execute(this RenderContext& self, const vir::Program& program, vir::FrameAllocator& allocator)
    -> std::expected<vir::SubmitTicket, VkResult>;
  auto execute_blocking(this RenderContext& self, vir::Module&& module) -> std::expected<vir::SubmitTicket, VkResult>;
  auto execute_blocking(this RenderContext& self, const vir::Program& program)
    -> std::expected<vir::SubmitTicket, VkResult>;
  // host visible, written and flushed, owned by the caller until the upload that reads it retires
  auto allocate_staging(this RenderContext& self, std::span<const u8> data) -> std::expected<vir::Buffer, VkResult>;
  // runs a one-shot module on an allocator of its own, then either waits for it or hands it and the
  // staging buffers to `batch` to settle later
  auto submit_upload(
    this RenderContext& self, vir::Module&& module, std::vector<vir::Buffer> staging, UploadBatch* batch
  ) -> std::expected<vir::SubmitTicket, VkResult>;
  auto wait(this const RenderContext& self, vir::SubmitTicket ticket) -> std::expected<void, VkResult>;
  auto is_retired(this const RenderContext& self, vir::SubmitTicket ticket) -> bool;
  auto wait(this const RenderContext& self) -> void;

  auto create_persistent_descriptor_set(
    this RenderContext& self,
    std::span<const VkDescriptorSetLayoutBinding> bindings,
    std::span<const VkDescriptorBindingFlags> binding_flags
  ) -> std::expected<DescriptorSet, VkResult>;
  auto commit_descriptor_set(this RenderContext& self, std::span<const VkWriteDescriptorSet> writes) -> void;
  auto bindless_set(this const RenderContext& self) -> vir::BindlessDescriptorSet;

  auto create_pipeline(this RenderContext& self, const ShaderPipelineData& pipeline_data) -> bool;
  auto pipeline(this const RenderContext& self, std::string_view name) -> vir::PipelineID;
  auto pipeline(this const RenderContext& self, PipelineID id) -> vir::PipelineID;
  auto load_shader_pack(this RenderContext& self, const AssetFile& pack) -> void;
  auto use_mesh_shaders(this const RenderContext& self) -> bool;
  auto use_ray_tracing(this const RenderContext& self) -> bool;
  auto use_ray_tracing_pipeline(this const RenderContext& self) -> bool;
  auto as_scratch_alignment(this const RenderContext& self) -> u64;

  auto allocate_buffer(this RenderContext& self, const vir::BufferInfo& info) -> std::expected<BufferID, VkResult>;
  auto destroy_buffer(this RenderContext& self, BufferID id) -> void;
  auto buffer(this const RenderContext& self, BufferID id) -> vir::Buffer;
  auto allocate_frame_buffer(this RenderContext& self, const vir::BufferInfo& info)
    -> std::expected<vir::Buffer, VkResult>;
  // host data a shader reads this frame, written into the frame allocator. Never empty, so a frame
  // with nothing to upload still has a buffer to bind
  auto upload_frame_buffer(this RenderContext& self, std::span<const u8> data, std::string_view name)
    -> std::expected<vir::Buffer, VkResult>;
  // grows `id` to hold at least `size` bytes, doubling so a slowly growing scene settles. The old
  // buffer retires, so frames still in flight keep reading it. True when it was reallocated
  auto resize_buffer(this RenderContext& self, BufferID& id, u64 size, std::string_view name)
    -> std::expected<bool, VkResult>;
  auto staging_buffer(
    this RenderContext& self, vir::FrameAllocator& allocator, std::span<const u8> data, u64 alignment = 4
  ) -> std::expected<vir::Buffer, VkResult>;

  auto allocate_image(this RenderContext& self, const vir::ImageInfo& info) -> std::expected<ImageID, VkResult>;
  auto destroy_image(this RenderContext& self, ImageID id) -> void;
  auto image(this const RenderContext& self, ImageID id) -> vir::Image;
  auto allocate_image_view(this RenderContext& self, const vir::ImageViewInfo& info, bool write_bindless = true)
    -> std::expected<ImageViewID, VkResult>;
  auto destroy_image_view(this RenderContext& self, ImageViewID id) -> void;
  auto image_view(this const RenderContext& self, ImageViewID id) -> vir::ImageView;
  auto allocate_sampler(this RenderContext& self, const vir::SamplerInfo& info, bool write_bindless = true)
    -> std::expected<SamplerID, VkResult>;
  auto destroy_sampler(this RenderContext& self, SamplerID id) -> void;
  // samplers built once per description and kept for the context's lifetime, bindless included
  auto cached_sampler(this RenderContext& self, const vir::SamplerInfo& info) -> SamplerID;
  auto cached_sampler_handle(this RenderContext& self, const vir::SamplerInfo& info) -> VkSampler;
  auto sampler(this const RenderContext& self, SamplerID id) -> vir::Sampler;
  auto allocate_acceleration_structure(
    this RenderContext& self, VkAccelerationStructureTypeKHR type, u64 size, std::string_view name
  ) -> std::expected<AccelerationStructureID, VkResult>;
  // a structure placed in a slice of a buffer the caller owns, which outlives it
  auto create_acceleration_structure_in(
    this RenderContext& self, VkAccelerationStructureTypeKHR type, BufferID buffer, u64 offset, u64 size
  ) -> std::expected<AccelerationStructureID, VkResult>;
  // retires the structure, and the buffer under it when it owns one
  auto destroy_acceleration_structure(this RenderContext& self, AccelerationStructureID id) -> void;
  auto acceleration_structure(this const RenderContext& self, AccelerationStructureID id) -> GPUAccelerationStructure;
  auto collect_retired(this RenderContext& self) -> void;
  auto seal_retirements(this RenderContext& self, vir::SubmitTicket frame_ticket) -> void;

  auto get_accel_structure_device_address(this const RenderContext& self, VkAccelerationStructureKHR handle) -> u64;
  auto create_query_pool(this const RenderContext& self, VkQueryType type, u32 count) -> VkQueryPool;
  auto destroy_query_pool(this const RenderContext& self, VkQueryPool pool) -> void;
  auto read_query_pool(this const RenderContext& self, VkQueryPool pool, std::span<u64> results) -> bool;
};

consteval auto enable_bitmask(RenderContext::Feature) -> void;
} // namespace ox
