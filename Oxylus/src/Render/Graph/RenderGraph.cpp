#include "Render/Graph/RenderGraph.hpp"

#include <algorithm>
#include <bit>

#include "Memory/Stack.hpp"
#include "Render/Graph/Transfer.hpp"
#include "Render/Graph/Types.hpp"
#include "Utils/Log.hpp"

namespace ox::vir {
#define VIR_TRY(expression)                                                                                            \
  if (auto vir_try_result = (expression); !vir_try_result) {                                                           \
    return std::unexpected(vir_try_result.error());                                                                    \
  }

#define VIR_CHECK(expression)                                                                                          \
  if (const auto vir_check_result = (expression); vir_check_result != VK_SUCCESS) {                                    \
    return std::unexpected(vir_check_result);                                                                          \
  }

static auto wait_for_query_writes(Context& context, CommandQueue& queue, std::unique_lock<std::mutex>& queue_lock)
  -> std::expected<void, VkResult> {
  ZoneScoped;
  while (queue.pending_query_retirement != 0) {
    const auto ticket = SubmitTicket{context.queue_id(queue.domain), queue.pending_query_retirement};
    queue_lock.unlock();
    const auto waited = context.wait(ticket);
    queue_lock.lock();
    if (!waited) {
      return waited;
    }
    if (queue.pending_query_retirement == ticket.value) {
      queue.pending_query_retirement = 0;
    }
  }
  return {};
}

auto DeclaredPipeline::bind_point(this const DeclaredPipeline& self) -> VkPipelineBindPoint {
  switch (self.kind) {
    case PipelineKind::Graphics  : return VK_PIPELINE_BIND_POINT_GRAPHICS;
    case PipelineKind::Compute   : return VK_PIPELINE_BIND_POINT_COMPUTE;
    case PipelineKind::RayTracing: return VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR;
  }

  return VK_PIPELINE_BIND_POINT_GRAPHICS;
}

auto ResolvedDescriptor::supports_type(this const ResolvedDescriptor& self, VkDescriptorType type) -> bool {
  const auto descriptor = ir::Descriptor{.kind = static_cast<ir::Descriptor::Kind>(self.kind)};
  return descriptor.supports_type(type);
}

auto ResolvedDescriptor::validate(this const ResolvedDescriptor& self) -> std::expected<void, VkResult> {
  auto is_invalid = false;
  switch (self.kind) {
    case Kind::Sampler: is_invalid = self.sampler == VK_NULL_HANDLE; break;
    case Kind::Image  : is_invalid = self.image_view == VK_NULL_HANDLE; break;
    case Kind::CombinedImageSampler:
      is_invalid = self.image_view == VK_NULL_HANDLE || self.sampler == VK_NULL_HANDLE;
      break;
    case Kind::TexelBuffer: is_invalid = self.buffer == VK_NULL_HANDLE || self.view == VK_NULL_HANDLE; break;
    case Kind::Buffer:
      is_invalid = self.buffer == VK_NULL_HANDLE || self.range == 0 || self.offset >= self.buffer_size ||
                   (self.range != VK_WHOLE_SIZE && self.offset + self.range > self.buffer_size);
      break;
    case Kind::AccelerationStructure:
      is_invalid = self.buffer == VK_NULL_HANDLE || self.acceleration_structure == VK_NULL_HANDLE;
      break;
  }

  if (is_invalid) {
    OX_LOG_ERROR(
      "descriptor at set {} binding {} resolves to an invalid or out of bounds resource",
      self.set,
      self.binding
    );
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }

  return {};
}

auto ResolvedDescriptor::operator==(const ResolvedDescriptor& other) const -> bool {
  return set == other.set && binding == other.binding && descriptor_type == other.descriptor_type &&
         kind == other.kind && sampler == other.sampler && image_view == other.image_view &&
         image_layout == other.image_layout && buffer == other.buffer && view == other.view &&
         buffer_size == other.buffer_size && base_offset == other.base_offset && offset == other.offset &&
         range == other.range && acceleration_structure == other.acceleration_structure;
}

struct DescriptorSetKey {
  PipelineID pipeline = PipelineID::Invalid;
  u32 set = 0;
  std::vector<ResolvedDescriptor> descriptors = {};

  auto operator==(const DescriptorSetKey&) const -> bool = default;
};

struct DescriptorSetKeyHash {
  using is_avalanching = void;
  auto operator()(const DescriptorSetKey& key) const noexcept -> u64 {
    auto hash = ankerl::unordered_dense::detail::wyhash::hash((u64(key.pipeline) << 32) | key.set);
    for (const auto& descriptor : key.descriptors) {
      const u64 words[] = {
        (u64(descriptor.set) << 32) | descriptor.binding,
        reinterpret_cast<u64>(descriptor.sampler),
        reinterpret_cast<u64>(descriptor.image_view),
        reinterpret_cast<u64>(descriptor.buffer),
        descriptor.base_offset + descriptor.offset,
        descriptor.range,
        reinterpret_cast<u64>(descriptor.acceleration_structure),
        static_cast<u64>(descriptor.image_layout),
      };
      hash = ankerl::unordered_dense::detail::wyhash::mix(
        hash,
        ankerl::unordered_dense::detail::wyhash::hash(words, sizeof(words))
      );
    }

    return hash;
  }
};

struct SemaphoreSubmit {
  VkSemaphore semaphore = VK_NULL_HANDLE;
  u64 value = 0;
  Access access = Access::MemoryRW;
};

struct Submit {
  DomainFlag domain = DomainFlag::Graphics;
  std::vector<SemaphoreSubmit> waits = {};
  std::vector<VkCommandBuffer> command_buffers = {};
  std::vector<SemaphoreSubmit> signals = {};
  bool has_query_writes = false;
};

struct PresentInfo {
  u32 image_index = 0;
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  VkSemaphore semaphore = VK_NULL_HANDLE;
};

static auto label_color(std::string_view name, f32 (&color)[4]) -> void {
  const auto hash = static_cast<u32>(ankerl::unordered_dense::detail::wyhash::hash(name.data(), name.size()));
  color[0] = static_cast<f32>(hash & 255) / 255.0f;
  color[1] = static_cast<f32>((hash >> 8) & 255) / 255.0f;
  color[2] = static_cast<f32>((hash >> 16) & 255) / 255.0f;
  color[3] = 1.0f;
}

static auto view_type_for(const ImageAttachment& root, const VkImageSubresourceRange& range, ImageViewType requested)
  -> ImageViewType {
  if (requested != ImageViewType::Auto) {
    return requested;
  }

  switch (root.view_type) {
    case ImageViewType::e3D     : return ImageViewType::e3D;
    case ImageViewType::e1D     :
    case ImageViewType::e1DArray: return range.layerCount > 1 ? ImageViewType::e1DArray : ImageViewType::e1D;
    case ImageViewType::Cube    :
    case ImageViewType::CubeArray:
      if (range.layerCount % 6 == 0 && range.layerCount > 0) {
        return range.layerCount > 6 ? ImageViewType::CubeArray : ImageViewType::Cube;
      }
      return range.layerCount > 1 ? ImageViewType::e2DArray : ImageViewType::e2D;
    default: return range.layerCount > 1 ? ImageViewType::e2DArray : ImageViewType::e2D;
  }
}

struct Execution {
  RenderGraph* graph = nullptr;
  Context* context = nullptr;
  const Program* program = nullptr;
  FrameAllocator* allocator = nullptr;
  std::vector<Value> values = {};

  VkPipeline bound_pipeline = VK_NULL_HANDLE;
  std::vector<ResolvedDescriptor> active_descriptors = {};
  std::vector<std::pair<ImageAttachment, Access>> callback_images = {};
  option<PipelineID> bound_descriptors = nullopt;
  VkRect2D render_area = {};
  std::vector<ResolvedViewport> recorded_viewports = {};
  std::vector<VkRect2D> recorded_scissors = {};
  option<std::pair<VkPipelineLayout, PushConstants>> recorded_push = nullopt;
  bool is_rendering = false;
  bool is_labeled_pass = false;
  bool has_pass_zone = false;
  void* pass_data = nullptr;

  option<CommandBuffer> batch = nullopt;
  Submit current_submit = {};
  std::vector<Submit> submits = {};
  std::vector<PresentInfo> presents = {};
  ankerl::unordered_dense::map<ValueID, PresentInfo> resource_to_swapchain = {};

  u32 pool_max_sets = 0;
  std::vector<VkDescriptorPoolSize> pool_sizes = {};
  std::vector<VkDescriptorPool> pools = {};
  ankerl::unordered_dense::map<DescriptorSetKey, VkDescriptorSet, DescriptorSetKeyHash> descriptor_cache = {};

  auto fn(this const Execution& self) -> const VulkanFunctions& { return self.context->fn; }

  auto pipeline(this const Execution& self, PipelineID id) -> DeclaredPipeline* {
    const auto index = static_cast<usize>(std::to_underlying(id));
    return index < self.graph->pipelines.size() ? self.graph->pipelines[index].get() : nullptr;
  }

  auto resolve_id(this const Execution& self, ValueID id) -> ValueID {
    const auto index = index_of(id);
    if (index < self.values.size()) {
      if (const auto* reference = std::get_if<Reference>(&self.values[index])) {
        return reference->id;
      }
    }

    return id;
  }

  auto value(this const Execution& self, ValueID id) -> const Value& {
    const auto index = index_of(self.resolve_id(id));
    OX_ASSERT(index < self.values.size(), "%%%u has no value", std::to_underlying(id));
    return self.values[index];
  }

  template <typename T>
  auto get(this const Execution& self, ValueID id) -> const T& {
    const auto* held = std::get_if<T>(&self.value(id));
    OX_ASSERT(held != nullptr, "%%%u does not hold the kind of value it is read as", std::to_underlying(id));
    return *held;
  }

  auto set_value(this Execution& self, ValueID id, Value value) -> void {
    if (auto* reference = std::get_if<Reference>(&value)) {
      reference->id = self.resolve_id(reference->id);
    }

    const auto index = index_of(id);
    if (index >= self.values.size()) {
      self.values.resize(index + 1);
    }

    self.values[index] = std::move(value);
  }

  auto forward(this Execution& self, ValueID id, ValueID target) -> void {
    self.set_value(id, Reference{.id = target});
  }

  auto elements(this const Execution& self, ValueID resource) -> std::vector<ValueID> {
    auto pending = std::vector<ValueID>{resource};
    auto result = std::vector<ValueID>{};
    while (!pending.empty()) {
      const auto value_id = pending.back();
      pending.pop_back();
      if (const auto* slice = std::get_if<Slice>(&self.value(value_id))) {
        pending.insert(pending.end(), slice->rbegin(), slice->rend());
      } else {
        result.push_back(value_id);
      }
    }

    return result;
  }

  auto reset(this Execution& self) -> std::expected<void, VkResult> {
    auto count = 0_sz;
    for (const auto& instruction : self.program->instructions) {
      count = std::max(count, index_of(instruction.id) + 1);
    }

    self.values.assign(count, std::monostate{});
    for (const auto& instruction : self.program->instructions) {
      if (const auto* variable = std::get_if<ir::Variable>(&instruction.ir)) {
        if (const auto* declared = self.program->variable(variable->slot)) {
          self.set_value(instruction.id, declared->value);
        }
      }
    }

    for (const auto& [resource, slot] : self.program->bound) {
      const auto& variable = self.program->variables[slot];
      if (std::holds_alternative<std::monostate>(variable.value)) {
        OX_LOG_ERROR("nothing was bound to the slot {} for %{}", variable.name, std::to_underlying(resource));
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
      }

      self.set_value(resource, variable.value);
    }

    return {};
  }

  auto ensure_batch(this Execution& self) -> std::expected<void, VkResult> {
    if (self.batch.has_value()) {
      return {};
    }

    auto* queue = self.context->queue(self.current_submit.domain);
    if (!queue) {
      return std::unexpected(VK_ERROR_UNKNOWN);
    }

    auto command_buffer = self.allocator->allocate_command_buffer(queue->family_index);
    if (!command_buffer) {
      return std::unexpected(command_buffer.error());
    }

    VIR_CHECK(command_buffer->begin());
    self.batch = *command_buffer;
    if (self.graph->callbacks.on_begin_command_buffer) {
      self.graph->callbacks.on_begin_command_buffer(self.graph->callbacks.user, command_buffer->handle);
    }

    self.bound_pipeline = VK_NULL_HANDLE;
    self.bound_descriptors = nullopt;
    self.recorded_viewports.clear();
    self.recorded_scissors.clear();
    self.recorded_push = nullopt;
    return {};
  }

  auto command_buffer(this const Execution& self) -> VkCommandBuffer {
    OX_ASSERT(self.batch.has_value(), "nothing is being recorded");
    return self.batch->handle;
  }

