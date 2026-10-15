#pragma once

#include <ankerl/unordered_dense.h>
#include <expected>
#include <mutex>
#include <thread>
#include <vector>

#include "Render/Graph/Context.hpp"
#include "Render/Graph/Resource.hpp"
#include "Render/Graph/Types.hpp"

typedef struct VmaAllocation_T* VmaAllocation;

namespace ox::vir {
auto align_up_checked(u64 value, u64 alignment) -> option<u64>;
auto allocation_alignment(const BufferInfo& info, const VkPhysicalDeviceLimits& limits) -> option<u64>;

struct CommandPoolKey {
  std::thread::id thread = {};
  u32 family = 0;
  auto operator==(const CommandPoolKey&) const -> bool = default;
};

struct CommandPoolKeyHash {
  using is_avalanching = void;
  auto operator()(const CommandPoolKey& key) const noexcept -> u64;
};

struct ThreadCommandPool {
  VkCommandPool handle = VK_NULL_HANDLE;
  std::vector<VkCommandBuffer> buffers = {};
};

struct PersistentAllocator {
  struct PoolKey {
    MemoryLocation location = MemoryLocation::GpuOnly;
    VkBufferUsageFlags usage = 0;

    auto operator==(const PoolKey&) const -> bool = default;
  };

  struct PoolKeyHash {
    using is_avalanching = void;
    auto operator()(const PoolKey& key) const noexcept -> u64;
  };

  Context* context = nullptr;
  std::vector<u32> queue_families = {};
  // guards every map below, the allocator is shared by every thread that records
  std::mutex mutex = {};
  u64 next_allocation_id = 1;
  ankerl::unordered_dense::map<u64, Buffer> buffer_allocations = {};
  ankerl::unordered_dense::map<VkBuffer, VmaAllocation> raw_buffers = {};
  ankerl::unordered_dense::map<VkImage, VmaAllocation> images = {};
  ankerl::unordered_dense::map<CommandPoolKey, ThreadCommandPool, CommandPoolKeyHash> command_pools = {};

  explicit PersistentAllocator(Context& ctx);
  PersistentAllocator(const PersistentAllocator&) = delete;
  PersistentAllocator(PersistentAllocator&&) = delete;
  ~PersistentAllocator();

  auto operator=(const PersistentAllocator&) -> PersistentAllocator& = delete;
  auto operator=(PersistentAllocator&&) -> PersistentAllocator& = delete;

  auto allocate_command_pool(this PersistentAllocator& self, u32 queue_family)
    -> std::expected<VkCommandPool, VkResult>;
  auto reset_command_pool(this PersistentAllocator& self, VkCommandPool pool) -> VkResult;
  auto destroy_command_pool(this PersistentAllocator& self, VkCommandPool pool) -> void;
  auto free_command_buffers(this PersistentAllocator& self) -> void;

  auto allocate_binary_semaphore(this PersistentAllocator& self) -> std::expected<VkSemaphore, VkResult>;
  auto deallocate_semaphore(this PersistentAllocator& self, VkSemaphore semaphore) -> void;
  auto allocate_command_buffer(this PersistentAllocator& self, u32 queue_family)
    -> std::expected<CommandBuffer, VkResult>;
  auto allocate_image_view(
    this PersistentAllocator& self,
    VkImage image,
    Format format,
    ImageViewType view_type,
    const VkImageSubresourceRange& range
  ) -> std::expected<VkImageView, VkResult>;
  auto deallocate_image_view(this PersistentAllocator& self, VkImageView view) -> void;
  auto allocate_sampler(this PersistentAllocator& self, const SamplerInfo& info) -> std::expected<VkSampler, VkResult>;
  auto deallocate_sampler(this PersistentAllocator& self, VkSampler sampler) -> void;
  auto allocate_query_pool(this PersistentAllocator& self, VkQueryType type, u32 count)
    -> std::expected<QueryPool, VkResult>;
  auto deallocate_query_pool(this PersistentAllocator& self, const QueryPool& pool) -> void;
  auto allocate_buffer(this PersistentAllocator& self, const BufferInfo& info) -> std::expected<Buffer, VkResult>;
  auto deallocate_buffer(this PersistentAllocator& self, const Buffer& buffer) -> void;
  // flush after host writes, invalidate after waiting for GPU writes to finish
  auto flush_buffer(this PersistentAllocator& self, const Buffer& buffer, u64 offset = 0, u64 size = VK_WHOLE_SIZE)
    -> std::expected<void, VkResult>;
  auto invalidate_buffer(this PersistentAllocator& self, const Buffer& buffer, u64 offset = 0, u64 size = VK_WHOLE_SIZE)
    -> std::expected<void, VkResult>;
  auto allocate_image(this PersistentAllocator& self, const ImageInfo& info) -> std::expected<Image, VkResult>;
  auto deallocate_image(this PersistentAllocator& self, const Image& image) -> void;

  auto allocate_raw_buffer(this PersistentAllocator& self, const BufferInfo& info) -> std::expected<Buffer, VkResult>;
  auto deallocate_raw_buffer(this PersistentAllocator& self, const Buffer& buffer) -> void;
};

struct ImageCacheKey {
  Extent3D extent = {};
  Format format = Format::Undefined;
  ImageUsageFlags usage = ImageUsageFlags::None;
  ImageType image_type = ImageType::e2D;
  ImageCreateFlags flags = ImageCreateFlags::None;
  u32 mip_levels = 1;
  u32 array_layers = 1;
  Samples samples = Samples::e1;
  MemoryLocation location = MemoryLocation::GpuOnly;
  std::vector<Format> view_formats = {};

