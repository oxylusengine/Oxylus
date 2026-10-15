#include "Render/Graph/Allocator.hpp"

#include <algorithm>
#include <bit>
#include <numeric>
#include <utility>
#include <vk_mem_alloc.h>

#include "Memory/Stack.hpp"
#include "Render/Graph/Types.hpp"
#include "Utils/Log.hpp"

namespace ox::vir {
auto CommandPoolKeyHash::operator()(const CommandPoolKey& key) const noexcept -> u64 {
  return ankerl::unordered_dense::detail::wyhash::mix(std::hash<std::thread::id>{}(key.thread), key.family);
}

static auto hash_bytes(const void* data, usize size) -> u64 {
  return ankerl::unordered_dense::detail::wyhash::hash(data, size);
}

auto align_up_checked(u64 value, u64 alignment) -> option<u64> {
  if (alignment == 0) {
    return nullopt;
  }

  const auto padding = (alignment - value % alignment) % alignment;
  if (value + padding < value) {
    return nullopt;
  }

  return value + padding;
}

auto allocation_alignment(const BufferInfo& info, const VkPhysicalDeviceLimits& limits) -> option<u64> {
  if (info.alignment == 0) {
    return nullopt;
  }

  const auto minimum = std::max(
    {4_u64,
     limits.minUniformBufferOffsetAlignment,
     limits.minStorageBufferOffsetAlignment,
     limits.minTexelBufferOffsetAlignment}
  );
  return std::lcm(minimum, info.alignment);
}

static auto mapped_buffer_range(const Buffer& buffer, u64 offset, u64 size) -> option<std::pair<u64, u64>> {
  if (buffer.mapped == nullptr || offset > buffer.size) {
    return nullopt;
  }
  const auto bytes = size == VK_WHOLE_SIZE ? buffer.size - offset : size;
  if (bytes > buffer.size - offset || offset > ~0_u64 - buffer.offset) {
    return nullopt;
  }
  return std::pair{buffer.offset + offset, bytes};
}

auto PersistentAllocator::PoolKeyHash::operator()(const PoolKey& key) const noexcept -> u64 {
  const u64 packed = (static_cast<u64>(key.location) << 32) | key.usage;
  return ankerl::unordered_dense::detail::wyhash::hash(packed);
}

static auto vma_create_info(MemoryLocation location) -> VmaAllocationCreateInfo {
  auto create_info = VmaAllocationCreateInfo{};
  create_info.usage = VMA_MEMORY_USAGE_AUTO;
  switch (location) {
    case MemoryLocation::CpuToGpu:
      create_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
      break;
    case MemoryLocation::GpuToCpu:
      create_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
      break;
    case MemoryLocation::Unknown:
    case MemoryLocation::GpuOnly: create_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE; break;
  }

  return create_info;
}

PersistentAllocator::PersistentAllocator(Context& ctx) : context(&ctx) {
  for (const auto& queue : ctx.queues) {
    queue_families.push_back(queue->family_index);
  }

  std::ranges::sort(queue_families);
  const auto [first, last] = std::ranges::unique(queue_families);
  queue_families.erase(first, last);
}

PersistentAllocator::~PersistentAllocator() {
  const auto& fn = context->fn;
  for (const auto& [handle, allocation] : raw_buffers) {
    vmaDestroyBuffer(context->memory, handle, allocation);
  }

  for (const auto& [handle, allocation] : images) {
    vmaDestroyImage(context->memory, handle, allocation);
  }

  for (const auto& [key, pool] : command_pools) {
    fn.vkDestroyCommandPool(context->device, pool.handle, nullptr);
  }
}

auto PersistentAllocator::allocate_command_pool(this PersistentAllocator& self, u32 queue_family)
  -> std::expected<VkCommandPool, VkResult> {
  const auto create_info = VkCommandPoolCreateInfo{
    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
    .pNext = nullptr,
    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
    .queueFamilyIndex = queue_family,
  };

  VkCommandPool pool = VK_NULL_HANDLE;
  if (
    const auto result = self.context->fn.vkCreateCommandPool(self.context->device, &create_info, nullptr, &pool);
    result != VK_SUCCESS
  ) {
    return std::unexpected(result);
  }

  return pool;
}

auto PersistentAllocator::reset_command_pool(this PersistentAllocator& self, VkCommandPool pool) -> VkResult {
  return self.context->fn.vkResetCommandPool(self.context->device, pool, 0);
}

auto PersistentAllocator::destroy_command_pool(this PersistentAllocator& self, VkCommandPool pool) -> void {
  self.context->fn.vkDestroyCommandPool(self.context->device, pool, nullptr);
}

auto PersistentAllocator::free_command_buffers(this PersistentAllocator& self) -> void {
  auto lock = std::unique_lock(self.mutex);
  for (auto& [key, pool] : self.command_pools) {
    if (!pool.buffers.empty()) {
      self.context->fn.vkFreeCommandBuffers(
        self.context->device,
        pool.handle,
        static_cast<u32>(pool.buffers.size()),
        pool.buffers.data()
      );
      pool.buffers.clear();
    }
  }
}

auto PersistentAllocator::allocate_binary_semaphore(this PersistentAllocator& self)
  -> std::expected<VkSemaphore, VkResult> {
  const auto create_info = VkSemaphoreCreateInfo{
    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    .pNext = nullptr,
    .flags = 0,
  };

  VkSemaphore semaphore = VK_NULL_HANDLE;
  if (
    const auto result = self.context->fn.vkCreateSemaphore(self.context->device, &create_info, nullptr, &semaphore);
    result != VK_SUCCESS
  ) {
    return std::unexpected(result);
  }

  return semaphore;
}

auto PersistentAllocator::deallocate_semaphore(this PersistentAllocator& self, VkSemaphore semaphore) -> void {
  self.context->fn.vkDestroySemaphore(self.context->device, semaphore, nullptr);
}

auto PersistentAllocator::allocate_command_buffer(this PersistentAllocator& self, u32 queue_family)
  -> std::expected<CommandBuffer, VkResult> {
  auto lock = std::unique_lock(self.mutex);
  auto& pool = self.command_pools[CommandPoolKey{.thread = std::this_thread::get_id(), .family = queue_family}];
  if (pool.handle == VK_NULL_HANDLE) {
    auto created = self.allocate_command_pool(queue_family);
    if (!created) {
      return std::unexpected(created.error());
    }
    pool.handle = *created;
  }
  const auto info = VkCommandBufferAllocateInfo{
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .pNext = nullptr,
    .commandPool = pool.handle,
    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    .commandBufferCount = 1
  };
  VkCommandBuffer handle = VK_NULL_HANDLE;
  if (
    const auto result = self.context->fn.vkAllocateCommandBuffers(self.context->device, &info, &handle);
    result != VK_SUCCESS
  ) {
    return std::unexpected(result);
  }
  pool.buffers.push_back(handle);
  return CommandBuffer{.fn = &self.context->fn, .handle = handle};
}

auto PersistentAllocator::allocate_image_view(
  this PersistentAllocator& self,
  VkImage image,
  Format format,
  ImageViewType view_type,
  const VkImageSubresourceRange& range
) -> std::expected<VkImageView, VkResult> {
  const auto create_info = VkImageViewCreateInfo{
    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
    .pNext = nullptr,
    .flags = 0,
    .image = image,
    .viewType = static_cast<VkImageViewType>(view_type),
    .format = static_cast<VkFormat>(format),
    .components = {},
    .subresourceRange = range,
  };

  VkImageView view = VK_NULL_HANDLE;
  if (
    const auto result = self.context->fn.vkCreateImageView(self.context->device, &create_info, nullptr, &view);
    result != VK_SUCCESS
  ) {
    return std::unexpected(result);
  }

  return view;
}

auto PersistentAllocator::deallocate_image_view(this PersistentAllocator& self, VkImageView view) -> void {
  self.context->fn.vkDestroyImageView(self.context->device, view, nullptr);
}

auto PersistentAllocator::allocate_sampler(this PersistentAllocator& self, const SamplerInfo& info)
  -> std::expected<VkSampler, VkResult> {
  auto reduction_info = VkSamplerReductionModeCreateInfo{
    .sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO,
    .pNext = nullptr,
    .reductionMode = info.reduction_mode,
  };
  const auto create_info = VkSamplerCreateInfo{
    .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
    .pNext = info.reduction_mode == VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE ? nullptr : &reduction_info,
    .flags = 0,
    .magFilter = info.mag_filter,
    .minFilter = info.min_filter,
    .mipmapMode = info.mipmap_mode,
    .addressModeU = info.address_mode_u,
    .addressModeV = info.address_mode_v,
    .addressModeW = info.address_mode_w,
    .mipLodBias = info.mip_lod_bias,
    .anisotropyEnable = info.max_anisotropy > 1.0f ? VK_TRUE : VK_FALSE,
    .maxAnisotropy = info.max_anisotropy,
    .compareEnable = info.compare_op != VK_COMPARE_OP_NEVER ? VK_TRUE : VK_FALSE,
    .compareOp = info.compare_op,
    .minLod = info.min_lod,
    .maxLod = info.max_lod,
    .borderColor = info.border_color,
    .unnormalizedCoordinates = VK_FALSE,
  };

  VkSampler sampler = VK_NULL_HANDLE;
  if (
    const auto result = self.context->fn.vkCreateSampler(self.context->device, &create_info, nullptr, &sampler);
    result != VK_SUCCESS
  ) {
    return std::unexpected(result);
  }

  return sampler;
}

auto PersistentAllocator::deallocate_sampler(this PersistentAllocator& self, VkSampler sampler) -> void {
  self.context->fn.vkDestroySampler(self.context->device, sampler, nullptr);
}

auto PersistentAllocator::allocate_query_pool(this PersistentAllocator& self, VkQueryType type, u32 count)
  -> std::expected<QueryPool, VkResult> {
  if (count == 0) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }
  const auto info = VkQueryPoolCreateInfo{
    .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
    .pNext = nullptr,
    .flags = 0,
    .queryType = type,
    .queryCount = count,
    .pipelineStatistics = 0
  };
  VkQueryPool handle = VK_NULL_HANDLE;
  if (
    const auto result = self.context->fn.vkCreateQueryPool(self.context->device, &info, nullptr, &handle);
    result != VK_SUCCESS
  ) {
    return std::unexpected(result);
  }
  return QueryPool{.handle = handle, .type = type, .count = count};
}