  auto record_memory_barriers(this Execution& self, std::span<const Instr> entries) -> void {
    ZoneScoped;
    auto src = Access::Empty;
    auto dst = Access::Empty;
    for (const auto& instruction : entries) {
      const auto& entry = std::get<ir::MemoryBarrier>(instruction.ir);
      src |= self.get<Access>(entry.src_access);
      dst |= self.get<Access>(entry.dst_access);
    }
    const auto barrier = CommandBuffer::memory_barrier_info(src, dst);
    self.batch->pipeline_barrier(std::span(&barrier, 1), {});
  }

  auto record_image_barriers(this Execution& self, std::span<const Instr> entries) -> void {
    ZoneScoped;
    memory::ScopedStack stack;
    const auto vk_images = stack.alloc<VkImageMemoryBarrier2>(entries.size());
    auto image_count = 0_sz;
    const auto flush = [&] -> void {
      if (image_count != 0) {
        self.batch->pipeline_barrier({}, vk_images.first(image_count));
        image_count = 0;
      }
    };
    for (const auto& instruction : entries) {
      const auto& entry = std::get<ir::ImageBarrier>(instruction.ir);
      const auto& root = self.get<ImageAttachment>(entry.value);
      const auto range = entry.range.apply(root.subresource_range);
      if (range.levelCount == 0 || range.layerCount == 0) {
        continue;
      }
      const auto barrier = CommandBuffer::image_barrier_info(
        root.image.handle,
        self.get<Access>(entry.src_access),
        self.get<Access>(entry.dst_access),
        entry.old_layout,
        entry.new_layout,
        range
      );
      // overlapping transitions need separate calls, including views bound through different roots
      const auto overlaps = [&] -> bool {
        for (usize held_index = 0; held_index < image_count; held_index++) {
          const auto& held = vk_images[held_index];
          const auto& previous = held.subresourceRange;
          if (
            held.image == barrier.image && (previous.aspectMask & range.aspectMask) != 0 &&
            u64(previous.baseMipLevel) < u64(range.baseMipLevel) + range.levelCount &&
            u64(range.baseMipLevel) < u64(previous.baseMipLevel) + previous.levelCount &&
            u64(previous.baseArrayLayer) < u64(range.baseArrayLayer) + range.layerCount &&
            u64(range.baseArrayLayer) < u64(previous.baseArrayLayer) + previous.layerCount
          ) {
            return true;
          }
        }
        return false;
      }();
      if (overlaps) {
        flush();
      }
      vk_images[image_count++] = barrier;
    }
    flush();
  }

  auto flush_submit(this Execution& self, option<SemaphoreSubmit> signal) -> std::expected<void, VkResult> {
    if (signal.has_value()) {
      self.current_submit.signals.push_back(*signal);
    }

    if (self.batch.has_value()) {
      VIR_CHECK(self.batch->end());
      self.current_submit.command_buffers.push_back(self.batch->handle);
      self.batch = nullopt;
    }

    self.submits.push_back(std::move(self.current_submit));
    self.current_submit = {};
    return {};
  }

  auto begin_pass(this Execution& self, ValueID name) -> void {
    if (!is_valid(name)) {
      return;
    }

    const auto label = self.get<std::string_view>(name);
    auto& callbacks = self.graph->callbacks;
    if (callbacks.on_begin_pass) {
      self.pass_data = callbacks.on_begin_pass(callbacks.user, label, self.command_buffer());
      self.has_pass_zone = true;
    }

    if (self.context->has_debug_utils()) {
      auto terminated = memory::ScopedStack{};
      f32 color[4] = {};
      label_color(label, color);
      self.batch->begin_label(terminated.null_terminate_cstr(label), color);
      self.is_labeled_pass = true;
    }
  }

  auto end_pass(this Execution& self) -> void {
    if (std::exchange(self.is_labeled_pass, false)) {
      self.batch->end_label();
    }

    if (std::exchange(self.has_pass_zone, false)) {
      auto& callbacks = self.graph->callbacks;
      if (callbacks.on_end_pass) {
        callbacks.on_end_pass(callbacks.user, self.pass_data, self.command_buffer());
      }
      self.pass_data = nullptr;
    }
  }

  // a pass starts and ends with nothing written, so neither inherits the other's sets
  auto open_descriptors(this Execution& self) -> void {
    self.active_descriptors.clear();
    self.bound_descriptors = nullopt;
  }

  auto prepare_descriptor_pools(this Execution& self) -> void {
    auto totals = std::map<VkDescriptorType, u32>{};
    for (const auto& instruction : self.program->instructions) {
      const auto* id = pipeline_of(instruction.ir);
      if (!id || !is_valid(*id)) {
        continue;
      }

      const auto* declared = self.pipeline(*id);
      if (!declared || !declared->layout.has_value()) {
        continue;
      }

      for (const auto& set : declared->layout->sets) {
        if (set.sizes.empty() || set.is_push) {
          continue;
        }

        self.pool_max_sets += 1;
        for (const auto& [type, count] : set.sizes) {
          totals[type] += count;
        }
      }
    }

    for (const auto& [type, count] : totals) {
      self.pool_sizes.push_back({.type = type, .descriptorCount = count});
    }
  }

  auto add_pool(this Execution& self) -> std::expected<VkDescriptorPool, VkResult> {
    const auto create_info = VkDescriptorPoolCreateInfo{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .maxSets = std::max(self.pool_max_sets, 1_u32),
      .poolSizeCount = static_cast<u32>(self.pool_sizes.size()),
      .pPoolSizes = self.pool_sizes.data(),
    };

    VkDescriptorPool pool = VK_NULL_HANDLE;
    VIR_CHECK(self.fn().vkCreateDescriptorPool(self.context->device, &create_info, nullptr, &pool));
    self.pools.push_back(pool);
    return pool;
  }

  auto allocate_set(this Execution& self, VkDescriptorSetLayout layout) -> std::expected<VkDescriptorSet, VkResult> {
    const auto allocate = [&](VkDescriptorPool pool, VkDescriptorSet& set) {
      const auto alloc_info = VkDescriptorSetAllocateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = nullptr,
        .descriptorPool = pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &layout,
      };
      return self.fn().vkAllocateDescriptorSets(self.context->device, &alloc_info, &set);
    };

    if (self.pools.empty()) {
      VIR_TRY(self.add_pool());
    }

    VkDescriptorSet set = VK_NULL_HANDLE;
    const auto result = allocate(self.pools.back(), set);
    if (result == VK_SUCCESS) {
      return set;
    }

    if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL) {
      return std::unexpected(result);
    }

    auto pool = self.add_pool();
    if (!pool) {
      return std::unexpected(pool.error());
    }