  static auto from(const ImageInfo& info) -> ImageCacheKey;
  auto operator==(const ImageCacheKey& other) const -> bool;
};

struct ImageCacheKeyHash {
  using is_avalanching = void;
  auto operator()(const ImageCacheKey& key) const noexcept -> u64;
};

// the n-th request for an image shape within a frame gets the n-th cached image of that shape
struct FrameImageCache {
  static constexpr usize IDLE_FRAMES = 16;

  struct CachedImage {
    Image image = {};
    usize last_use_frame = 0;
  };

  ankerl::unordered_dense::map<ImageCacheKey, std::vector<CachedImage>, ImageCacheKeyHash> cached = {};
  ankerl::unordered_dense::map<ImageCacheKey, usize, ImageCacheKeyHash> uses = {};

  template <typename F>
  auto acquire(this FrameImageCache& self, const ImageInfo& info, usize frame, F&& create)
    -> std::expected<Image, VkResult> {
    const auto key = ImageCacheKey::from(info);
    auto& at = self.uses[key];
    auto& images = self.cached[key];
    if (at < images.size()) {
      images[at].last_use_frame = frame;
      at += 1;
      return images[at - 1].image;
    }

    auto image = create();
    if (!image) {
      return image;
    }

    images.push_back({.image = *image, .last_use_frame = frame});
    at += 1;
    return image;
  }

  // only once the frame slot's work has finished on the GPU
  auto retire(this FrameImageCache& self, usize frame) -> std::vector<Image>;
  auto drain(this FrameImageCache& self) -> std::vector<Image>;
};

struct FrameAllocator {
  static constexpr u64 FRAME_BLOCK_SIZE = 4_u64 * 1024 * 1024;
  // folded into every frame buffer's usage, so one segment serves most of the requests
  static constexpr VkBufferUsageFlags COMMON_USAGE = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                     VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                                     VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  static constexpr usize IDLE_FRAMES = 16;

  struct Segment {
    Buffer backing = {};
    u64 used = 0;
    usize last_use_frame = 0;
  };

  PersistentAllocator* upstream = nullptr;
  usize issued_frame = 0;
  ankerl::unordered_dense::map<CommandPoolKey, ThreadCommandPool, CommandPoolKeyHash> command_pools = {};
  std::vector<VkSemaphore> semaphores = {};
  std::vector<VkImageView> image_views = {};
  std::vector<VkSampler> samplers = {};
  std::vector<QueryPool> query_pools = {};
  ankerl::unordered_dense::map<PersistentAllocator::PoolKey, std::vector<Segment>, PersistentAllocator::PoolKeyHash>
    segments = {};
  u64 next_buffer_id = 1_u64 << 63;
  FrameImageCache image_cache = {};
  std::vector<SubmitTicket> timeline_waits = {};

  explicit FrameAllocator(PersistentAllocator& persistent);
  FrameAllocator(const FrameAllocator&) = delete;
  FrameAllocator(FrameAllocator&&) = delete;
  ~FrameAllocator();

  auto operator=(const FrameAllocator&) -> FrameAllocator& = delete;
  auto operator=(FrameAllocator&&) -> FrameAllocator& = delete;

  auto add_timeline_wait(this FrameAllocator& self, SubmitTicket ticket) -> void;
  auto wait_idle(this FrameAllocator& self) -> std::expected<void, VkResult>;
  // waits for this slot's work, then hands back everything it allocated last time around
  auto recycle(this FrameAllocator& self, usize frame) -> std::expected<void, VkResult>;

  auto allocate_binary_semaphore(this FrameAllocator& self) -> std::expected<VkSemaphore, VkResult>;
  auto allocate_command_buffer(this FrameAllocator& self, u32 queue_family) -> std::expected<CommandBuffer, VkResult>;
  auto allocate_image_view(
    this FrameAllocator& self,
    VkImage image,
    Format format,
    ImageViewType view_type,
    const VkImageSubresourceRange& range
  ) -> std::expected<VkImageView, VkResult>;
  auto allocate_sampler(this FrameAllocator& self, const SamplerInfo& info) -> std::expected<VkSampler, VkResult>;
  auto allocate_query_pool(this FrameAllocator& self, VkQueryType type, u32 count)
    -> std::expected<QueryPool, VkResult>;
  auto allocate_buffer(this FrameAllocator& self, const BufferInfo& info) -> std::expected<Buffer, VkResult>;
  auto allocate_image(this FrameAllocator& self, const ImageInfo& info) -> std::expected<Image, VkResult>;
};

struct SuperFrameAllocator {
  std::vector<std::unique_ptr<FrameAllocator>> frames = {};
  usize frame_counter = 0;

  SuperFrameAllocator(PersistentAllocator& persistent, usize frames_in_flight);

  auto current(this SuperFrameAllocator& self) -> FrameAllocator&;
  auto next_frame(this SuperFrameAllocator& self) -> std::expected<FrameAllocator*, VkResult>;
};
} // namespace ox::vir