auto PersistentAllocator::deallocate_query_pool(this PersistentAllocator& self, const QueryPool& pool) -> void {
  self.context->fn.vkDestroyQueryPool(self.context->device, pool.handle, nullptr);
}

auto PersistentAllocator::allocate_raw_buffer(this PersistentAllocator& self, const BufferInfo& info)
  -> std::expected<Buffer, VkResult> {
  auto create_info = VkBufferCreateInfo{
    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
    .pNext = nullptr,
    .flags = 0,
    .size = info.size,
    .usage = info.usage,
    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    .queueFamilyIndexCount = 0,
    .pQueueFamilyIndices = nullptr,
  };
  if (self.queue_families.size() > 1) {
    create_info.sharingMode = VK_SHARING_MODE_CONCURRENT;
    create_info.queueFamilyIndexCount = static_cast<u32>(self.queue_families.size());
    create_info.pQueueFamilyIndices = self.queue_families.data();
  }

  const auto allocation_info = vma_create_info(info.location);
  VkBuffer handle = VK_NULL_HANDLE;
  VmaAllocation allocation = nullptr;
  auto allocated = VmaAllocationInfo{};
  const auto result = vmaCreateBufferWithAlignment(
    self.context->memory,
    &create_info,
    &allocation_info,
    std::max(info.alignment, 1_u64),
    &handle,
    &allocation,
    &allocated
  );
  if (result != VK_SUCCESS) {
    OX_LOG_ERROR("failed to allocate a {} byte buffer ({})", info.size, std::to_underlying(result));
    return std::unexpected(result);
  }

  auto device_address = VkDeviceAddress{0};
  if (info.usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
    const auto address_info = VkBufferDeviceAddressInfo{
      .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
      .pNext = nullptr,
      .buffer = handle,
    };
    device_address = self.context->fn.vkGetBufferDeviceAddress(self.context->device, &address_info);
  }

  if (!info.name.empty()) {
    self.context->set_name(VK_OBJECT_TYPE_BUFFER, handle, info.name);
    vmaSetAllocationName(self.context->memory, allocation, info.name.c_str());
  }

  self.raw_buffers.emplace(handle, allocation);
  return Buffer{
    .handle = handle,
    .offset = 0,
    .size = info.size,
    .device_address = device_address,
    .mapped = static_cast<u8*>(allocated.pMappedData),
    .allocation_id = 0,
  };
}