    VIR_CHECK(allocate(*pool, set));
    return set;
  }

  auto write_sets(
    this Execution& self,
    VkDescriptorSet set,
    std::span<const ResolvedDescriptor> descriptors,
    bool is_push,
    VkPipelineBindPoint bind_point,
    VkPipelineLayout layout,
    u32 set_index
  ) -> std::expected<void, VkResult> {
    auto stack = memory::ScopedStack{};
    auto image_infos = stack.alloc<VkDescriptorImageInfo>(descriptors.size());
    auto buffer_infos = stack.alloc<VkDescriptorBufferInfo>(descriptors.size());
    auto acceleration_infos = stack.alloc<VkWriteDescriptorSetAccelerationStructureKHR>(descriptors.size());
    auto writes = stack.alloc<VkWriteDescriptorSet>(descriptors.size());
    for (usize index = 0; index < descriptors.size(); index++) {
      const auto& descriptor = descriptors[index];
      auto& write = writes[index];
      write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = nullptr,
        .dstSet = set,
        .dstBinding = descriptor.binding,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = *descriptor.descriptor_type,
        .pImageInfo = nullptr,
        .pBufferInfo = nullptr,
        .pTexelBufferView = nullptr,
      };

      switch (descriptor.kind) {
        case ResolvedDescriptor::Kind::Sampler:
          image_infos[index] =
            {.sampler = descriptor.sampler, .imageView = VK_NULL_HANDLE, .imageLayout = VK_IMAGE_LAYOUT_UNDEFINED};
          write.pImageInfo = &image_infos[index];
          break;
        case ResolvedDescriptor::Kind::Image               :
        case ResolvedDescriptor::Kind::CombinedImageSampler: {
          auto layout_for = descriptor.image_layout;
          if (layout_for == VK_IMAGE_LAYOUT_UNDEFINED) {
            layout_for = *descriptor.descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                           ? VK_IMAGE_LAYOUT_GENERAL
                           : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
          }

          image_infos[index] =
            {.sampler = descriptor.sampler, .imageView = descriptor.image_view, .imageLayout = layout_for};
          write.pImageInfo = &image_infos[index];
          break;
        }
        case ResolvedDescriptor::Kind::TexelBuffer: write.pTexelBufferView = &descriptor.view; break;
        case ResolvedDescriptor::Kind::Buffer     : {
          const auto range = descriptor.range == VK_WHOLE_SIZE ? descriptor.buffer_size - descriptor.offset
                                                               : descriptor.range;
          buffer_infos[index] = {
            .buffer = descriptor.buffer,
            .offset = descriptor.base_offset + descriptor.offset,
            .range = range,
          };
          write.pBufferInfo = &buffer_infos[index];
          break;
        }
        case ResolvedDescriptor::Kind::AccelerationStructure:
          acceleration_infos[index] = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
            .pNext = nullptr,
            .accelerationStructureCount = 1,
            .pAccelerationStructures = &descriptor.acceleration_structure,
          };
          write.pNext = &acceleration_infos[index];
          break;
      }
    }

    if (is_push) {
      self.fn().vkCmdPushDescriptorSetKHR(
        self.command_buffer(),
        bind_point,
        layout,
        set_index,
        static_cast<u32>(writes.size()),
        writes.data()
      );
    } else {
      self.fn()
        .vkUpdateDescriptorSets(self.context->device, static_cast<u32>(writes.size()), writes.data(), 0, nullptr);
    }

    return {};
  }

  auto bind_resolved_descriptor_sets(
    this Execution& self, PipelineID pipeline, std::span<const ResolvedDescriptor> descriptors
  ) -> std::expected<void, VkResult> {
    const auto* declared = self.pipeline(pipeline);
    if (!declared || !declared->layout.has_value()) {
      return std::unexpected(VK_ERROR_UNKNOWN);
    }

    const auto& layout = *declared->layout;
    const auto bind_point = declared->bind_point();
    // a pass can draw with several pipelines, each only reads the locations of its own layout
    auto ordinary = std::map<u32, std::vector<ResolvedDescriptor>>{};
    for (const auto& expected : declared->bindings) {
      if (layout.bindless.has_value() && layout.bindless->index == expected.set) {
        continue;
      }

      const auto found = std::ranges::lower_bound(
        descriptors,
        std::pair{expected.set, expected.binding},
        {},
        [](const auto& d) { return std::pair{d.set, d.binding}; }
      );
      if (found == descriptors.end() || found->set != expected.set || found->binding != expected.binding) {
        OX_LOG_ERROR(
          "pipeline {} reads set {} binding {}, but the pass bound nothing there",
          declared->name,
          expected.set,
          expected.binding
        );
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
      }

      auto descriptor = *found;
      if (
        !descriptor.supports_type(expected.descriptor_type) ||
        (descriptor.descriptor_type.has_value() && *descriptor.descriptor_type != expected.descriptor_type)
      ) {
        OX_LOG_ERROR(
          "descriptor at set {} binding {} does not match the type pipeline {} declares",
          expected.set,
          expected.binding,
          declared->name
        );
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
      }

      VIR_TRY(descriptor.validate());
      descriptor.descriptor_type = expected.descriptor_type;
      ordinary[expected.set].push_back(descriptor);
    }

    auto sets = std::vector<std::pair<u32, VkDescriptorSet>>{};
    for (auto& [set, bindings] : ordinary) {
      if (set >= layout.sets.size()) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
      }

      const auto& set_layout = layout.sets[set];
      if (set_layout.is_push) {
        VIR_TRY(self.write_sets(VK_NULL_HANDLE, bindings, true, bind_point, layout.handle, set));
        continue;
      }

      auto key = DescriptorSetKey{.pipeline = pipeline, .set = set, .descriptors = bindings};
      if (const auto it = self.descriptor_cache.find(key); it != self.descriptor_cache.end()) {
        sets.emplace_back(set, it->second);
        continue;
      }

      auto descriptor_set = self.allocate_set(set_layout.handle);
      if (!descriptor_set) {
        return std::unexpected(descriptor_set.error());
      }

      VIR_TRY(self.write_sets(*descriptor_set, bindings, false, bind_point, layout.handle, set));
      self.descriptor_cache.emplace(std::move(key), *descriptor_set);
      sets.emplace_back(set, *descriptor_set);
    }

    if (layout.bindless.has_value()) {
      sets.emplace_back(layout.bindless->index, layout.bindless->set);
    }

    std::ranges::sort(sets, {}, &std::pair<u32, VkDescriptorSet>::first);
    for (const auto& [set, descriptor_set] : sets) {
      self.fn()
        .vkCmdBindDescriptorSets(self.command_buffer(), bind_point, layout.handle, set, 1, &descriptor_set, 0, nullptr);
    }

    return {};
  }

  auto bind_active_descriptors(this Execution& self, PipelineID pipeline) -> std::expected<void, VkResult> {
    if (self.bound_descriptors == pipeline) {
      return {};
    }

    VIR_TRY(self.bind_resolved_descriptor_sets(pipeline, self.active_descriptors));
    self.bound_descriptors = pipeline;
    return {};
  }

  auto record_push_constants(this Execution& self, PipelineID pipeline, const PushConstants& push)
    -> std::expected<void, VkResult> {
    if (push.is_empty()) {
      return {};
    }

    auto resolved = push;
    for (const auto& variable : push.variables) {
      const auto& bytes = self.get<Bytes>(variable.source);
      if (bytes.data.size() != variable.size) {
        OX_LOG_ERROR(
          "push constant variable holds {} bytes but was declared with {}",
          bytes.data.size(),
          variable.size
        );
        return std::unexpected(VK_ERROR_UNKNOWN);
      }

      if (
        variable.offset < resolved.offset || variable.offset - resolved.offset + variable.size > resolved.data.size()
      ) {
        OX_LOG_ERROR("a push constant variable at {} does not fit its block", variable.offset);
        return std::unexpected(VK_ERROR_UNKNOWN);
      }

      std::ranges::copy(bytes.data, resolved.data.begin() + (variable.offset - resolved.offset));
    }

    // a transient buffer has no address until it is allocated, so the block only gets one now
    for (const auto& [offset, buffer] : push.addresses) {
      const auto address = self.get<Buffer>(buffer).device_address;
      if (offset < resolved.offset || offset - resolved.offset + sizeof(VkDeviceAddress) > resolved.data.size()) {
        OX_LOG_ERROR("a pushed device address at {} does not fit the block it was declared in", offset);
        return std::unexpected(VK_ERROR_UNKNOWN);
      }

      std::memcpy(resolved.data.data() + (offset - resolved.offset), &address, sizeof(VkDeviceAddress));
    }

    const auto* declared = self.pipeline(pipeline);
    if (!declared || !declared->layout.has_value()) {
      return std::unexpected(VK_ERROR_UNKNOWN);
    }

    const auto& layout = *declared->layout;
    if (
      self.recorded_push.has_value() && self.recorded_push->first == layout.handle &&
      self.recorded_push->second.offset == resolved.offset && self.recorded_push->second.data == resolved.data
    ) {
      return {};
    }

    u32 covered = 0;
    for (const auto& range : layout.cover(resolved.offset, resolved.size())) {
      self.fn().vkCmdPushConstants(
        self.command_buffer(),
        layout.handle,
        static_cast<VkShaderStageFlags>(range.stages),
        range.offset,
        range.size,
        resolved.data.data() + (range.offset - resolved.offset)
      );
      covered += range.size;
    }

    if (covered < resolved.size()) {
      auto lock = std::unique_lock(self.graph->warned_mutex);
      if (self.graph->warned_push_constants.insert(pipeline).second) {
        OX_LOG_WARN(
          "pipeline {} is pushed {} bytes at {} but its shaders declare {} of them, the rest are dropped",
          declared->name,
          resolved.size(),
          resolved.offset,
          covered
        );
      }
    }

    self.recorded_push = std::pair{layout.handle, std::move(resolved)};
    return {};
  }

  auto bind_pipeline_handle(this Execution& self, VkPipelineBindPoint bind_point, VkPipeline handle) -> void {
    if (self.bound_pipeline != handle) {
      self.fn().vkCmdBindPipeline(self.command_buffer(), bind_point, handle);
      self.bound_pipeline = handle;
    }
  }

  auto prepare_draw(
    this Execution& self,
    PipelineID pipeline,
    const PipelineState& state,
    const DynamicValues& dynamic,
    bool should_bind_descriptors
  ) -> std::expected<void, VkResult> {
    auto* declared = self.pipeline(pipeline);
    if (!declared || declared->kind != PipelineKind::Graphics) {
      OX_LOG_ERROR("a draw runs pipeline {} which is not a graphics pipeline", std::to_underlying(pipeline));
      return std::unexpected(VK_ERROR_UNKNOWN);
    }

    const auto variant = declared->graphics_variants.find(state);
    if (variant == declared->graphics_variants.end()) {
      return std::unexpected(VK_ERROR_UNKNOWN);
    }

    self.bind_pipeline_handle(VK_PIPELINE_BIND_POINT_GRAPHICS, variant->second);
    if (should_bind_descriptors) {
      VIR_TRY(self.bind_active_descriptors(pipeline));
    }

    const auto viewports = dynamic.resolved_viewports(self.render_area);
    if (!viewports.empty() && viewports != self.recorded_viewports) {
      auto stack = memory::ScopedStack{};
      auto handles = stack.alloc<VkViewport>(viewports.size());
      for (usize index = 0; index < viewports.size(); index++) {
        handles[index] = viewports[index].to_vk();
      }

      self.fn().vkCmdSetViewport(self.command_buffer(), 0, static_cast<u32>(handles.size()), handles.data());
      self.recorded_viewports = viewports;
    }

    const auto scissors = dynamic.resolved_scissors(self.render_area);
    const auto scissors_equal = scissors.size() == self.recorded_scissors.size() &&
                                std::ranges::equal(
                                  scissors,
                                  self.recorded_scissors,
                                  [](const VkRect2D& a, const VkRect2D& b) {
                                    return std::memcmp(&a, &b, sizeof(VkRect2D)) == 0;
                                  }
                                );
    if (!scissors.empty() && !scissors_equal) {
      self.fn().vkCmdSetScissor(self.command_buffer(), 0, static_cast<u32>(scissors.size()), scissors.data());
      self.recorded_scissors = scissors;
    }

    return self.record_push_constants(pipeline, dynamic.push_constants);
  }

  auto local_size(this const Execution& self, PipelineID pipeline, const Specialization& specialization)
    -> std::array<u32, 3> {
    const auto* declared = self.pipeline(pipeline);
    if (!declared || declared->reflections.empty()) {
      return {1, 1, 1};
    }

    const auto& reflection = declared->reflections.front();
    auto local = reflection.local_size;
    for (usize axis = 0; axis < 3; axis++) {
      if (reflection.local_size_spec_ids[axis] == NO_SPEC_ID) {
        continue;
      }

      const auto value = specialization.get(reflection.local_size_spec_ids[axis]);
      if (value.has_value()) {
        if (const auto specialized = value->as_workgroup_axis(); specialized.has_value() && *specialized > 0) {
          local[axis] = *specialized;
        }
      }
    }

    return local;
  }

  auto groups_for(
    this const Execution& self,
    std::array<u32, 3> invocations,
    PipelineID pipeline,
    const Specialization& specialization
  ) -> std::array<u32, 3> {
    const auto local = self.local_size(pipeline, specialization);
    return {
      (invocations[0] + local[0] - 1) / local[0],
      (invocations[1] + local[1] - 1) / local[1],
      (invocations[2] + local[2] - 1) / local[2],
    };
  }

  auto prepare_dispatch(
    this Execution& self,
    PipelineID pipeline,
    const Specialization& specialization,
    const PushConstants& push,
    bool should_bind_descriptors
  ) -> std::expected<const RayTracingVariant*, VkResult> {
    auto* declared = self.pipeline(pipeline);
    if (!declared) {
      return std::unexpected(VK_ERROR_UNKNOWN);
    }

    const RayTracingVariant* ray_tracing = nullptr;
    if (declared->kind == PipelineKind::Compute) {
      const auto variant = declared->compute_variants.find(specialization);
      if (variant == declared->compute_variants.end()) {
        return std::unexpected(VK_ERROR_UNKNOWN);
      }

      self.bind_pipeline_handle(VK_PIPELINE_BIND_POINT_COMPUTE, variant->second);
    } else if (declared->kind == PipelineKind::RayTracing) {
      const auto variant = declared->ray_tracing_variants.find(specialization);
      if (variant == declared->ray_tracing_variants.end()) {
        return std::unexpected(VK_ERROR_UNKNOWN);
      }

      self.bind_pipeline_handle(VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, variant->second.pipeline);
      ray_tracing = &variant->second;
    } else {
      OX_LOG_ERROR("a dispatch runs graphics pipeline {}", declared->name);
      return std::unexpected(VK_ERROR_UNKNOWN);
    }

    if (should_bind_descriptors) {
      VIR_TRY(self.bind_active_descriptors(pipeline));
    }
    VIR_TRY(self.record_push_constants(pipeline, push));
    return ray_tracing;
  }

  auto resolve_descriptor(this const Execution& self, const ir::WriteDescriptor& write) -> ResolvedDescriptor {
    const auto& descriptor = write.descriptor;
    auto resolved = ResolvedDescriptor{
      .set = write.set,
      .binding = write.binding,
      .descriptor_type = write.descriptor_type,
      .kind = static_cast<ResolvedDescriptor::Kind>(descriptor.kind),
      .sampler = descriptor.sampler,
      .image_layout = write.image_layout,
      .view = descriptor.view,
      .offset = descriptor.offset,
      .range = descriptor.range,
      .acceleration_structure = descriptor.acceleration_structure,
    };
    if (const auto image = descriptor.image_value(); image.has_value()) {
      resolved.image_view = self.get<ImageAttachment>(*image).image_view;
    }

    if (const auto buffer_id = descriptor.buffer_value(); buffer_id.has_value()) {
      const auto& buffer = self.get<Buffer>(*buffer_id);
      resolved.buffer = buffer.handle;
      resolved.buffer_size = buffer.size;
      resolved.base_offset = buffer.offset;
    }

    return resolved;
  }

  auto execute_one(this Execution& self, const Instr& instruction) -> std::expected<void, VkResult>;

  auto callback_resources(this Execution& self, std::span<const std::pair<ValueID, ValueID>> resources) -> void {
    self.callback_images.clear();
    for (const auto& [resource, access] : resources) {
      for (const auto element : self.elements(resource)) {
        if (const auto* image = std::get_if<ImageAttachment>(&self.value(element))) {
          self.callback_images.emplace_back(*image, self.get<Access>(access));
        }
      }
    }
  }
};