auto PersistentAllocator::deallocate_raw_buffer(this PersistentAllocator& self, const Buffer& buffer) -> void {
  const auto it = self.raw_buffers.find(buffer.handle);
  if (it == self.raw_buffers.end()) {
    OX_LOG_ERROR("buffer was not allocated by this allocator");
    return;
  }

  vmaDestroyBuffer(self.context->memory, it->first, it->second);
  self.raw_buffers.erase(it);
}

auto PersistentAllocator::allocate_buffer(this PersistentAllocator& self, const BufferInfo& info)
  -> std::expected<Buffer, VkResult> {
  if (info.size == 0) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }

  const auto alignment = allocation_alignment(info, self.context->limits);
  if (!alignment.has_value()) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }

  auto aligned_info = info;
  aligned_info.alignment = *alignment;
  auto lock = std::unique_lock(self.mutex);
  auto raw = self.allocate_raw_buffer(aligned_info);
  if (!raw) {
    return raw;
  }

  const auto id = self.next_allocation_id++;
  auto buffer = *raw->slice(0, info.size, id);
  self.buffer_allocations.emplace(id, buffer);
  return buffer;
}

auto PersistentAllocator::deallocate_buffer(this PersistentAllocator& self, const Buffer& buffer) -> void {
  auto lock = std::unique_lock(self.mutex);
  const auto it = self.buffer_allocations.find(buffer.allocation_id);
  if (it == self.buffer_allocations.end() || it->second != buffer) {
    OX_LOG_ERROR("buffer was not allocated by this allocator or was already freed");
    return;
  }

  const auto backing = it->second;
  self.buffer_allocations.erase(it);
  self.deallocate_raw_buffer(backing);
}

auto PersistentAllocator::flush_buffer(this PersistentAllocator& self, const Buffer& buffer, u64 offset, u64 size)
  -> std::expected<void, VkResult> {
  const auto range = mapped_buffer_range(buffer, offset, size);
  if (!range.has_value()) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }
  auto lock = std::unique_lock(self.mutex);
  const auto allocation = self.raw_buffers.find(buffer.handle);
  if (allocation == self.raw_buffers.end()) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }
  if (range->second != 0) {
    const auto result = vmaFlushAllocation(self.context->memory, allocation->second, range->first, range->second);
    if (result != VK_SUCCESS) {
      return std::unexpected(result);
    }
  }
  return {};
}

auto PersistentAllocator::invalidate_buffer(this PersistentAllocator& self, const Buffer& buffer, u64 offset, u64 size)
  -> std::expected<void, VkResult> {
  const auto range = mapped_buffer_range(buffer, offset, size);
  if (!range.has_value()) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }
  auto lock = std::unique_lock(self.mutex);
  const auto allocation = self.raw_buffers.find(buffer.handle);
  if (allocation == self.raw_buffers.end()) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }
  if (range->second != 0) {
    const auto result = vmaInvalidateAllocation(self.context->memory, allocation->second, range->first, range->second);
    if (result != VK_SUCCESS) {
      return std::unexpected(result);
    }
  }
  return {};
}

auto PersistentAllocator::allocate_image(this PersistentAllocator& self, const ImageInfo& info)
  -> std::expected<Image, VkResult> {
  auto stack = memory::ScopedStack{};
  auto formats = stack.alloc<VkFormat>(info.view_formats.size() + 1);
  usize format_count = 0;
  if (!info.view_formats.empty()) {
    formats[format_count++] = static_cast<VkFormat>(info.format);
    for (const auto format : info.view_formats) {
      if (format != info.format) {
        formats[format_count++] = static_cast<VkFormat>(format);
      }
    }
  }
  auto format_list = VkImageFormatListCreateInfo{
    .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
    .pNext = nullptr,
    .viewFormatCount = static_cast<u32>(format_count),
    .pViewFormats = formats.data(),
  };
  auto flags = info.flags;
  if (!info.view_formats.empty()) {
    flags |= ImageCreateFlags::MutableFormat;
  }

  const auto create_info = VkImageCreateInfo{
    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
    .pNext = info.view_formats.empty() ? nullptr : &format_list,
    .flags = static_cast<VkImageCreateFlags>(flags),
    .imageType = static_cast<VkImageType>(info.image_type),
    .format = static_cast<VkFormat>(info.format),
    .extent = std::bit_cast<VkExtent3D>(info.extent),
    .mipLevels = info.mip_levels,
    .arrayLayers = info.array_layers,
    .samples = static_cast<VkSampleCountFlagBits>(info.samples),
    .tiling = VK_IMAGE_TILING_OPTIMAL,
    .usage = static_cast<VkImageUsageFlags>(info.usage),
    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    .queueFamilyIndexCount = 0,
    .pQueueFamilyIndices = nullptr,
    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };

  const auto allocation_info = vma_create_info(info.location);
  VkImage handle = VK_NULL_HANDLE;
  VmaAllocation allocation = nullptr;
  if (
    const auto
      result = vmaCreateImage(self.context->memory, &create_info, &allocation_info, &handle, &allocation, nullptr);
    result != VK_SUCCESS
  ) {
    OX_LOG_ERROR("failed to allocate image {} ({})", info.name, std::to_underlying(result));
    return std::unexpected(result);
  }

  if (!info.name.empty()) {
    self.context->set_name(VK_OBJECT_TYPE_IMAGE, handle, info.name);
    vmaSetAllocationName(self.context->memory, allocation, info.name.c_str());
  }

  auto lock = std::unique_lock(self.mutex);
  self.images.emplace(handle, allocation);
  auto image = Image::from_info(handle, info);
  image.usage = info.usage;
  return image;
}