auto Execution::execute_one(this Execution& self, const Instr& instruction) -> std::expected<void, VkResult> {
  const auto id = instruction.id;
  return std::visit(
    [&](const auto& node) -> std::expected<void, VkResult> {
      using T = std::decay_t<decltype(node)>;
      if constexpr (std::is_same_v<T, ir::Constant>) {
        switch (node.kind) {
          case ir::Constant::Kind::I32       : self.set_value(id, node.as_i32()); break;
          case ir::Constant::Kind::U32       : self.set_value(id, node.as_u32()); break;
          case ir::Constant::Kind::U64       : self.set_value(id, node.as_u64()); break;
          case ir::Constant::Kind::Extent2D  : self.set_value(id, node.as_extent_2d()); break;
          case ir::Constant::Kind::Extent3D  : self.set_value(id, node.as_extent_3d()); break;
          case ir::Constant::Kind::String    : self.set_value(id, std::string_view(node.string)); break;
          case ir::Constant::Kind::Access    : self.set_value(id, node.as_access()); break;
          case ir::Constant::Kind::ClearValue: self.set_value(id, node.as_clear()); break;
        }
      } else if constexpr (std::is_same_v<T, ir::Array>) {
        self.set_value(id, Slice(node.elements));
      } else if constexpr (std::is_same_v<T, ir::Index>) {
        const auto& elements = self.get<Slice>(node.array);
        const auto index = self.get<u32>(node.index);
        if (index >= elements.size()) {
          return std::unexpected(VK_ERROR_UNKNOWN);
        }

        self.forward(id, elements[index]);
      } else if constexpr (std::is_same_v<T, ir::ConstructBuffer>) {
        if (self.program->bound.contains(id)) {
          return {};
        }

        if (!node.buffer.is_null()) {
          self.set_value(id, node.buffer);
          return {};
        }

        auto buffer = self.allocator->allocate_buffer({
          .size = is_valid(node.size) ? self.get<u64>(node.size) : 0,
          .usage = node.usage,
          .location = node.location,
          .alignment = 1,
          .name = is_valid(node.name) ? std::string(self.get<std::string_view>(node.name)) : std::string{},
        });
        if (!buffer) {
          return std::unexpected(buffer.error());
        }

        self.set_value(id, *buffer);
      } else if constexpr (std::is_same_v<T, ir::ConstructImage>) {
        if (self.program->bound.contains(id)) {
          return {};
        }

        const auto extent = is_valid(node.extent) ? self.get<Extent3D>(node.extent) : Extent3D{};
        const auto range = VkImageSubresourceRange{
          .aspectMask = aspect_mask(node.format),
          .baseMipLevel = self.get<u32>(node.base_level),
          .levelCount = self.get<u32>(node.level_count),
          .baseArrayLayer = self.get<u32>(node.base_layer),
          .layerCount = self.get<u32>(node.layer_count),
        };

        auto image = node.image;
        if (image.is_null()) {
          auto name = std::string{};
          if (is_valid(node.name)) {
            name = std::string(self.get<std::string_view>(node.name));
          }

          auto allocated = self.allocator->allocate_image({
            .extent = extent,
            .format = node.format,
            .usage = node.usage,
            .image_type = node.image_type,
            .view_type = node.view_type,
            .flags = node.flags,
            .mip_levels = range.levelCount,
            .array_layers = range.layerCount,
            .samples = node.samples,
            .location = MemoryLocation::GpuOnly,
            .view_formats = node.view_formats,
            .name = std::move(name),
          });
          if (!allocated) {
            return std::unexpected(allocated.error());
          }

          image = *allocated;
        }

        auto view = node.image_view;
        if (view == VK_NULL_HANDLE) {
          auto allocated = self.allocator->allocate_image_view(image.handle, node.format, node.view_type, range);
          if (!allocated) {
            return std::unexpected(allocated.error());
          }

          view = *allocated;
        }

        self.set_value(
          id,
          ImageAttachment{
            .image = image,
            .image_view = view,
            .view_type = node.view_type,
            .format = node.format,
            .extent = extent,
            .samples = node.samples,
            .layout = node.initial_layout,
            .subresource_range = range,
          }
        );
      } else if constexpr (std::is_same_v<T, ir::View>) {
        // ranges are relative to the root, which the parent may already have narrowed
        auto view = self.get<ImageAttachment>(node.root);
        view.subresource_range = node.range.apply(view.subresource_range);
        if (view.subresource_range.levelCount == 0 || view.subresource_range.layerCount == 0) {
          OX_LOG_ERROR("view %{} falls outside the image it is taken from", std::to_underlying(id));
          return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
        }

        const auto& root = self.get<ImageAttachment>(node.root);
        view.format = node.format != Format::Undefined ? node.format : root.format;
        view.view_type = view_type_for(root, view.subresource_range, node.view_type);
        auto allocated = self.allocator->allocate_image_view(
          root.image.handle,
          view.format,
          view.view_type,
          view.subresource_range
        );
        if (!allocated) {
          return std::unexpected(allocated.error());
        }

        view.image_view = *allocated;
        self.set_value(id, std::move(view));
      } else if constexpr (std::is_same_v<T, ir::AcquireNextImage>) {
        const auto& swapchain = self.get<AcquiredSwapchain>(node.swapchain);
        auto semaphore = self.allocator->allocate_binary_semaphore();
        if (!semaphore) {
          return std::unexpected(semaphore.error());
        }

        u32 image_index = 0;
        const auto result = self.fn().vkAcquireNextImageKHR(
          self.context->device,
          swapchain.handle,
          ~0_u64,
          *semaphore,
          VK_NULL_HANDLE,
          &image_index
        );
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
          return std::unexpected(result);
        }

        self.current_submit.waits.push_back({.semaphore = *semaphore, .value = 0, .access = Access::MemoryRW});
        self.set_value(id, image_index);
      } else if constexpr (std::is_same_v<T, ir::SwapchainImage>) {
        const auto image_index = self.get<u32>(node.acquire);
        const auto& swapchain = self.get<AcquiredSwapchain>(node.swapchain);
        if (image_index >= swapchain.attachments.size() || image_index >= swapchain.present_semaphores.size()) {
          return std::unexpected(VK_ERROR_UNKNOWN);
        }

        self.resource_to_swapchain.insert_or_assign(
          id,
          PresentInfo{
            .image_index = image_index,
            .swapchain = swapchain.handle,
            .semaphore = swapchain.present_semaphores[image_index],
          }
        );
        self.set_value(id, swapchain.attachments[image_index]);
      } else if constexpr (std::is_same_v<T, ir::Export>) {
        const auto access = self.get<Access>(node.access);
        const auto resolved = self.resolve_id(node.resource);
        if (contains(node.dst_domain, DomainFlag::Present)) {
          const auto present = self.resource_to_swapchain.find(resolved);
          if (present == self.resource_to_swapchain.end()) {
            OX_LOG_ERROR("only an image acquired from a swapchain can be presented");
            return std::unexpected(VK_ERROR_UNKNOWN);
          }

          VIR_TRY(
            self.flush_submit(SemaphoreSubmit{.semaphore = present->second.semaphore, .value = 0, .access = access})
          );
          self.presents.push_back(present->second);
          self.resource_to_swapchain.erase(present);
        }

        self.forward(id, node.resource);
      } else if constexpr (std::is_same_v<T, ir::Clear>) {
        VIR_TRY(self.ensure_batch());
        self.forward(id, node.attachment);
        const auto& attachment = self.get<ImageAttachment>(node.attachment);
        const auto color = self.get<ClearValue>(node.color);
        const auto range = attachment.subresource_range;
        if (range.aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
          self.fn().vkCmdClearDepthStencilImage(
            self.command_buffer(),
            attachment.image.handle,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            &color.value.depthStencil,
            1,
            &range
          );
        } else {
          self.fn().vkCmdClearColorImage(
            self.command_buffer(),
            attachment.image.handle,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            &color.value.color,
            1,
            &range
          );
        }
      } else if constexpr (std::is_same_v<T, ir::Blit>) {
        VIR_TRY(self.ensure_batch());
        self.forward(id, node.dst);
        const auto& src = self.get<ImageAttachment>(node.src);
        const auto& dst = self.get<ImageAttachment>(node.dst);
        const auto layers = [](const ImageAttachment& attachment) {
          return VkImageSubresourceLayers{
            .aspectMask = attachment.subresource_range.aspectMask,
            .mipLevel = attachment.subresource_range.baseMipLevel,
            .baseArrayLayer = attachment.subresource_range.baseArrayLayer,
            .layerCount = attachment.subresource_range.layerCount,
          };
        };
        const auto corner = [](const Extent3D& extent) {
          return VkOffset3D{
            static_cast<i32>(extent.width),
            static_cast<i32>(extent.height),
            static_cast<i32>(std::max(extent.depth, 1_u32)),
          };
        };
        const auto region = VkImageBlit{
          .srcSubresource = layers(src),
          .srcOffsets = {{0, 0, 0}, corner(src.view_extent())},
          .dstSubresource = layers(dst),
          .dstOffsets = {{0, 0, 0}, corner(dst.view_extent())},
        };
        self.fn().vkCmdBlitImage(
          self.command_buffer(),
          src.image.handle,
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          dst.image.handle,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          1,
          &region,
          node.filter
        );
      } else if constexpr (std::is_same_v<T, ir::CopyBufferToImage> || std::is_same_v<T, ir::CopyImageToBuffer>) {
        VIR_TRY(self.ensure_batch());
        constexpr auto IS_UPLOAD = std::is_same_v<T, ir::CopyBufferToImage>;
        self.forward(id, IS_UPLOAD ? node.image : node.buffer);
        auto image = self.get<ImageAttachment>(node.image);
        image.layout = IS_UPLOAD ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        return record_copy_buffer_image(*self.batch, self.get<Buffer>(node.buffer), image, node.region, IS_UPLOAD);
      } else if constexpr (std::is_same_v<T, ir::CopyBuffer>) {
        VIR_TRY(self.ensure_batch());
        self.forward(id, node.dst);
        return record_copy_buffer(*self.batch, self.get<Buffer>(node.src), self.get<Buffer>(node.dst), node.region);
      } else if constexpr (std::is_same_v<T, ir::FillBuffer>) {
        VIR_TRY(self.ensure_batch());
        self.forward(id, node.buffer);
        return record_fill_buffer(*self.batch, self.get<Buffer>(node.buffer), node.data, node.offset, node.size);
      } else if constexpr (std::is_same_v<T, ir::UpdateBuffer>) {
        VIR_TRY(self.ensure_batch());
        self.forward(id, node.buffer);
        const auto data = is_valid(node.source) ? std::span<const u8>(self.get<Bytes>(node.source).data)
                                                : std::span<const u8>(node.data);
        return record_update_buffer(*self.batch, self.get<Buffer>(node.buffer), data, node.offset);
      } else if constexpr (std::is_same_v<T, ir::CopyImage> || std::is_same_v<T, ir::ResolveImage>) {
        VIR_TRY(self.ensure_batch());
        self.forward(id, node.dst);
        auto src = self.get<ImageAttachment>(node.src);
        auto dst = self.get<ImageAttachment>(node.dst);
        src.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        dst.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        return record_copy_image(*self.batch, src, dst, node.region, std::is_same_v<T, ir::ResolveImage>);
      } else if constexpr (std::is_same_v<T, ir::ConstructQueryPool>) {
        auto queries = self.allocator->allocate_query_pool(node.type, node.count);
        if (!queries) {
          return std::unexpected(queries.error());
        }
        self.set_value(id, *queries);
      } else if constexpr (std::is_same_v<T, ir::BuildAccelerationStructures>) {
        if (!self.fn().vkCmdBuildAccelerationStructuresKHR) {
          return std::unexpected(VK_ERROR_EXTENSION_NOT_PRESENT);
        }
        const auto& builds = self.get<AccelerationStructureBuilds>(node.builds);
        if (!builds || builds->empty()) {
          return {};
        }
        VIR_TRY(self.ensure_batch());
        // separate calls preserve the order of dependent builds within the variable
        for (usize build_index = 0; build_index < builds->size(); build_index++) {
          const auto& build = (*builds)[build_index];
          if (
            build.geometries.empty() || build.geometries.size() != build.ranges.size() ||
            build.info.dstAccelerationStructure == VK_NULL_HANDLE
          ) {
            return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
          }
          auto stack = memory::ScopedStack{};
          auto geometries = stack.alloc<VkAccelerationStructureGeometryKHR>(build.geometries.size());
          std::ranges::copy(build.geometries, geometries.begin());
          auto info = build.info;
          info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
          info.geometryCount = static_cast<u32>(geometries.size());
          info.pGeometries = geometries.data();
          info.ppGeometries = nullptr;
          for (const auto& patch : node.addresses) {
            if (patch.build_index >= builds->size()) {
              return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
            }
            if (patch.build_index != build_index) {
              continue;
            }
            const auto& buffer = self.get<Buffer>(patch.buffer);
            if (
              buffer.device_address == 0 || patch.offset >= buffer.size || patch.offset > ~0_u64 - buffer.device_address
            ) {
              return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
            }
            // buffer device addresses already include their suballocation offset
            const auto address = buffer.device_address + patch.offset;
            if (patch.kind == AccelerationStructureAddressKind::Scratch) {
              if (address % self.context->acceleration_structure_scratch_alignment != 0) {
                return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
              }
              info.scratchData.deviceAddress = address;
              continue;
            }
            if (patch.geometry_index >= geometries.size()) {
              return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
            }
            auto& geometry = geometries[patch.geometry_index];
            switch (patch.kind) {
              case AccelerationStructureAddressKind::Vertices:
              case AccelerationStructureAddressKind::Indices :
              case AccelerationStructureAddressKind::Transform:
                if (geometry.geometryType != VK_GEOMETRY_TYPE_TRIANGLES_KHR) {
                  return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
                }
                if (patch.kind == AccelerationStructureAddressKind::Vertices) {
                  geometry.geometry.triangles.vertexData.deviceAddress = address;
                } else if (patch.kind == AccelerationStructureAddressKind::Indices) {
                  geometry.geometry.triangles.indexData.deviceAddress = address;
                } else {
                  geometry.geometry.triangles.transformData.deviceAddress = address;
                }
                break;
              case AccelerationStructureAddressKind::Aabbs:
                if (geometry.geometryType != VK_GEOMETRY_TYPE_AABBS_KHR) {
                  return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
                }
                geometry.geometry.aabbs.data.deviceAddress = address;
                break;
              case AccelerationStructureAddressKind::Instances:
                if (geometry.geometryType != VK_GEOMETRY_TYPE_INSTANCES_KHR) {
                  return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
                }
                geometry.geometry.instances.data.deviceAddress = address;
                break;
              case AccelerationStructureAddressKind::Scratch: break;
            }
          }
          const auto* ranges = build.ranges.data();
          self.fn().vkCmdBuildAccelerationStructuresKHR(self.command_buffer(), 1, &info, &ranges);
          if (build_index + 1 < builds->size()) {
            const auto barrier = CommandBuffer::memory_barrier_info(
              Access::AccelerationStructureBuildRW,
              Access::AccelerationStructureBuildRW
            );
            self.batch->pipeline_barrier(std::span(&barrier, 1), {});
          }
        }
      } else if constexpr (std::is_same_v<T, ir::WriteAccelerationStructureProperties>) {
        if (!self.fn().vkCmdWriteAccelerationStructuresPropertiesKHR) {
          return std::unexpected(VK_ERROR_EXTENSION_NOT_PRESENT);
        }
        const auto& queries = self.get<QueryPool>(node.queries);
        if (
          queries.handle == VK_NULL_HANDLE || node.first_query >= queries.count ||
          (queries.type != VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR &&
           queries.type != VK_QUERY_TYPE_ACCELERATION_STRUCTURE_SERIALIZATION_SIZE_KHR)
        ) {
          return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
        }
        VIR_TRY(self.ensure_batch());
        self.forward(id, node.source);
        self.fn().vkCmdResetQueryPool(self.command_buffer(), queries.handle, node.first_query, 1);
        self.fn().vkCmdWriteAccelerationStructuresPropertiesKHR(
          self.command_buffer(),
          1,
          &node.structure,
          queries.type,
          queries.handle,
          node.first_query
        );
        self.current_submit.has_query_writes = true;
      } else if constexpr (std::is_same_v<T, ir::CopyAccelerationStructure>) {
        if (!self.fn().vkCmdCopyAccelerationStructureKHR) {
          return std::unexpected(VK_ERROR_EXTENSION_NOT_PRESENT);
        }
        VIR_TRY(self.ensure_batch());
        self.forward(id, node.dst);
        const auto copied = VkCopyAccelerationStructureInfoKHR{
          .sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR,
          .pNext = nullptr,
          .src = node.source,
          .dst = node.destination,
          .mode = node.mode
        };
        self.fn().vkCmdCopyAccelerationStructureKHR(self.command_buffer(), &copied);
      } else if constexpr (std::is_same_v<T, ir::BeginRendering>) {
        VIR_TRY(self.ensure_batch());
        self.begin_pass(node.name);
        self.open_descriptors();
        self.callback_resources(node.attachments);

        struct Attachment {
          ImageAttachment image;
          Access access;
          option<ClearValue> clear;
        };
        auto colors = std::vector<Attachment>{};
        auto depth = option<Attachment>{};
        for (const auto& [resource, access_id] : node.attachments) {
          const auto access = self.get<Access>(access_id);
          auto clear = option<ClearValue>{};
          for (const auto& [cleared, color] : node.clears) {
            if (cleared == resource) {
              clear = self.get<ClearValue>(color);
            }
          }

          if (intersects(access, Access::ColorRW)) {
            for (const auto element : self.elements(resource)) {
              colors.push_back({self.get<ImageAttachment>(element), access, clear});
            }
          }

          if (!depth.has_value() && intersects(access, Access::DepthStencilRW)) {
            const auto elements = self.elements(resource);
            if (!elements.empty()) {
              depth = Attachment{self.get<ImageAttachment>(elements.front()), access, clear};
            }
          }
        }

        auto extent = Extent2D{};
        if (is_valid(node.render_area)) {
          extent = self.get<Extent2D>(node.render_area);
        } else if (!colors.empty()) {
          const auto view = colors.front().image.view_extent();
          extent = {view.width, view.height};
        } else if (depth.has_value()) {
          const auto view = depth->image.view_extent();
          extent = {view.width, view.height};
        } else {
          OX_LOG_ERROR("a rendering pass with no attachments has to name its area");
          return std::unexpected(VK_ERROR_UNKNOWN);
        }

        self.render_area = {.offset = {}, .extent = std::bit_cast<VkExtent2D>(extent)};
        const auto to_info = [](const Attachment& attachment) {
          auto info = VkRenderingAttachmentInfo{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .pNext = nullptr,
            .imageView = attachment.image.image_view,
            .imageLayout = to_image_layout(attachment.access),
            .resolveMode = VK_RESOLVE_MODE_NONE,
            .resolveImageView = VK_NULL_HANDLE,
            .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .loadOp = intersects(attachment.access, Access::ColorRead | Access::DepthStencilRead)
                        ? VK_ATTACHMENT_LOAD_OP_LOAD
                        : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = {},
          };
          if (attachment.clear.has_value()) {
            info.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            info.clearValue = attachment.clear->value;
          }

          return info;
        };

        auto color_infos = std::vector<VkRenderingAttachmentInfo>{};
        for (const auto& color : colors) {
          color_infos.push_back(to_info(color));
        }

        auto depth_info = VkRenderingAttachmentInfo{};
        auto has_depth = false;
        auto has_stencil = false;
        if (depth.has_value()) {
          depth_info = to_info(*depth);
          const auto aspects = depth->image.subresource_range.aspectMask;
          has_depth = (aspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
          has_stencil = (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
        }

        const auto rendering_info = VkRenderingInfo{
          .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
          .pNext = nullptr,
          .flags = 0,
          .renderArea = self.render_area,
          .layerCount = node.layer_count,
          .viewMask = node.view_mask,
          .colorAttachmentCount = static_cast<u32>(color_infos.size()),
          .pColorAttachments = color_infos.data(),
          .pDepthAttachment = has_depth ? &depth_info : nullptr,
          .pStencilAttachment = has_stencil ? &depth_info : nullptr,
        };
        self.fn().vkCmdBeginRendering(self.command_buffer(), &rendering_info);
        self.is_rendering = true;
        if (!node.attachments.empty()) {
          self.forward(id, node.attachments.front().first);
        }
      } else if constexpr (std::is_same_v<T, ir::BindPipeline> || std::is_same_v<T, ir::SetState>) {
        self.forward(id, node.pass);
      } else if constexpr (std::is_same_v<T, ir::WriteDescriptor>) {
        self.forward(id, node.pass);
        const auto written = self.resolve_descriptor(node);
        VIR_TRY(written.validate());
        const auto key = std::pair{written.set, written.binding};
        const auto it = std::ranges::lower_bound(self.active_descriptors, key, {}, [](const ResolvedDescriptor& d) {
          return std::pair{d.set, d.binding};
        });
        if (it != self.active_descriptors.end() && it->set == written.set && it->binding == written.binding) {
          if (*it == written) {
            return {};
          }

          *it = written;
        } else {
          self.active_descriptors.insert(it, written);
        }

        self.bound_descriptors = nullopt;
      } else if constexpr (std::is_same_v<T, ir::BindVertexBuffers>) {
        self.forward(id, node.pass);
        VIR_TRY(self.ensure_batch());
        const auto& buffers = self.get<Slice>(node.buffers);
        const auto& offsets = self.get<Slice>(node.offsets);
        if (buffers.size() != offsets.size()) {
          return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
        }

        auto stack = memory::ScopedStack{};
        auto handles = stack.alloc<VkBuffer>(buffers.size());
        auto byte_offsets = stack.alloc<VkDeviceSize>(buffers.size());
        for (usize index = 0; index < buffers.size(); index++) {
          const auto& buffer = self.get<Buffer>(buffers[index]);
          const auto offset = buffer.checked_offset(self.get<u64>(offsets[index]));
          if (!offset) {
            return std::unexpected(offset.error());
          }

          handles[index] = buffer.handle;
          byte_offsets[index] = *offset;
        }

        self.fn().vkCmdBindVertexBuffers(
          self.command_buffer(),
          node.first_binding,
          static_cast<u32>(handles.size()),
          handles.data(),
          byte_offsets.data()
        );
      } else if constexpr (std::is_same_v<T, ir::BindIndexBuffer>) {
        self.forward(id, node.pass);
        VIR_TRY(self.ensure_batch());
        const auto& buffer = self.get<Buffer>(node.buffer);
        const auto offset = buffer.checked_offset(node.offset);
        if (!offset) {
          return std::unexpected(offset.error());
        }

        self.fn().vkCmdBindIndexBuffer(self.command_buffer(), buffer.handle, *offset, node.index_type);
      } else if constexpr (std::is_same_v<T, ir::Draw>) {
        self.forward(id, node.pass);
        VIR_TRY(self.prepare_draw(node.pipeline, node.state, node.dynamic, true));
        self.fn().vkCmdDraw(
          self.command_buffer(),
          self.get<u32>(node.vertex_count),
          self.get<u32>(node.instance_count),
          self.get<u32>(node.first_vertex),
          self.get<u32>(node.first_instance)
        );
      } else if constexpr (std::is_same_v<T, ir::DrawIndexed>) {
        self.forward(id, node.pass);
        VIR_TRY(self.prepare_draw(node.pipeline, node.state, node.dynamic, true));
        self.fn().vkCmdDrawIndexed(
          self.command_buffer(),
          self.get<u32>(node.index_count),
          self.get<u32>(node.instance_count),
          self.get<u32>(node.first_index),
          self.get<i32>(node.vertex_offset),
          self.get<u32>(node.first_instance)
        );
      } else if constexpr (std::is_same_v<T, ir::DrawIndirect> || std::is_same_v<T, ir::DrawMeshTasksIndirect>) {
        self.forward(id, node.pass);
        VIR_TRY(self.prepare_draw(node.pipeline, node.state, node.dynamic, true));
        const auto& buffer = self.get<Buffer>(node.buffer);
        const auto offset = buffer.checked_offset(node.offset);
        if (!offset) {
          return std::unexpected(offset.error());
        }

        constexpr auto IS_MESH = std::is_same_v<T, ir::DrawMeshTasksIndirect>;
        auto is_indexed = false;
        if constexpr (!IS_MESH) {
          is_indexed = node.is_indexed;
        }

        if (const auto counts_id = node.count.count_buffer(); counts_id.has_value()) {
          const auto& counts = self.get<Buffer>(*counts_id);
          const auto count_offset = counts.checked_offset(node.count.offset);
          if (!count_offset) {
            return std::unexpected(count_offset.error());
          }

          const auto max = self.get<u32>(node.count.max);
          if (IS_MESH) {
            self.fn().vkCmdDrawMeshTasksIndirectCountEXT(
              self.command_buffer(),
              buffer.handle,
              *offset,
              counts.handle,
              *count_offset,
              max,
              node.stride
            );
          } else if (is_indexed) {
            self.fn().vkCmdDrawIndexedIndirectCount(
              self.command_buffer(),
              buffer.handle,
              *offset,
              counts.handle,
              *count_offset,
              max,
              node.stride
            );
          } else {
            self.fn().vkCmdDrawIndirectCount(
              self.command_buffer(),
              buffer.handle,
              *offset,
              counts.handle,
              *count_offset,
              max,
              node.stride
            );
          }
        } else {
          const auto count = self.get<u32>(node.count.fixed);
          if (IS_MESH) {
            self.fn().vkCmdDrawMeshTasksIndirectEXT(self.command_buffer(), buffer.handle, *offset, count, node.stride);
          } else if (is_indexed) {
            self.fn().vkCmdDrawIndexedIndirect(self.command_buffer(), buffer.handle, *offset, count, node.stride);
          } else {
            self.fn().vkCmdDrawIndirect(self.command_buffer(), buffer.handle, *offset, count, node.stride);
          }
        }
      } else if constexpr (std::is_same_v<T, ir::DrawMeshTasks>) {
        self.forward(id, node.pass);
        VIR_TRY(self.prepare_draw(node.pipeline, node.state, node.dynamic, true));
        self.fn().vkCmdDrawMeshTasksEXT(
          self.command_buffer(),
          self.get<u32>(node.x),
          self.get<u32>(node.y),
          self.get<u32>(node.z)
        );
      } else if constexpr (std::is_same_v<T, ir::CallOpaque>) {
        self.forward(id, node.pass);
        if (is_valid(node.pipeline) && self.is_rendering) {
          VIR_TRY(self.prepare_draw(node.pipeline, node.state, node.dynamic, false));
        } else if (is_valid(node.pipeline)) {
          // the callback can finish binding descriptors or skip dispatch when it has no jobs
          VIR_TRY(self.ensure_batch());
          VIR_TRY(self.prepare_dispatch(node.pipeline, node.state.specialization, node.dynamic.push_constants, false));
        }

        const auto& body = self.get<PassCallback>(node.body);
        auto recorder = Recorder{.execution = &self, .pipeline = node.pipeline};
        for (const auto& descriptor : self.active_descriptors) {
          recorder.descriptors.emplace(std::pair{descriptor.set, descriptor.binding}, descriptor);
        }

        if (body && *body) {
          (*body)(recorder);
        }

        // the callback bound whatever it drew with, so nothing the pass wrote is still in force
        self.bound_pipeline = VK_NULL_HANDLE;
        self.bound_descriptors = nullopt;
        self.recorded_viewports.clear();
        self.recorded_scissors.clear();
        self.recorded_push = nullopt;
        if (!recorder.result) {
          return recorder.result;
        }
      } else if constexpr (std::is_same_v<T, ir::EndRendering>) {
        self.forward(id, node.pass);
        self.fn().vkCmdEndRendering(self.command_buffer());
        self.is_rendering = false;
        self.end_pass();
        self.open_descriptors();
      } else if constexpr (std::is_same_v<T, ir::BeginCompute>) {
        VIR_TRY(self.ensure_batch());
        self.begin_pass(node.name);
        self.open_descriptors();
        self.callback_resources(node.attachments);
        if (!node.attachments.empty()) {
          self.forward(id, node.attachments.front().first);
        }
      } else if constexpr (std::is_same_v<T, ir::EndCompute>) {
        self.forward(id, node.pass);
        self.end_pass();
        self.open_descriptors();
      } else if constexpr (std::is_same_v<T, ir::PassResult>) {
        self.forward(id, node.resource);
      } else if constexpr (std::is_same_v<T, ir::Dispatch>) {
        self.forward(id, node.pass);
        VIR_TRY(self.ensure_batch());
        VIR_TRY(self.prepare_dispatch(node.pipeline, node.specialization, node.push_constants, true));
        const auto& size = node.size;
        auto groups = std::array<u32, 3>{};
        switch (size.kind) {
          case ir::DispatchSize::Kind::Groups:
            groups = {self.get<u32>(size.x), self.get<u32>(size.y), self.get<u32>(size.z)};
            break;
          case ir::DispatchSize::Kind::Invocations:
            groups = self.groups_for(
              {self.get<u32>(size.x), self.get<u32>(size.y), self.get<u32>(size.z)},
              node.pipeline,
              node.specialization
            );
            break;
          case ir::DispatchSize::Kind::InvocationsPerPixel: {
            const auto extent = self.get<ImageAttachment>(size.resource).view_extent();
            groups = self.groups_for(
              {ir::scaled(extent.width, size.scale[0]),
               ir::scaled(extent.height, size.scale[1]),
               ir::scaled(extent.depth, size.scale[2])},
              node.pipeline,
              node.specialization
            );
            break;
          }
          case ir::DispatchSize::Kind::InvocationsPerElement: {
            const auto buffer_size = self.get<Buffer>(size.resource).size;
            groups = self.groups_for(
              {ir::scaled(buffer_size / std::max(size.element_size, 1_u64), size.scale[0]), 1, 1},
              node.pipeline,
              node.specialization
            );
            break;
          }
          case ir::DispatchSize::Kind::Indirect: {
            const auto& buffer = self.get<Buffer>(size.resource);
            const auto offset = buffer.checked_offset(size.offset);
            if (!offset) {
              return std::unexpected(offset.error());
            }

            self.fn().vkCmdDispatchIndirect(self.command_buffer(), buffer.handle, *offset);
            return {};
          }
        }

        if (groups[0] != 0 && groups[1] != 0 && groups[2] != 0) {
          self.fn().vkCmdDispatch(self.command_buffer(), groups[0], groups[1], groups[2]);
        }
      } else if constexpr (std::is_same_v<T, ir::TraceRays>) {
        self.forward(id, node.pass);
        VIR_TRY(self.ensure_batch());
        auto variant = self.prepare_dispatch(node.pipeline, node.specialization, node.push_constants, true);
        if (!variant) {
          return std::unexpected(variant.error());
        }

        if (!*variant) {
          OX_LOG_ERROR("rays are traced with a pipeline that is not a ray tracing pipeline");
          return std::unexpected(VK_ERROR_UNKNOWN);
        }

        const auto* tables = *variant;
        self.fn().vkCmdTraceRaysKHR(
          self.command_buffer(),
          &tables->raygen,
          &tables->miss,
          &tables->hit,
          &tables->callable,
          self.get<u32>(node.width),
          self.get<u32>(node.height),
          self.get<u32>(node.depth)
        );
      }

      return {};
    },
    instruction.ir
  );
}

auto Recorder::command_buffer(this const Recorder& self) -> VkCommandBuffer { return self.execution->command_buffer(); }

auto Recorder::functions(this const Recorder& self) -> const VulkanFunctions& { return self.execution->fn(); }

auto Recorder::render_area(this const Recorder& self) -> VkRect2D { return self.execution->render_area; }

auto Recorder::buffer(this const Recorder& self, ValueID id) -> Buffer { return self.execution->get<Buffer>(id); }

auto Recorder::image(this const Recorder& self, ValueID id) -> ImageAttachment {
  auto result = self.execution->get<ImageAttachment>(id);
  auto combined = Access::Empty;
  const auto& wanted = result.subresource_range;
  for (const auto& [image, access] : self.execution->callback_images) {
    const auto& range = image.subresource_range;
    if (
      image.image.handle == result.image.handle && u64(range.baseMipLevel) + range.levelCount > wanted.baseMipLevel &&
      u64(wanted.baseMipLevel) + wanted.levelCount > range.baseMipLevel &&
      u64(range.baseArrayLayer) + range.layerCount > wanted.baseArrayLayer &&
      u64(wanted.baseArrayLayer) + wanted.layerCount > range.baseArrayLayer
    ) {
      combined |= access;
    }
  }
  if (combined != Access::Empty) {
    result.layout = to_image_layout(combined);
  }
  return result;
}

auto Recorder::transfer_command(this Recorder& self) -> option<CommandBuffer> {
  if (!self.result) {
    return nullopt;
  }
  if (self.execution->is_rendering) {
    OX_LOG_ERROR("transfer commands cannot run inside rendering");
    self.result = std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    return nullopt;
  }
  if (auto ready = self.execution->ensure_batch(); !ready) {
    self.result = ready;
    return nullopt;
  }
  if (std::exchange(self.has_transfer_command, true)) {
    const auto barrier = CommandBuffer::memory_barrier_info(Access::TransferRW, Access::TransferRW);
    self.execution->batch->pipeline_barrier(std::span(&barrier, 1), {});
  }
  return self.execution->batch;
}

auto Recorder::copy_buffer(this Recorder& self, const Buffer& src, const Buffer& dst, const BufferCopy& region)
  -> Recorder& {
  if (const auto command = self.transfer_command(); command.has_value()) {
    self.result = record_copy_buffer(*command, src, dst, region);
  }
  return self;
}

auto Recorder::fill_buffer(this Recorder& self, const Buffer& buffer, u32 data, u64 offset, u64 size) -> Recorder& {
  if (const auto command = self.transfer_command(); command.has_value()) {
    self.result = record_fill_buffer(*command, buffer, data, offset, size);
  }
  return self;
}

auto Recorder::update_buffer(this Recorder& self, const Buffer& buffer, std::span<const u8> data, u64 offset)
  -> Recorder& {
  if (const auto command = self.transfer_command(); command.has_value()) {
    self.result = record_update_buffer(*command, buffer, data, offset);
  }
  return self;
}

auto Recorder::copy_image(
  this Recorder& self, const ImageAttachment& src, const ImageAttachment& dst, option<ImageCopy> region
) -> Recorder& {
  if (const auto command = self.transfer_command(); command.has_value()) {
    self.result = record_copy_image(*command, src, dst, region, false);
  }
  return self;
}

auto Recorder::resolve(
  this Recorder& self, const ImageAttachment& src, const ImageAttachment& dst, option<ImageCopy> region
) -> Recorder& {
  if (const auto command = self.transfer_command(); command.has_value()) {
    self.result = record_copy_image(*command, src, dst, region, true);
  }
  return self;
}

auto Recorder::copy_buffer_to_image(
  this Recorder& self, const Buffer& buffer, const ImageAttachment& image, option<BufferImageCopy> region
) -> Recorder& {
  if (const auto command = self.transfer_command(); command.has_value()) {
    self.result = record_copy_buffer_image(*command, buffer, image, region, true);
  }
  return self;
}

auto Recorder::copy_image_to_buffer(
  this Recorder& self, const ImageAttachment& image, const Buffer& buffer, option<BufferImageCopy> region
) -> Recorder& {
  if (const auto command = self.transfer_command(); command.has_value()) {
    self.result = record_copy_buffer_image(*command, buffer, image, region, false);
  }
  return self;
}

auto Recorder::set_viewport(this Recorder& self, u32 index, const Viewport& viewport) -> Recorder& {
  const auto resolved = viewport.resolve(self.execution->render_area).to_vk();
  self.execution->fn().vkCmdSetViewport(self.command_buffer(), index, 1, &resolved);
  return self;
}

auto Recorder::set_scissor(this Recorder& self, u32 index, const Rect2D& rect) -> Recorder& {
  const auto resolved = rect.resolve(self.execution->render_area);
  self.execution->fn().vkCmdSetScissor(self.command_buffer(), index, 1, &resolved);
  return self;
}

auto Recorder::set_scissor_absolute(this Recorder& self, const VkRect2D& rect) -> Recorder& {
  self.execution->fn().vkCmdSetScissor(self.command_buffer(), 0, 1, &rect);
  return self;
}

auto Recorder::push_constant_bytes(this Recorder& self, u32 offset, std::span<const u8> data) -> Recorder& {
  if (!self.result) {
    return self;
  }

  auto push = PushConstants{};
  push.write(offset, data);
  self.execution->recorded_push = nullopt;
  self.result = self.execution->record_push_constants(self.pipeline, push);
  return self;
}

auto Recorder::write_descriptor(this Recorder& self, ResolvedDescriptor descriptor) -> Recorder& {
  if (!self.result) {
    return self;
  }

  if (auto valid = descriptor.validate(); !valid) {
    self.result = valid;
    return self;
  }

  self.descriptors.insert_or_assign(std::pair{descriptor.set, descriptor.binding}, descriptor);
  return self;
}

auto Recorder::bind_image(this Recorder& self, u32 set, u32 binding, const ImageAttachment& image) -> Recorder& {
  return self.write_descriptor(
    {.set = set, .binding = binding, .kind = ResolvedDescriptor::Kind::Image, .image_view = image.image_view}
  );
}

auto Recorder::bind_texture(this Recorder& self, u32 set, u32 binding, const ImageAttachment& image, VkSampler sampler)
  -> Recorder& {
  return self.write_descriptor({
    .set = set,
    .binding = binding,
    .kind = ResolvedDescriptor::Kind::CombinedImageSampler,
    .sampler = sampler,
    .image_view = image.image_view,
  });
}

auto Recorder::bind_sampler(this Recorder& self, u32 set, u32 binding, VkSampler sampler) -> Recorder& {
  return self.write_descriptor(
    {.set = set, .binding = binding, .kind = ResolvedDescriptor::Kind::Sampler, .sampler = sampler}
  );
}

auto Recorder::bind_buffer(this Recorder& self, u32 set, u32 binding, const Buffer& buffer, u64 offset, u64 range)
  -> Recorder& {
  return self.write_descriptor({
    .set = set,
    .binding = binding,
    .kind = ResolvedDescriptor::Kind::Buffer,
    .buffer = buffer.handle,
    .buffer_size = buffer.size,
    .base_offset = buffer.offset,
    .offset = offset,
    .range = range,
  });
}

auto Recorder::bind_vertex_buffer(this Recorder& self, u32 binding, const Buffer& buffer, u64 offset) -> Recorder& {
  const auto byte_offset = buffer.offset + offset;
  self.execution->fn().vkCmdBindVertexBuffers(self.command_buffer(), binding, 1, &buffer.handle, &byte_offset);
  return self;
}

auto Recorder::bind_index_buffer(this Recorder& self, const Buffer& buffer, VkIndexType index_type, u64 offset)
  -> Recorder& {
  self.execution->fn().vkCmdBindIndexBuffer(self.command_buffer(), buffer.handle, buffer.offset + offset, index_type);
  return self;
}

auto Recorder::bind_descriptors(this Recorder& self) -> std::expected<void, VkResult> {
  if (!is_valid(self.pipeline)) {
    return {};
  }

  // a pipeline can use only the bindless set, with no pass descriptors to resolve
  auto descriptors = std::vector<ResolvedDescriptor>{};
  for (const auto& [key, descriptor] : self.descriptors) {
    descriptors.push_back(descriptor);
  }

  return self.execution->bind_resolved_descriptor_sets(self.pipeline, descriptors);
}

auto Recorder::draw(this Recorder& self, u32 vertex_count, u32 instance_count, u32 first_vertex, u32 first_instance)
  -> Recorder& {
  if (!self.result) {
    return self;
  }

  self.result = self.bind_descriptors();
  if (self.result) {
    self.execution->fn().vkCmdDraw(self.command_buffer(), vertex_count, instance_count, first_vertex, first_instance);
  }

  return self;
}

auto Recorder::dispatch(this Recorder& self, u32 groups_x, u32 groups_y, u32 groups_z) -> Recorder& {
  if (!self.result) {
    return self;
  }

  self.result = self.bind_descriptors();
  if (self.result) {
    self.execution->fn().vkCmdDispatch(self.command_buffer(), groups_x, groups_y, groups_z);
  }

  return self;
}

auto Recorder::draw_indexed(
  this Recorder& self, u32 index_count, u32 instance_count, u32 first_index, i32 vertex_offset, u32 first_instance
) -> Recorder& {
  if (!self.result) {
    return self;
  }

  self.result = self.bind_descriptors();
  if (self.result) {
    self.execution->fn()
      .vkCmdDrawIndexed(self.command_buffer(), index_count, instance_count, first_index, vertex_offset, first_instance);
  }

  return self;
}

auto Recorder::draw_indexed_indirect(this Recorder& self, const Buffer& buffer, u64 offset, u32 draw_count, u32 stride)
  -> Recorder& {
  if (!self.result) {
    return self;
  }

  self.result = self.bind_descriptors();
  if (self.result) {
    self.execution->fn()
      .vkCmdDrawIndexedIndirect(self.command_buffer(), buffer.handle, buffer.offset + offset, draw_count, stride);
  }

  return self;
}

RenderGraph::RenderGraph(Context& ctx, PersistentAllocator& allocator) : context(&ctx), persistent(&allocator) {}

RenderGraph::~RenderGraph() {
  context->wait_idle();
  for (const auto& retired : retired_arenas) {
    destroy_pools(retired.pools);
  }

  const auto& fn = context->fn;
  for (const auto& declared : pipelines) {
    for (const auto& [state, pipeline] : declared->graphics_variants) {
      fn.vkDestroyPipeline(context->device, pipeline, nullptr);
    }

    for (const auto& [specialization, pipeline] : declared->compute_variants) {
      fn.vkDestroyPipeline(context->device, pipeline, nullptr);
    }

    for (const auto& [specialization, variant] : declared->ray_tracing_variants) {
      fn.vkDestroyPipeline(context->device, variant.pipeline, nullptr);
      persistent->deallocate_buffer(variant.shader_binding_table);
    }

    if (declared->layout.has_value()) {
      declared->layout->destroy(*context);
    }
  }
}

static auto with_binding_accesses(ShaderLayout layout) -> ShaderLayout {
  for (auto& binding : layout.bindings) {
    binding.access = descriptor_access(binding.usage, binding.stages);
  }

  return layout;
}

auto RenderGraph::declare_graphics_pipeline(this RenderGraph& self, GraphicsPipelineInfo info)
  -> std::expected<PipelineID, VkResult> {
  if (info.shaders.empty()) {
    OX_LOG_ERROR("graphics pipeline {} declared without any shaders", info.name);
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }

  auto declared = std::make_unique<DeclaredPipeline>();
  for (const auto& shader : info.shaders) {
    declared->reflections.push_back(with_binding_accesses(shader.layout));
  }

  VIR_TRY(validate_descriptor_bindings(declared->reflections, info.bindless));
  declared->kind = PipelineKind::Graphics;
  declared->name = info.name;
  declared->bindings = merged_bindings(declared->reflections);
  declared->bindless = info.bindless;
  declared->vertex = info.vertex_layout.has_value() ? *info.vertex_layout
                                                    : VertexLayout::interleaved(declared->reflections);
  declared->graphics = std::move(info);

  auto lock = std::unique_lock(self.pipelines_mutex);
  const auto id = static_cast<PipelineID>(self.pipelines.size());
  self.pipelines.push_back(std::move(declared));
  return id;
}

auto RenderGraph::declare_compute_pipeline(this RenderGraph& self, ComputePipelineInfo info)
  -> std::expected<PipelineID, VkResult> {
  if (info.shader.spirv.empty()) {
    OX_LOG_ERROR("compute pipeline {} declared without a shader", info.name);
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }

  if (info.shader.layout.stage != ShaderStage::Compute) {
    OX_LOG_ERROR("compute pipeline {} declared with a shader that is not compute", info.name);
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }

  auto declared = std::make_unique<DeclaredPipeline>();
  declared->reflections.push_back(with_binding_accesses(info.shader.layout));
  VIR_TRY(validate_descriptor_bindings(declared->reflections, info.bindless));
  declared->kind = PipelineKind::Compute;
  declared->name = info.name;
  declared->bindings = merged_bindings(declared->reflections);
  declared->bindless = info.bindless;
  declared->compute = std::move(info);

  auto lock = std::unique_lock(self.pipelines_mutex);
  const auto id = static_cast<PipelineID>(self.pipelines.size());
  self.pipelines.push_back(std::move(declared));
  return id;
}

auto RenderGraph::declare_ray_tracing_pipeline(this RenderGraph& self, RayTracingPipelineInfo info)
  -> std::expected<PipelineID, VkResult> {
  if (info.shaders.empty()) {
    OX_LOG_ERROR("ray tracing pipeline {} declared without any shaders", info.name);
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }

  auto declared = std::make_unique<DeclaredPipeline>();
  for (const auto& shader : info.shaders) {
    declared->reflections.push_back(with_binding_accesses(shader.layout));
  }

  VIR_TRY(validate_descriptor_bindings(declared->reflections, info.bindless));
  declared->kind = PipelineKind::RayTracing;
  declared->name = info.name;
  declared->bindings = merged_bindings(declared->reflections);
  declared->bindless = info.bindless;
  declared->groups = info.groups.empty() ? default_shader_groups(declared->reflections) : info.groups;
  VIR_TRY(validate_shader_groups(declared->reflections, declared->groups));
  declared->ray_tracing = std::move(info);

  auto lock = std::unique_lock(self.pipelines_mutex);
  const auto id = static_cast<PipelineID>(self.pipelines.size());
  self.pipelines.push_back(std::move(declared));
  return id;
}

auto RenderGraph::pipeline_bindings(this RenderGraph& self) -> PipelineBindings {
  return {
    .user = &self,
    .bindings_fn = [](const void* user, PipelineID pipeline) -> option<std::span<const DescriptorBinding>> {
      auto* graph = const_cast<RenderGraph*>(static_cast<const RenderGraph*>(user));
      auto lock = std::shared_lock(graph->pipelines_mutex);
      const auto index = static_cast<usize>(std::to_underlying(pipeline));
      if (index >= graph->pipelines.size()) {
        return nullopt;
      }

      // the bindings never change once declared and the declaration outlives the graph's use of it
      return std::span<const DescriptorBinding>(graph->pipelines[index]->bindings);
    },
    .bindless_set_fn = [](const void* user, PipelineID pipeline) -> option<u32> {
      auto* graph = const_cast<RenderGraph*>(static_cast<const RenderGraph*>(user));
      auto lock = std::shared_lock(graph->pipelines_mutex);
      const auto index = static_cast<usize>(std::to_underlying(pipeline));
      if (index >= graph->pipelines.size() || !graph->pipelines[index]->bindless.has_value()) {
        return nullopt;
      }

      return graph->pipelines[index]->bindless->index;
    },
  };
}

auto RenderGraph::local_size(this RenderGraph& self, PipelineID pipeline) -> std::array<u32, 3> {
  auto lock = std::shared_lock(self.pipelines_mutex);
  const auto index = static_cast<usize>(std::to_underlying(pipeline));
  if (index >= self.pipelines.size() || self.pipelines[index]->reflections.empty()) {
    return {1, 1, 1};
  }

  return self.pipelines[index]->reflections.front().local_size;
}

auto shader_binding_table_layout(const RayTracingProperties& properties, std::span<const u32, 4> counts)
  -> std::expected<ShaderBindingTableLayout, VkResult> {
  if (
    counts[0] != 1 || properties.shader_group_handle_size == 0 || properties.shader_group_handle_alignment == 0 ||
    properties.shader_group_base_alignment == 0 || properties.max_shader_group_stride == 0
  ) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }
  const auto stride = align_up_checked(properties.shader_group_handle_size, properties.shader_group_handle_alignment);
  if (!stride.has_value() || *stride > properties.max_shader_group_stride) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }
  auto layout = ShaderBindingTableLayout{};
  for (usize index = 0; index < counts.size(); index++) {
    if (counts[index] == 0) {
      continue;
    }
    const auto offset = align_up_checked(layout.size, properties.shader_group_base_alignment);
    const auto bytes = u64(counts[index]) * *stride;
    if (!offset.has_value() || bytes > ~0_u64 - *offset) {
      return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }
    layout.regions[index] = {.offset = *offset, .stride = *stride, .size = bytes};
    layout.size = *offset + bytes;
  }
  return layout;
}