auto PersistentAllocator::deallocate_image(this PersistentAllocator& self, const Image& image) -> void {
  auto lock = std::unique_lock(self.mutex);
  const auto it = self.images.find(image.handle);
  if (it == self.images.end()) {
    OX_LOG_ERROR("image was not allocated by this allocator");
    return;
  }

  vmaDestroyImage(self.context->memory, it->first, it->second);
  self.images.erase(it);
}

auto ImageCacheKey::from(const ImageInfo& info) -> ImageCacheKey {
  return {
    .extent = info.extent,
    .format = info.format,
    .usage = info.usage,
    .image_type = info.image_type,
    .flags = info.flags | (info.view_formats.empty() ? ImageCreateFlags::None : ImageCreateFlags::MutableFormat),
    .mip_levels = info.mip_levels,
    .array_layers = info.array_layers,
    .samples = info.samples,
    .location = info.location,
    .view_formats = info.view_formats,
  };
}

auto ImageCacheKey::operator==(const ImageCacheKey& other) const -> bool {
  return extent.width == other.extent.width && extent.height == other.extent.height &&
         extent.depth == other.extent.depth && format == other.format && usage == other.usage &&
         image_type == other.image_type && flags == other.flags && mip_levels == other.mip_levels &&
         array_layers == other.array_layers && samples == other.samples && location == other.location &&
         view_formats == other.view_formats;
}

auto ImageCacheKeyHash::operator()(const ImageCacheKey& key) const noexcept -> u64 {
  const u32 words[] = {
    key.extent.width,
    key.extent.height,
    key.extent.depth,
    static_cast<u32>(key.format),
    std::to_underlying(key.usage),
    static_cast<u32>(key.image_type),
    std::to_underlying(key.flags),
    key.mip_levels,
    key.array_layers,
    static_cast<u32>(key.samples),
    static_cast<u32>(key.location),
  };
  return ankerl::unordered_dense::detail::wyhash::mix(
    hash_bytes(words, sizeof(words)),
    hash_bytes(key.view_formats.data(), key.view_formats.size() * sizeof(Format))
  );
}

auto FrameImageCache::retire(this FrameImageCache& self, usize frame) -> std::vector<Image> {
  self.uses.clear();
  auto idle = std::vector<Image>{};
  for (auto& [key, images] : self.cached) {
    std::erase_if(images, [&](const CachedImage& entry) {
      if (frame > entry.last_use_frame && frame - entry.last_use_frame > IDLE_FRAMES) {
        idle.push_back(entry.image);
        return true;
      }

      return false;
    });
  }

  std::erase_if(self.cached, [](const auto& entry) { return entry.second.empty(); });
  return idle;
}

auto FrameImageCache::drain(this FrameImageCache& self) -> std::vector<Image> {
  self.uses.clear();
  auto drained = std::vector<Image>{};
  for (const auto& [key, images] : self.cached) {
    for (const auto& entry : images) {
      drained.push_back(entry.image);
    }
  }

  self.cached.clear();
  return drained;
}

FrameAllocator::FrameAllocator(PersistentAllocator& persistent) : upstream(&persistent) {}

FrameAllocator::~FrameAllocator() {
  if (auto waited = wait_idle(); !waited) {
    OX_LOG_ERROR("failed to wait for a frame before tearing it down");
  }

  auto* context = upstream->context;
  for (const auto& [key, pool] : command_pools) {
    upstream->destroy_command_pool(pool.handle);
  }

  for (auto semaphore : semaphores) {
    upstream->deallocate_semaphore(semaphore);
  }

  for (auto view : image_views) {
    upstream->deallocate_image_view(view);
  }

  for (auto sampler : samplers) {
    upstream->deallocate_sampler(sampler);
  }

  for (const auto& queries : query_pools) {
    upstream->deallocate_query_pool(queries);
  }

  for (const auto& image : image_cache.drain()) {
    upstream->deallocate_image(image);
  }

  for (const auto& [key, key_segments] : segments) {
    for (const auto& segment : key_segments) {
      upstream->deallocate_buffer(segment.backing);
    }
  }
}

auto FrameAllocator::add_timeline_wait(this FrameAllocator& self, SubmitTicket ticket) -> void {
  if (ticket.value == 0) {
    return;
  }
  auto found = std::ranges::find(self.timeline_waits, ticket.queue, &SubmitTicket::queue);
  if (found != self.timeline_waits.end()) {
    found->value = std::max(found->value, ticket.value);
  } else {
    self.timeline_waits.push_back(ticket);
  }
}

auto FrameAllocator::wait_idle(this FrameAllocator& self) -> std::expected<void, VkResult> {
  for (const auto& ticket : self.timeline_waits) {
    if (auto waited = self.upstream->context->wait(ticket); !waited) {
      return waited;
    }
  }
  return {};
}

auto FrameAllocator::recycle(this FrameAllocator& self, usize frame) -> std::expected<void, VkResult> {
  ZoneScoped;

  if (auto waited = self.wait_idle(); !waited) {
    return waited;
  }

  self.timeline_waits.clear();

  auto* context = self.upstream->context;
  for (auto& [key, pool] : self.command_pools) {
    if (!pool.buffers.empty()) {
      context->fn
        .vkFreeCommandBuffers(context->device, pool.handle, static_cast<u32>(pool.buffers.size()), pool.buffers.data());
      pool.buffers.clear();
    }
    if (const auto result = self.upstream->reset_command_pool(pool.handle); result != VK_SUCCESS) {
      return std::unexpected(result);
    }
  }

  for (auto semaphore : self.semaphores) {
    self.upstream->deallocate_semaphore(semaphore);
  }
  self.semaphores.clear();

  for (auto view : self.image_views) {
    self.upstream->deallocate_image_view(view);
  }
  self.image_views.clear();

  for (auto sampler : self.samplers) {
    self.upstream->deallocate_sampler(sampler);
  }
  self.samplers.clear();

  for (const auto& queries : self.query_pools) {
    self.upstream->deallocate_query_pool(queries);
  }
  self.query_pools.clear();

  auto idle_buffers = std::vector<Buffer>{};
  for (auto& [key, key_segments] : self.segments) {
    std::erase_if(key_segments, [&](Segment& segment) {
      if (frame > segment.last_use_frame && frame - segment.last_use_frame > IDLE_FRAMES) {
        idle_buffers.push_back(segment.backing);
        return true;
      }

      segment.used = 0;
      return false;
    });
  }

  std::erase_if(self.segments, [](const auto& entry) { return entry.second.empty(); });
  for (const auto& buffer : idle_buffers) {
    self.upstream->deallocate_buffer(buffer);
  }

  for (const auto& image : self.image_cache.retire(frame)) {
    self.upstream->deallocate_image(image);
  }

  self.issued_frame = frame;
  return {};
}

auto FrameAllocator::allocate_binary_semaphore(this FrameAllocator& self) -> std::expected<VkSemaphore, VkResult> {
  auto semaphore = self.upstream->allocate_binary_semaphore();
  if (semaphore) {
    self.semaphores.push_back(*semaphore);
  }

  return semaphore;
}