static auto build_shader_binding_table(
  Context& context, PersistentAllocator& allocator, const DeclaredPipeline& declared, VkPipeline pipeline
) -> std::expected<RayTracingVariant, VkResult> {
  const auto& properties = context.ray_tracing;
  const auto handle_size = properties.shader_group_handle_size;
  const auto group_count = static_cast<u32>(declared.groups.size());
  auto stack = memory::ScopedStack{};
  auto handles = stack.alloc<u8>(static_cast<usize>(handle_size) * group_count);
  auto groups_by_region = std::array<std::span<u32>, 4>{};
  for (auto& indices : groups_by_region) {
    indices = stack.alloc<u32>(group_count);
  }
  auto counts = std::array<u32, 4>{};
  for (u32 index = 0; index < group_count; index++) {
    const auto& group = declared.groups[index];
    auto region = 2_sz;
    if (group.type == VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR) {
      switch (declared.reflections[group.general].stage) {
        case ShaderStage::RayGeneration: region = 0; break;
        case ShaderStage::Miss         : region = 1; break;
        case ShaderStage::Callable     : region = 3; break;
        default                        : break;
      }
    }

    groups_by_region[region][counts[region]++] = index;
  }

  for (usize index = 0; index < counts.size(); index++) {
    groups_by_region[index] = groups_by_region[index].first(counts[index]);
  }
  const auto layout = shader_binding_table_layout(properties, counts);
  if (!layout) {
    return std::unexpected(layout.error());
  }
  if (!context.fn.vkGetRayTracingShaderGroupHandlesKHR) {
    return std::unexpected(VK_ERROR_EXTENSION_NOT_PRESENT);
  }
  VIR_CHECK(
    context.fn
      .vkGetRayTracingShaderGroupHandlesKHR(context.device, pipeline, 0, group_count, handles.size(), handles.data())
  );

  auto buffer = allocator.allocate_buffer({
    .size = layout->size,
    .usage = VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    .location = MemoryLocation::CpuToGpu,
    .alignment = properties.shader_group_base_alignment,
    .name = declared.name + " shader binding table",
  });
  if (!buffer) {
    return std::unexpected(buffer.error());
  }
  if (buffer->mapped == nullptr || buffer->device_address == 0) {
    allocator.deallocate_buffer(*buffer);
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }
  std::memset(buffer->mapped, 0, layout->size);

  for (usize region = 0; region < 4; region++) {
    const auto& placed = layout->regions[region];
    for (usize slot = 0; slot < groups_by_region[region].size(); slot++) {
      const auto group = groups_by_region[region][slot];
      std::memcpy(
        buffer->mapped + placed.offset + slot * placed.stride,
        handles.data() + group * handle_size,
        handle_size
      );
    }
  }
  const auto flushed = allocator.flush_buffer(*buffer);
  if (!flushed) {
    allocator.deallocate_buffer(*buffer);
    return std::unexpected(flushed.error());
  }

  const auto region_of = [&](usize region) -> VkStridedDeviceAddressRegionKHR {
    if (groups_by_region[region].empty()) {
      return {};
    }

    const auto& placed = layout->regions[region];
    return {
      .deviceAddress = buffer->device_address + placed.offset,
      .stride = placed.stride,
      .size = placed.size,
    };
  };

  return RayTracingVariant{
    .pipeline = pipeline,
    .shader_binding_table = *buffer,
    .raygen = region_of(0),
    .miss = region_of(1),
    .hit = region_of(2),
    .callable = region_of(3),
  };
}

auto RenderGraph::construct_pipelines(this RenderGraph& self, std::span<const Instr> instructions)
  -> std::expected<void, VkResult> {
  ZoneScoped;

  auto graphics = std::vector<std::pair<PipelineID, const PipelineState*>>{};
  auto compute = std::vector<std::pair<PipelineID, const Specialization*>>{};
  auto ray_tracing = std::vector<std::pair<PipelineID, const Specialization*>>{};

  {
    auto lock = std::shared_lock(self.pipelines_mutex);
    for (const auto& instruction : instructions) {
      const auto* pipeline = pipeline_of(instruction.ir);
      if (!pipeline) {
        continue;
      }

      if (!is_valid(*pipeline)) {
        if (std::holds_alternative<ir::CallOpaque>(instruction.ir)) {
          continue;
        }

        OX_LOG_ERROR("%{} runs no pipeline", std::to_underlying(instruction.id));
        return std::unexpected(VK_ERROR_UNKNOWN);
      }

      const auto index = static_cast<usize>(std::to_underlying(*pipeline));
      if (index >= self.pipelines.size()) {
        OX_LOG_ERROR("pipeline {} was never declared", index);
        return std::unexpected(VK_ERROR_UNKNOWN);
      }

      const auto& declared = *self.pipelines[index];
      std::visit(
        [&](const auto& node) {
          using T = std::decay_t<decltype(node)>;
          if constexpr (std::is_same_v<T, ir::Dispatch> || std::is_same_v<T, ir::TraceRays>) {
            if (declared.kind == PipelineKind::Compute && !declared.compute_variants.contains(node.specialization)) {
              compute.emplace_back(*pipeline, &node.specialization);
            } else if (
              declared.kind == PipelineKind::RayTracing && !declared.ray_tracing_variants.contains(node.specialization)
            ) {
              ray_tracing.emplace_back(*pipeline, &node.specialization);
            }
          } else if constexpr (requires { node.state; }) {
            if (declared.kind == PipelineKind::Graphics && !declared.graphics_variants.contains(node.state)) {
              graphics.emplace_back(*pipeline, &node.state);
            } else if (
              declared.kind == PipelineKind::Compute && !declared.compute_variants.contains(node.state.specialization)
            ) {
              compute.emplace_back(*pipeline, &node.state.specialization);
            }
          }
        },
        instruction.ir
      );
    }
  }

  if (graphics.empty() && compute.empty() && ray_tracing.empty()) {
    return {};
  }

  auto lock = std::unique_lock(self.pipelines_mutex);
  const auto ensure_layout = [&](DeclaredPipeline& declared) -> std::expected<void, VkResult> {
    if (declared.layout.has_value()) {
      return {};
    }

    auto layout = PipelineLayout::create(
      *self.context,
      declared.reflections,
      declared.bindless,
      self.context->max_push_descriptors
    );
    if (!layout) {
      return std::unexpected(layout.error());
    }

    declared.layout = std::move(*layout);
    return {};
  };

  auto graphics_requests = std::vector<GraphicsPipelineRequest>{};
  auto graphics_keys = std::vector<std::pair<DeclaredPipeline*, const PipelineState*>>{};
  for (const auto& [id, state] : graphics) {
    auto& declared = *self.pipelines[static_cast<usize>(std::to_underlying(id))];
    // a second request for the same variant may have been queued, or made by another thread
    if (declared.graphics_variants.contains(*state) || std::ranges::any_of(graphics_keys, [&](const auto& key) {
          return key.first == &declared && *key.second == *state;
        })) {
      continue;
    }

    VIR_TRY(ensure_layout(declared));
    graphics_requests.push_back({
      .info = &declared.graphics,
      .reflections = declared.reflections,
      .vertex = &declared.vertex,
      .state = state,
      .layout = declared.layout->handle,
    });
    graphics_keys.emplace_back(&declared, state);
  }

  if (!graphics_requests.empty()) {
    OX_LOG_DEBUG("compiling {} graphics pipeline variants", graphics_requests.size());
    auto handles = create_graphics_pipelines(*self.context, graphics_requests);
    if (!handles) {
      return std::unexpected(handles.error());
    }

    for (usize index = 0; index < graphics_keys.size(); index++) {
      graphics_keys[index].first->graphics_variants.emplace(*graphics_keys[index].second, (*handles)[index]);
    }
  }

  auto compute_requests = std::vector<ComputePipelineRequest>{};
  auto compute_keys = std::vector<std::pair<DeclaredPipeline*, const Specialization*>>{};
  for (const auto& [id, specialization] : compute) {
    auto& declared = *self.pipelines[static_cast<usize>(std::to_underlying(id))];
    if (declared.compute_variants.contains(*specialization) || std::ranges::any_of(compute_keys, [&](const auto& key) {
          return key.first == &declared && *key.second == *specialization;
        })) {
      continue;
    }

    VIR_TRY(ensure_layout(declared));
    compute_requests.push_back({
      .info = &declared.compute,
      .reflection = &declared.reflections.front(),
      .specialization = specialization,
      .layout = declared.layout->handle,
    });
    compute_keys.emplace_back(&declared, specialization);
  }

  if (!compute_requests.empty()) {
    OX_LOG_DEBUG("compiling {} compute pipeline variants", compute_requests.size());
    auto handles = create_compute_pipelines(*self.context, compute_requests);
    if (!handles) {
      return std::unexpected(handles.error());
    }

    for (usize index = 0; index < compute_keys.size(); index++) {
      compute_keys[index].first->compute_variants.emplace(*compute_keys[index].second, (*handles)[index]);
    }
  }

  for (const auto& [id, specialization] : ray_tracing) {
    auto& declared = *self.pipelines[static_cast<usize>(std::to_underlying(id))];
    if (declared.ray_tracing_variants.contains(*specialization)) {
      continue;
    }

    VIR_TRY(ensure_layout(declared));
    auto pipeline = create_ray_tracing_pipeline(
      *self.context,
      declared.ray_tracing,
      declared.reflections,
      declared.groups,
      *specialization,
      declared.layout->handle
    );
    if (!pipeline) {
      return std::unexpected(pipeline.error());
    }

    auto variant = build_shader_binding_table(*self.context, *self.persistent, declared, *pipeline);
    if (!variant) {
      self.context->fn.vkDestroyPipeline(self.context->device, *pipeline, nullptr);
      return std::unexpected(variant.error());
    }

    declared.ray_tracing_variants.emplace(*specialization, *variant);
  }

  return {};
}