auto FrameAllocator::allocate_command_buffer(this FrameAllocator& self, u32 queue_family)
  -> std::expected<CommandBuffer, VkResult> {
  auto* context = self.upstream->context;
  auto& pool = self.command_pools[CommandPoolKey{.thread = std::this_thread::get_id(), .family = queue_family}];
  if (pool.handle == VK_NULL_HANDLE) {
    auto created = self.upstream->allocate_command_pool(queue_family);
    if (!created) {
      return std::unexpected(created.error());
    }
    pool.handle = *created;
  }
  const auto info = VkCommandBufferAllocateInfo{
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .pNext = nullptr,
    .commandPool = pool.handle,
    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    .commandBufferCount = 1
  };
  VkCommandBuffer handle = VK_NULL_HANDLE;
  if (const auto result = context->fn.vkAllocateCommandBuffers(context->device, &info, &handle); result != VK_SUCCESS) {
    return std::unexpected(result);
  }
  pool.buffers.push_back(handle);
  return CommandBuffer{.fn = &context->fn, .handle = handle};
}

auto FrameAllocator::allocate_query_pool(this FrameAllocator& self, VkQueryType type, u32 count)
  -> std::expected<QueryPool, VkResult> {
  auto queries = self.upstream->allocate_query_pool(type, count);
  if (queries) {
    self.query_pools.push_back(*queries);
  }
  return queries;
}

auto FrameAllocator::allocate_image_view(
  this FrameAllocator& self, VkImage image, Format format, ImageViewType view_type, const VkImageSubresourceRange& range
) -> std::expected<VkImageView, VkResult> {
  auto view = self.upstream->allocate_image_view(image, format, view_type, range);
  if (view) {
    self.image_views.push_back(*view);
  }

  return view;
}

auto FrameAllocator::allocate_sampler(this FrameAllocator& self, const SamplerInfo& info)
  -> std::expected<VkSampler, VkResult> {
  auto sampler = self.upstream->allocate_sampler(info);
  if (sampler) {
    self.samplers.push_back(*sampler);
  }

  return sampler;
}

auto FrameAllocator::allocate_buffer(this FrameAllocator& self, const BufferInfo& info)
  -> std::expected<Buffer, VkResult> {
  if (info.size == 0) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }

  const auto alignment = allocation_alignment(info, self.upstream->context->limits);
  if (!alignment.has_value()) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }

  const auto id = self.next_buffer_id++;
  const auto usage = info.usage | COMMON_USAGE;
  const auto key = PersistentAllocator::PoolKey{.location = info.location, .usage = usage};
  auto& key_segments = self.segments[key];
  for (auto& segment : key_segments) {
    const auto offset = align_up_checked(segment.used, *alignment);
    if (!offset.has_value() || *offset + info.size > segment.backing.size) {
      continue;
    }

    segment.used = *offset + info.size;
    segment.last_use_frame = self.issued_frame;
    return *segment.backing.slice(*offset, info.size, id);
  }

  const auto size = align_up_checked(std::max(info.size, FRAME_BLOCK_SIZE), FRAME_BLOCK_SIZE);
  if (!size.has_value()) {
    return std::unexpected(VK_ERROR_OUT_OF_DEVICE_MEMORY);
  }

  auto backing = self.upstream->allocate_buffer({
    .size = *size,
    .usage = usage,
    .location = info.location,
    .alignment = 1,
    .name = {},
  });
  if (!backing) {
    return backing;
  }

  key_segments.push_back({.backing = *backing, .used = info.size, .last_use_frame = self.issued_frame});
  return *backing->slice(0, info.size, id);
}

auto FrameAllocator::allocate_image(this FrameAllocator& self, const ImageInfo& info)
  -> std::expected<Image, VkResult> {
  return self.image_cache.acquire(info, self.issued_frame, [&] { return self.upstream->allocate_image(info); });
}

SuperFrameAllocator::SuperFrameAllocator(PersistentAllocator& persistent, usize frames_in_flight) {
  OX_ASSERT(frames_in_flight > 0, "a super-frame allocator needs at least one frame");
  for (usize i = 0; i < frames_in_flight; i++) {
    frames.push_back(std::make_unique<FrameAllocator>(persistent));
  }
}

auto SuperFrameAllocator::current(this SuperFrameAllocator& self) -> FrameAllocator& {
  return *self.frames[self.frame_counter % self.frames.size()];
}

auto SuperFrameAllocator::next_frame(this SuperFrameAllocator& self) -> std::expected<FrameAllocator*, VkResult> {
  self.frame_counter += 1;
  auto& frame = self.current();
  if (auto recycled = frame.recycle(self.frame_counter); !recycled) {
    return std::unexpected(recycled.error());
  }

  return &frame;
}
} // namespace ox::vir