auto RenderGraph::destroy_pools(this RenderGraph& self, std::span<const VkDescriptorPool> pools) -> void {
  for (auto pool : pools) {
    self.context->fn.vkDestroyDescriptorPool(self.context->device, pool, nullptr);
  }
}

auto RenderGraph::collect_descriptor_arenas(this RenderGraph& self) -> void {
  auto lock = std::unique_lock(self.retired_mutex);
  std::erase_if(self.retired_arenas, [&](const RetiredDescriptorArena& retired) {
    if (!self.context->is_complete(retired.ticket)) {
      return false;
    }

    self.destroy_pools(retired.pools);
    return true;
  });
}

auto RenderGraph::execute(this RenderGraph& self, const Program& program, FrameAllocator& allocator)
  -> std::expected<SubmitTicket, VkResult> {
  ZoneScoped;

  auto* queue = self.context->queue(DomainFlag::Graphics);
  if (!queue) {
    return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
  }
  // covers recording and submission, query writers also retire before another submit starts
  auto queue_lock = std::unique_lock(queue->submit_mutex);
  VIR_TRY(wait_for_query_writes(*self.context, *queue, queue_lock));
  self.collect_descriptor_arenas();
  VIR_TRY(self.construct_pipelines(program.instructions));

  auto lock = std::shared_lock(self.pipelines_mutex);
  auto execution = Execution{.graph = &self, .context = self.context, .program = &program, .allocator = &allocator};
  VIR_TRY(execution.reset());
  execution.prepare_descriptor_pools();

  const auto retire = [&](SubmitTicket ticket) {
    if (execution.pools.empty()) {
      return;
    }

    auto retired_lock = std::unique_lock(self.retired_mutex);
    self.retired_arenas.push_back({.pools = std::move(execution.pools), .ticket = ticket});
  };

  auto block = option<LabelID>{};
  auto arrived_from = option<LabelID>{};
  const auto& instructions = program.instructions;
  for (usize pc = 0; pc < instructions.size();) {
    const auto& instruction = instructions[pc];

    if (is_barrier(instruction.ir)) {
      const auto start = pc;
      const auto memory_barrier = std::holds_alternative<ir::MemoryBarrier>(instruction.ir);
      while (pc < instructions.size() && instructions[pc].ir.index() == instruction.ir.index()) {
        pc++;
      }
      if (auto ready = execution.ensure_batch(); !ready) {
        retire({});
        return std::unexpected(ready.error());
      }
      const auto entries = std::span(instructions).subspan(start, pc - start);
      if (memory_barrier) {
        execution.record_memory_barriers(entries);
      } else {
        execution.record_image_barriers(entries);
      }
      continue;
    } else if (const auto* label = std::get_if<ir::Label>(&instruction.ir)) {
      block = label->label;
    } else if (const auto* branch = std::get_if<ir::Branch>(&instruction.ir)) {
      arrived_from = block;
      const auto target = program.label_index(branch->target);
      if (!target.has_value()) {
        retire({});
        return std::unexpected(VK_ERROR_UNKNOWN);
      }

      pc = *target;
      continue;
    } else if (const auto* conditional = std::get_if<ir::BranchConditional>(&instruction.ir)) {
      const auto taken = execution.get<bool>(conditional->condition) ? conditional->true_label
                                                                     : conditional->false_label;
      arrived_from = block;
      const auto target = program.label_index(taken);
      if (!target.has_value()) {
        retire({});
        return std::unexpected(VK_ERROR_UNKNOWN);
      }

      pc = *target;
      continue;
    } else if (std::holds_alternative<ir::Return>(instruction.ir)) {
      break;
    } else if (const auto* phi = std::get_if<ir::Phi>(&instruction.ir)) {
      const auto chosen = std::ranges::find_if(phi->incoming, [&](const auto& entry) {
        return arrived_from == entry.second;
      });
      if (chosen == phi->incoming.end()) {
        retire({});
        return std::unexpected(VK_ERROR_UNKNOWN);
      }

      execution.forward(instruction.id, chosen->first);
    } else if (auto executed = execution.execute_one(instruction); !executed) {
      retire({});
      return std::unexpected(executed.error());
    }

    pc += 1;
  }

  if (
    execution.batch.has_value() || !execution.current_submit.command_buffers.empty() ||
    !execution.current_submit.waits.empty()
  ) {
    if (auto flushed = execution.flush_submit(nullopt); !flushed) {
      retire({});
      return std::unexpected(flushed.error());
    }
  }

  auto ticket = SubmitTicket{};
  {
    for (const auto& submit : execution.submits) {
      if (queue->pending_query_retirement != 0) {
        // another waiter can resume and construct pipelines while the queue lock is released
        lock.unlock();
        const auto waited = wait_for_query_writes(*self.context, *queue, queue_lock);
        lock.lock();
        if (!waited) {
          allocator.add_timeline_wait(ticket);
          retire(ticket);
          return std::unexpected(waited.error());
        }
      }
      auto stack = memory::ScopedStack{};
      auto waits = stack.alloc<VkSemaphoreSubmitInfo>(submit.waits.size());
      auto command_buffers = stack.alloc<VkCommandBufferSubmitInfo>(submit.command_buffers.size());
      auto signals = stack.alloc<VkSemaphoreSubmitInfo>(submit.signals.size() + 1);
      const auto to_info = [](const SemaphoreSubmit& semaphore) {
        return VkSemaphoreSubmitInfo{
          .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
          .pNext = nullptr,
          .semaphore = semaphore.semaphore,
          .value = semaphore.value,
          .stageMask = to_stage_flags(semaphore.access),
          .deviceIndex = 0,
        };
      };

      for (usize index = 0; index < submit.waits.size(); index++) {
        waits[index] = to_info(submit.waits[index]);
      }

      for (usize index = 0; index < submit.command_buffers.size(); index++) {
        command_buffers[index] = {
          .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
          .pNext = nullptr,
          .commandBuffer = submit.command_buffers[index],
          .deviceMask = 0,
        };
      }

      for (usize index = 0; index < submit.signals.size(); index++) {
        signals[index] = to_info(submit.signals[index]);
      }

      queue->timeline_value += 1;
      const auto submitted = SubmitTicket{
        .queue = self.context->queue_id(DomainFlag::Graphics),
        .value = queue->timeline_value
      };
      signals.back() = to_info({.semaphore = queue->timeline, .value = submitted.value, .access = Access::MemoryRW});

      const auto submit_info = VkSubmitInfo2{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .pNext = nullptr,
        .flags = 0,
        .waitSemaphoreInfoCount = static_cast<u32>(waits.size()),
        .pWaitSemaphoreInfos = waits.data(),
        .commandBufferInfoCount = static_cast<u32>(command_buffers.size()),
        .pCommandBufferInfos = command_buffers.data(),
        .signalSemaphoreInfoCount = static_cast<u32>(signals.size()),
        .pSignalSemaphoreInfos = signals.data(),
      };
      if (
        const auto result = self.context->fn.vkQueueSubmit2(queue->handle, 1, &submit_info, VK_NULL_HANDLE);
        result != VK_SUCCESS
      ) {
        // the timeline value is spent, nothing signals it now, so later tickets would never complete
        queue->timeline_value -= 1;
        allocator.add_timeline_wait(ticket);
        retire(ticket);
        return std::unexpected(result);
      }
      ticket = submitted;
      if (submit.has_query_writes) {
        // gpu validation retires queries with command buffer -> queue locks, while a later submit
        // takes queue -> command buffer locks, so the next submit waits outside our queue lock
        queue->pending_query_retirement = ticket.value;
      }
    }

    allocator.add_timeline_wait(ticket);
    retire(ticket);

    auto present_result = VK_SUCCESS;
    for (const auto& present : execution.presents) {
      const auto present_info = VkPresentInfoKHR{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = nullptr,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &present.semaphore,
        .swapchainCount = 1,
        .pSwapchains = &present.swapchain,
        .pImageIndices = &present.image_index,
        .pResults = nullptr,
      };
      const auto result = self.context->fn.vkQueuePresentKHR(queue->handle, &present_info);
      if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        present_result = result;
      }
    }

    if (present_result != VK_SUCCESS) {
      return std::unexpected(present_result);
    }
  }

  return ticket;
}

auto RenderGraph::execute_blocking(this RenderGraph& self, const Program& program) -> std::expected<void, VkResult> {
  // the allocator's teardown waits for the work and frees everything it handed out
  auto allocator = FrameAllocator(*self.persistent);
  auto ticket = self.execute(program, allocator);
  if (!ticket) {
    return std::unexpected(ticket.error());
  }

  return self.context->wait(*ticket);
}
} // namespace ox::vir
