#include <array>
#include <atomic>
#include <barrier>
#include <cstring>
#include <thread>
#include <utility>

#include "Render/Graph/RenderGraph.hpp"
#include "Render/Graph/Transfer.hpp"
#include "Render/Graph/Types.hpp"
#include "RenderGraphTest.hpp"

namespace ox {
struct GraphCapture {
  CommandQueue* queue = nullptr;
  struct Pool {
    VkCommandPool handle;
    std::thread::id thread;
    u32 family;
  };
  std::vector<Pool> pools = {};
  std::vector<VkCommandPool> allocated_from = {};
  std::vector<VkCommandPool> destroyed_pools = {};
  std::vector<VkMemoryBarrier2> memory = {};
  std::vector<VkImageMemoryBarrier2> images = {};
  std::vector<std::pair<u32, u32>> dependency_calls = {};
  std::vector<u64> signals = {};
  std::vector<u64> waits = {};
  std::vector<usize> waits_before_submits = {};
  std::barrier<>* query_waiters = nullptr;
  std::atomic<u32> query_wait_count = 0;
  std::atomic<bool> query_retired = false;
  std::vector<AccelerationStructureBuild> builds = {};
  std::vector<VkImageCopy> copies = {};
  std::vector<VkImageResolve> resolves = {};
  std::vector<std::pair<VkImageLayout, VkImageLayout>> transfer_layouts = {};
  std::vector<VkBufferImageCopy> uploads = {};
  std::vector<VkBufferImageCopy> readbacks = {};
  std::vector<VkBufferCopy> buffer_copies = {};
  std::vector<std::pair<u64, Bytes>> updates = {};
  std::vector<std::pair<u64, u64>> fills = {};
  std::vector<VkCopyAccelerationStructureInfoKHR> structure_copies = {};
  std::vector<QueryPool> created_queries = {};
  std::vector<VkQueryPool> destroyed_queries = {};
  std::vector<VkQueryPool> written_queries = {};
  std::vector<std::pair<u32, u32>> reset_queries = {};
  struct DescriptorSet {
    VkPipelineBindPoint bind_point;
    VkPipelineLayout layout;
    u32 index;
    VkDescriptorSet handle;
  };
  std::vector<DescriptorSet> descriptor_sets = {};
  std::vector<usize> draws_after_descriptor_bindings = {};
  struct DescriptorBuffer {
    u32 set;
    u32 binding;
    VkDescriptorType type;
    VkDescriptorBufferInfo info;
  };
  std::vector<DescriptorBuffer> descriptor_buffers = {};
  std::vector<usize> dispatches_after_descriptor_bindings = {};
  struct Push {
    VkShaderStageFlags stages;
    u32 offset;
    Bytes bytes;
  };
  std::vector<Push> pushes = {};
  struct IndexBinding {
    VkBuffer buffer;
    u64 offset;
    VkIndexType type;
  };
  std::vector<IndexBinding> index_bindings = {};
  std::vector<VkAttachmentLoadOp> color_loads = {};
  std::vector<VkAttachmentLoadOp> depth_loads = {};
  u32 layer_count = 0;
  u32 view_mask = 0;
  u32 color_count = 0;
  u32 rendering_ends = 0;
  u32 submit_attempts = 0;
  u32 fail_submit_at = 0;
  std::atomic<u32> recording = 0;
  std::atomic<u32> max_recording = 0;
};

template <typename T>
static auto graph_capture(T handle) -> GraphCapture& {
  return *reinterpret_cast<GraphCapture*>(handle);
}

template <typename T>
static auto graph_handle(u64 number) -> T {
  return reinterpret_cast<T>(number);
}

static auto stub_graph_context(GraphCapture& capture) -> std::unique_ptr<Context> {
  auto context = std::make_unique<Context>();
  context->device = reinterpret_cast<VkDevice>(&capture);
  auto queue = std::make_unique<CommandQueue>();
  queue->handle = reinterpret_cast<VkQueue>(&capture);
  queue->domain = DomainFlag::Graphics;
  queue->family_index = 3;
  queue->timeline = graph_handle<VkSemaphore>(1);
  context->queues.push_back(std::move(queue));
  capture.queue = context->queues.back().get();
  auto& fn = context->fn;
  fn.vkDeviceWaitIdle = [](VkDevice) -> VkResult {
    return VK_SUCCESS;
  };
  fn.vkDestroySemaphore = [](VkDevice, VkSemaphore, const VkAllocationCallbacks*) -> void {
  };
  fn.vkWaitSemaphores = [](VkDevice device, const VkSemaphoreWaitInfo* info, u64) -> VkResult {
    auto& captured = graph_capture(device);
    captured.waits.push_back(info->pValues[0]);
    return VK_SUCCESS;
  };
  fn.vkGetSemaphoreCounterValue = [](VkDevice device, VkSemaphore, u64* value) -> VkResult {
    const auto& captured = graph_capture(device);
    *value = captured.signals.empty() ? 0 : captured.signals.back();
    return VK_SUCCESS;
  };
  fn.vkCreateCommandPool =
    [](VkDevice device, const VkCommandPoolCreateInfo* info, const VkAllocationCallbacks*, VkCommandPool* pool)
    -> VkResult {
    auto& captured = graph_capture(device);
    *pool = graph_handle<VkCommandPool>(captured.pools.size() + 10);
    captured.pools.push_back({*pool, std::this_thread::get_id(), info->queueFamilyIndex});
    return VK_SUCCESS;
  };
  fn.vkDestroyCommandPool = [](VkDevice device, VkCommandPool pool, const VkAllocationCallbacks*) -> void {
    graph_capture(device).destroyed_pools.push_back(pool);
  };
  fn.vkResetCommandPool = [](VkDevice, VkCommandPool, VkCommandPoolResetFlags) -> VkResult {
    return VK_SUCCESS;
  };
  fn.vkFreeCommandBuffers = [](VkDevice, VkCommandPool, u32, const VkCommandBuffer*) -> void {
  };
  fn.vkAllocateCommandBuffers =
    [](VkDevice device, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* command) -> VkResult {
    graph_capture(device).allocated_from.push_back(info->commandPool);
    *command = reinterpret_cast<VkCommandBuffer>(device);
    return VK_SUCCESS;
  };
  fn.vkBeginCommandBuffer = [](VkCommandBuffer command, const VkCommandBufferBeginInfo*) -> VkResult {
    auto& captured = graph_capture(command);
    const auto active = captured.recording.fetch_add(1) + 1;
    captured.max_recording.store(std::max(active, captured.max_recording.load()));
    std::this_thread::yield();
    return VK_SUCCESS;
  };
  fn.vkEndCommandBuffer = [](VkCommandBuffer command) -> VkResult {
    graph_capture(command).recording.fetch_sub(1);
    return VK_SUCCESS;
  };
  fn.vkQueueSubmit2 = [](VkQueue submitted, u32, const VkSubmitInfo2* info, VkFence) -> VkResult {
    auto& captured = graph_capture(submitted);
    captured.submit_attempts += 1;
    if (captured.fail_submit_at == captured.submit_attempts) {
      return VK_ERROR_DEVICE_LOST;
    }
    captured.signals.push_back(info->pSignalSemaphoreInfos[info->signalSemaphoreInfoCount - 1].value);
    captured.waits_before_submits.push_back(captured.waits.size());
    return VK_SUCCESS;
  };
  fn.vkCreateImageView =
    [](VkDevice, const VkImageViewCreateInfo* info, const VkAllocationCallbacks*, VkImageView* view) -> VkResult {
    *view = graph_handle<VkImageView>(reinterpret_cast<u64>(info->image) + info->subresourceRange.baseMipLevel + 100);
    return VK_SUCCESS;
  };
  fn.vkDestroyImageView = [](VkDevice, VkImageView, const VkAllocationCallbacks*) -> void {
  };
  fn.vkCmdPipelineBarrier2 = [](VkCommandBuffer command, const VkDependencyInfo* info) -> void {
    auto& captured = graph_capture(command);
    captured.dependency_calls.emplace_back(info->memoryBarrierCount, info->imageMemoryBarrierCount);
    for (u32 index = 0; index < info->memoryBarrierCount; index++) {
      captured.memory.push_back(info->pMemoryBarriers[index]);
    }
    for (u32 index = 0; index < info->imageMemoryBarrierCount; index++) {
      captured.images.push_back(info->pImageMemoryBarriers[index]);
    }
  };
  fn.vkCmdUpdateBuffer = [](VkCommandBuffer command, VkBuffer, u64 offset, u64 size, const void* data) -> void {
    const auto* bytes = static_cast<const u8*>(data);
    graph_capture(command).updates.emplace_back(offset, Bytes{.data = {bytes, bytes + size}});
  };
  fn.vkCmdFillBuffer = [](VkCommandBuffer command, VkBuffer, u64 offset, u64 size, u32) -> void {
    graph_capture(command).fills.emplace_back(offset, size);
  };
  fn.vkCmdCopyBuffer = [](VkCommandBuffer command, VkBuffer, VkBuffer, u32, const VkBufferCopy* copied) -> void {
    graph_capture(command).buffer_copies.push_back(*copied);
  };
  fn.vkCmdCopyImage = [](
                        VkCommandBuffer command,
                        VkImage,
                        VkImageLayout src_layout,
                        VkImage,
                        VkImageLayout dst_layout,
                        u32,
                        const VkImageCopy* copied
                      ) -> void {
    graph_capture(command).copies.push_back(*copied);
    graph_capture(command).transfer_layouts.emplace_back(src_layout, dst_layout);
  };
  fn.vkCmdResolveImage = [](
                           VkCommandBuffer command,
                           VkImage,
                           VkImageLayout src_layout,
                           VkImage,
                           VkImageLayout dst_layout,
                           u32,
                           const VkImageResolve* resolved
                         ) -> void {
    graph_capture(command).resolves.push_back(*resolved);
    graph_capture(command).transfer_layouts.emplace_back(src_layout, dst_layout);
  };
  fn.vkCmdCopyBufferToImage =
    [](VkCommandBuffer command, VkBuffer, VkImage, VkImageLayout, u32, const VkBufferImageCopy* copied) -> void {
    graph_capture(command).uploads.push_back(*copied);
  };
  fn.vkCmdCopyImageToBuffer =
    [](VkCommandBuffer command, VkImage, VkImageLayout, VkBuffer, u32, const VkBufferImageCopy* copied) -> void {
    graph_capture(command).readbacks.push_back(*copied);
  };
  fn.vkCmdBeginRendering = [](VkCommandBuffer command, const VkRenderingInfo* info) -> void {
    auto& captured = graph_capture(command);
    captured.layer_count = info->layerCount;
    captured.view_mask = info->viewMask;
    captured.color_count = info->colorAttachmentCount;
    for (u32 index = 0; index < info->colorAttachmentCount; index++) {
      captured.color_loads.push_back(info->pColorAttachments[index].loadOp);
    }
    if (info->pDepthAttachment) {
      captured.depth_loads.push_back(info->pDepthAttachment->loadOp);
    }
  };
  fn.vkCmdEndRendering = [](VkCommandBuffer command) -> void {
    graph_capture(command).rendering_ends += 1;
  };
  fn.vkCreateDescriptorSetLayout =
    [](VkDevice, const VkDescriptorSetLayoutCreateInfo*, const VkAllocationCallbacks*, VkDescriptorSetLayout* layout)
    -> VkResult {
    *layout = graph_handle<VkDescriptorSetLayout>(100);
    return VK_SUCCESS;
  };
  fn.vkDestroyDescriptorSetLayout = [](VkDevice, VkDescriptorSetLayout, const VkAllocationCallbacks*) -> void {
  };
  fn.vkCreatePipelineLayout =
    [](VkDevice, const VkPipelineLayoutCreateInfo*, const VkAllocationCallbacks*, VkPipelineLayout* layout)
    -> VkResult {
    *layout = graph_handle<VkPipelineLayout>(101);
    return VK_SUCCESS;
  };
  fn.vkDestroyPipelineLayout = [](VkDevice, VkPipelineLayout, const VkAllocationCallbacks*) -> void {
  };
  fn.vkCreateShaderModule =
    [](VkDevice, const VkShaderModuleCreateInfo*, const VkAllocationCallbacks*, VkShaderModule* shader) -> VkResult {
    *shader = graph_handle<VkShaderModule>(102);
    return VK_SUCCESS;
  };
  fn.vkDestroyShaderModule = [](VkDevice, VkShaderModule, const VkAllocationCallbacks*) -> void {
  };
  fn.vkCreateGraphicsPipelines = [](
                                   VkDevice,
                                   VkPipelineCache,
                                   u32 count,
                                   const VkGraphicsPipelineCreateInfo*,
                                   const VkAllocationCallbacks*,
                                   VkPipeline* pipelines
                                 ) -> VkResult {
    for (u32 index = 0; index < count; index++) {
      pipelines[index] = graph_handle<VkPipeline>(103 + index);
    }
    return VK_SUCCESS;
  };
  fn.vkCreateComputePipelines = [](
                                  VkDevice,
                                  VkPipelineCache,
                                  u32 count,
                                  const VkComputePipelineCreateInfo*,
                                  const VkAllocationCallbacks*,
                                  VkPipeline* pipelines
                                ) -> VkResult {
    for (u32 index = 0; index < count; index++) {
      pipelines[index] = graph_handle<VkPipeline>(103 + index);
    }
    return VK_SUCCESS;
  };
  fn.vkDestroyPipeline = [](VkDevice, VkPipeline, const VkAllocationCallbacks*) -> void {
  };
  fn.vkCmdBindPipeline = [](VkCommandBuffer, VkPipelineBindPoint, VkPipeline) -> void {
  };
  fn.vkCmdSetViewport = [](VkCommandBuffer, u32, u32, const VkViewport*) -> void {
  };
  fn.vkCmdSetScissor = [](VkCommandBuffer, u32, u32, const VkRect2D*) -> void {
  };
  fn.vkCmdBindIndexBuffer = [](VkCommandBuffer command, VkBuffer buffer, u64 offset, VkIndexType type) -> void {
    graph_capture(command).index_bindings.push_back({buffer, offset, type});
  };
  fn.vkCmdPushConstants =
    [](VkCommandBuffer command, VkPipelineLayout, VkShaderStageFlags stages, u32 offset, u32 size, const void* data)
    -> void {
    const auto* bytes = static_cast<const u8*>(data);
    graph_capture(command).pushes.push_back({stages, offset, {.data = {bytes, bytes + size}}});
  };
  fn.vkCmdBindDescriptorSets = [](
                                 VkCommandBuffer command,
                                 VkPipelineBindPoint bind_point,
                                 VkPipelineLayout layout,
                                 u32 first_set,
                                 u32 count,
                                 const VkDescriptorSet* sets,
                                 u32,
                                 const u32*
                               ) -> void {
    auto& captured = graph_capture(command);
    for (u32 index = 0; index < count; index++) {
      captured.descriptor_sets.push_back({bind_point, layout, first_set + index, sets[index]});
    }
  };
  fn.vkCmdPushDescriptorSetKHR = [](
                                   VkCommandBuffer command,
                                   VkPipelineBindPoint bind_point,
                                   VkPipelineLayout,
                                   u32 set,
                                   u32 count,
                                   const VkWriteDescriptorSet* writes
                                 ) -> void {
    EXPECT_EQ(bind_point, VK_PIPELINE_BIND_POINT_COMPUTE);
    auto& captured = graph_capture(command);
    for (u32 index = 0; index < count; index++) {
      captured.descriptor_buffers.push_back(
        {set, writes[index].dstBinding, writes[index].descriptorType, *writes[index].pBufferInfo}
      );
    }
  };
  fn.vkCmdDispatch = [](VkCommandBuffer command, u32, u32, u32) -> void {
    auto& captured = graph_capture(command);
    captured.dispatches_after_descriptor_bindings.push_back(captured.descriptor_buffers.size());
  };
  fn.vkCmdDraw = [](VkCommandBuffer command, u32, u32, u32, u32) -> void {
    auto& captured = graph_capture(command);
    captured.draws_after_descriptor_bindings.push_back(captured.descriptor_sets.size());
  };
  fn.vkCmdDrawIndexed = [](VkCommandBuffer command, u32, u32, u32, i32, u32) -> void {
    auto& captured = graph_capture(command);
    captured.draws_after_descriptor_bindings.push_back(captured.descriptor_sets.size());
  };
  fn.vkCmdDrawIndexedIndirect = [](VkCommandBuffer command, VkBuffer, u64, u32, u32) -> void {
    auto& captured = graph_capture(command);
    captured.draws_after_descriptor_bindings.push_back(captured.descriptor_sets.size());
  };
  fn.vkCreateQueryPool =
    [](VkDevice device, const VkQueryPoolCreateInfo* info, const VkAllocationCallbacks*, VkQueryPool* queries)
    -> VkResult {
    auto& captured = graph_capture(device);
    *queries = graph_handle<VkQueryPool>(captured.created_queries.size() + 50);
    captured.created_queries.push_back({*queries, info->queryType, info->queryCount});
    return VK_SUCCESS;
  };
  fn.vkDestroyQueryPool = [](VkDevice device, VkQueryPool queries, const VkAllocationCallbacks*) -> void {
    graph_capture(device).destroyed_queries.push_back(queries);
  };
  fn.vkCmdResetQueryPool = [](VkCommandBuffer command, VkQueryPool, u32 first, u32 count) -> void {
    graph_capture(command).reset_queries.emplace_back(first, count);
  };
  fn.vkCmdWriteAccelerationStructuresPropertiesKHR =
    [](VkCommandBuffer command, u32, const VkAccelerationStructureKHR*, VkQueryType, VkQueryPool queries, u32) -> void {
    graph_capture(command).written_queries.push_back(queries);
  };
  fn.vkCmdBuildAccelerationStructuresKHR = [](
                                             VkCommandBuffer command,
                                             u32 count,
                                             const VkAccelerationStructureBuildGeometryInfoKHR* info,
                                             const VkAccelerationStructureBuildRangeInfoKHR* const* ranges
                                           ) -> void {
    auto& captured = graph_capture(command);
    for (u32 index = 0; index < count; index++) {
      const auto geometry_count = info[index].geometryCount;
      captured.builds.push_back(
        {.info = info[index],
         .geometries = {info[index].pGeometries, info[index].pGeometries + geometry_count},
         .ranges = {ranges[index], ranges[index] + geometry_count}}
      );
      captured.builds.back().info.pGeometries = nullptr;
    }
  };
  fn.vkCmdCopyAccelerationStructureKHR =
    [](VkCommandBuffer command, const VkCopyAccelerationStructureInfoKHR* info) -> void {
    graph_capture(command).structure_copies.push_back(*info);
  };
  return context;
}

struct GraphDevice {
  GraphCapture capture = {};
  std::unique_ptr<Context> context = stub_graph_context(capture);
  PersistentAllocator persistent{*context};
  FrameAllocator frame{persistent};
  RenderGraph graph{*context, persistent};

  auto command(this GraphDevice& self) -> CommandBuffer {
    return {.fn = &self.context->fn, .handle = reinterpret_cast<VkCommandBuffer>(&self.capture)};
  }
};

static auto test_buffer(u64 handle, u64 offset = 0, u64 address = 0) -> Buffer {
  return {.handle = graph_handle<VkBuffer>(handle), .offset = offset, .size = 4096, .device_address = address};
}

static auto test_image(u64 handle, vir::Samples samples = vir::Samples::e1) -> ImageAttachment {
  auto info = untyped_info();
  info.mip_levels = samples == vir::Samples::e1 ? 4 : 1;
  info.array_layers = 6;
  info.samples = samples;
  auto attachment = ImageAttachment::from_image(
    Image::from_info(graph_handle<VkImage>(handle), info),
    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
  );
  attachment.image_view = graph_handle<VkImageView>(handle + 100);
  return attachment;
}

static auto unoptimized_program(Module module) -> Program {
  auto instructions = std::vector<Instr>{};
  instructions.reserve(module.instructions.size());
  for (usize index = 0; index < module.instructions.size(); index++) {
    instructions.push_back({.id = static_cast<ValueID>(index), .ir = std::move(module.instructions[index])});
  }
  auto slots = ankerl::unordered_dense::map<ValueID, u32>{};
  for (u32 slot = 0; slot < module.variable_ids.size(); slot++) {
    slots.emplace(module.variable_ids[slot], slot);
  }
  return Program(std::move(instructions), std::move(module.variables), std::move(slots));
}

static auto callback_compute_pipeline(GraphDevice& device) -> std::expected<PipelineID, VkResult> {
  device.context->max_push_descriptors = 16;
  auto layout = ShaderLayout{.stage = vir::ShaderStage::Compute, .local_size = {64, 1, 1}};
  for (const auto binding : {0_u32, 3_u32, 4_u32}) {
    layout.bindings.push_back({
      .binding = binding,
      .descriptor_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .stages = vir::ShaderStage::Compute,
      .usage = binding == 0 ? DescriptorUsage::Read : DescriptorUsage::ReadWrite,
    });
  }
  return device.graph.declare_compute_pipeline({
    .shader = {.spirv = {0x07230203}, .layout = std::move(layout)},
    .name = "callback skinning",
  });
}

TEST(RenderGraphExecution, TypedMappedWritesUseSliceOffsetsAndProtectAdjacentData) {
  auto storage = std::array<u32, 16>{};
  std::ranges::fill(storage, 0xcccccccc);
  auto buffer = test_buffer(1, 64);
  buffer.mapped = reinterpret_cast<u8*>(storage.data());
  buffer.size = sizeof(storage);
  const auto slice = buffer.subrange(16, 16);
  ASSERT_TRUE(slice.has_value());
  const auto values = std::array<u32, 2>{0x01020304, 0x05060708};
  ASSERT_TRUE(slice->write<u32>(4, values).has_value());
  EXPECT_EQ(storage[4], 0xcccccccc);
  EXPECT_EQ(storage[5], values[0]);
  EXPECT_EQ(storage[6], values[1]);
  EXPECT_EQ(storage[7], 0xcccccccc);
  EXPECT_FALSE(slice->write<u32>(12, values).has_value());
  EXPECT_EQ(storage[8], 0xcccccccc);
}

TEST(RenderGraphExecution, UpdateCopiesDataAndReusesRuntimeVariable) {
  auto device = GraphDevice{};
  auto module = Module{};
  const auto buffer = module.declare_buffer_var("destination", Access::ComputeRead);
  const auto bytes = module.declare_bytes_var("update", 8);
  const auto updated = module.update_buffer_from(buffer, bytes, 12);
  auto program = compile(module, module.export_to(updated, Access::ComputeRead));
  EXPECT_EQ(buffer_usage(program, buffer) & VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  program.set(buffer, test_buffer(1, 128));
  const auto first = std::array<u8, 8>{1, 2, 3, 4, 5, 6, 7, 8};
  program.set_bytes(bytes, std::span<const u8>(first));
  const auto one = device.graph.execute(program, device.frame);
  ASSERT_TRUE(one.has_value());
  EXPECT_EQ(one->queue, QueueID{0});
  EXPECT_EQ(one->value, 1);
  const auto second = std::array<u8, 8>{8, 7, 6, 5, 4, 3, 2, 1};
  program.set_bytes(bytes, std::span<const u8>(second));
  ASSERT_TRUE(device.graph.execute(program, device.frame).has_value());
  ASSERT_EQ(device.capture.updates.size(), 2);
  EXPECT_EQ(device.capture.updates[0].first, 140);
  EXPECT_TRUE(std::ranges::equal(device.capture.updates[0].second.data, first));
  EXPECT_TRUE(std::ranges::equal(device.capture.updates[1].second.data, second));
  ASSERT_GE(device.capture.memory.size(), 2);
  EXPECT_EQ(device.capture.memory.back().srcStageMask, VK_PIPELINE_STAGE_2_CLEAR_BIT);
  EXPECT_EQ(device.capture.memory.back().dstStageMask, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
}

TEST(RenderGraphExecution, LiteralUpdateOwnsItsBytes) {
  auto module = Module{};
  const auto buffer = module.import_buffer(test_buffer(1), Access::None);
  auto data = std::array<u8, 4>{1, 2, 3, 4};
  const auto updated = module.update_buffer(buffer, data);
  data.fill(9);
  const auto program = compile(module, updated);
  const auto updates = find_all<ir::UpdateBuffer>(program);
  ASSERT_EQ(updates.size(), 1);
  EXPECT_EQ(updates.front()->data, (std::vector<u8>{1, 2, 3, 4}));
}

TEST(RenderGraphExecution, InvalidUpdatesAreDroppedBeforeCompilation) {
  auto module = Module{};
  const auto buffer = module.transient_buffer({.size = 64});
  const auto data = std::array<u8, 4>{};
  EXPECT_EQ(module.update_buffer(buffer, data, 1), buffer);
  EXPECT_EQ(module.update_buffer(buffer, std::span(data).first(3)), buffer);
  EXPECT_EQ(module.update_buffer(buffer, {}), buffer);
  const auto wrong = module.declare_u32_var("wrong", 0);
  EXPECT_EQ(module.update_buffer_from(buffer, wrong), buffer);
}

TEST(RenderGraphExecution, TransferValidationRejectsOutOfBoundsAndMisalignedOffsets) {
  auto device = GraphDevice{};
  const auto command = device.command();
  const auto buffer = test_buffer(1, 128);
  const auto data = std::array<u8, 8>{};
  EXPECT_FALSE(record_update_buffer(command, buffer, data, 4092).has_value());
  EXPECT_FALSE(record_update_buffer(command, test_buffer(1, 1), data, 0).has_value());
  EXPECT_FALSE(record_fill_buffer(command, buffer, 0, 0, 6).has_value());
  EXPECT_FALSE(record_fill_buffer(command, buffer, 0, 4092, 8).has_value());
  EXPECT_FALSE(record_copy_buffer(command, buffer, test_buffer(2), {.size = ~0_u64 - 1}).has_value());
  EXPECT_FALSE(record_copy_buffer(command, buffer, buffer, {.src_offset = 0, .dst_offset = 4, .size = 8}).has_value());
  EXPECT_TRUE(device.capture.updates.empty());
  EXPECT_TRUE(device.capture.fills.empty());
  EXPECT_TRUE(device.capture.buffer_copies.empty());
}

TEST(RenderGraphExecution, CopiesAndFillsRespectBufferSuballocations) {
  auto device = GraphDevice{};
  auto module = Module{};
  const auto src = module.import_buffer(test_buffer(1, 64), Access::HostWrite);
  const auto dst = module.import_buffer(test_buffer(2, 128), Access::None);
  const auto copied = module.copy_buffer(src, dst, {.src_offset = 4, .dst_offset = 8, .size = 16});
  const auto filled = module.fill_buffer(copied, 0, 24, 12);
  const auto program = compile(module, filled);
  ASSERT_TRUE(device.graph.execute(program, device.frame).has_value());
  ASSERT_EQ(device.capture.buffer_copies.size(), 1);
  EXPECT_EQ(device.capture.buffer_copies[0].srcOffset, 68);
  EXPECT_EQ(device.capture.buffer_copies[0].dstOffset, 136);
  EXPECT_EQ(device.capture.buffer_copies[0].size, 16);
  EXPECT_EQ(device.capture.fills, (std::vector<std::pair<u64, u64>>{{152, 12}}));
}

TEST(RenderGraphExecution, ImageCopyRegionsAreRelativeToViews) {
  auto device = GraphDevice{};
  auto src = test_image(1);
  auto dst = test_image(2);
  src.subresource_range.baseMipLevel = 1;
  src.subresource_range.levelCount = 3;
  src.subresource_range.baseArrayLayer = 2;
  src.subresource_range.layerCount = 4;
  dst.subresource_range.baseArrayLayer = 1;
  dst.subresource_range.layerCount = 5;
  auto module = Module{};
  const auto source = module.import_attachment(src, Access::CopyRead);
  const auto destination = module.import_attachment(dst, Access::None);
  const auto copied = module.copy_image_region(
    source,
    destination,
    {.src_mip = 1,
     .dst_mip = 1,
     .src_layer = 1,
     .dst_layer = 2,
     .layer_count = 2,
     .src_offset = {1, 2, 0},
     .dst_offset = {3, 4, 0},
     .extent = {8, 4, 1}}
  );
  const auto program = compile(module, copied);
  ASSERT_TRUE(device.graph.execute(program, device.frame).has_value());
  ASSERT_EQ(device.capture.copies.size(), 1);
  const auto& copy = device.capture.copies[0];
  EXPECT_EQ(copy.srcSubresource.mipLevel, 2);
  EXPECT_EQ(copy.dstSubresource.mipLevel, 1);
  EXPECT_EQ(copy.srcSubresource.baseArrayLayer, 3);
  EXPECT_EQ(copy.dstSubresource.baseArrayLayer, 3);
  EXPECT_EQ(copy.srcSubresource.layerCount, 2);
  EXPECT_EQ(copy.srcOffset.x, 1);
  EXPECT_EQ(copy.dstOffset.x, 3);
  const auto barriers = image_barriers(module, program);
  EXPECT_TRUE(std::ranges::any_of(barriers, [destination](const Barrier& barrier) -> bool {
    return barrier.resource == destination &&
           barrier.range == Subresource{.base_level = 1, .level_count = 1, .base_layer = 2, .layer_count = 2};
  }));
}

TEST(RenderGraphExecution, ResolveHasItsOwnAccessAndValidatesSamples) {
  auto device = GraphDevice{};
  auto module = Module{};
  const auto src = module.import_attachment(test_image(1, vir::Samples::e4), Access::ColorWrite);
  const auto dst = module.import_attachment(test_image(2), Access::None);
  const auto resolved = module.resolve_region(
    src,
    dst,
    {.src_layer = 1, .dst_layer = 2, .layer_count = 2, .extent = {32, 16, 1}}
  );
  const auto program = compile(module, module.export_to(resolved, Access::FragmentSampled));
  ASSERT_TRUE(device.graph.execute(program, device.frame).has_value());
  ASSERT_EQ(device.capture.resolves.size(), 1);
  EXPECT_EQ(device.capture.resolves[0].srcSubresource.baseArrayLayer, 1);
  const auto barriers = image_barriers(module, program);
  EXPECT_TRUE(std::ranges::any_of(barriers, [](const Barrier& barrier) { return barrier.dst == Access::ResolveRead; }));
  EXPECT_TRUE(std::ranges::any_of(barriers, [](const Barrier& barrier) {
    return barrier.src == Access::ResolveWrite;
  }));
  EXPECT_FALSE(record_copy_image(device.command(), test_image(1), test_image(2), nullopt, true).has_value());
  EXPECT_EQ(device.capture.resolves.size(), 1);
}

TEST(RenderGraphExecution, ImageCopiesRejectInvalidRegions) {
  auto device = GraphDevice{};
  const auto src = test_image(1);
  const auto dst = test_image(2);
  EXPECT_FALSE(
    record_copy_image(device.command(), src, dst, ImageCopy{.src_mip = 4, .extent = {1, 1, 1}}, false).has_value()
  );
  EXPECT_FALSE(record_copy_image(
                 device.command(),
                 src,
                 dst,
                 ImageCopy{.src_layer = 5, .layer_count = 2, .extent = {1, 1, 1}},
                 false
  )
                 .has_value());
  EXPECT_FALSE(
    record_copy_image(device.command(), src, dst, ImageCopy{.src_offset = {-1, 0, 0}, .extent = {1, 1, 1}}, false)
      .has_value()
  );
  EXPECT_FALSE(record_copy_image(device.command(), src, dst, ImageCopy{.extent = {65, 1, 1}}, false).has_value());
  EXPECT_TRUE(device.capture.copies.empty());
}

TEST(RenderGraphExecution, CopiesWithinOneImageRequireDisjointRegionsAndGeneralLayout) {
  auto device = GraphDevice{};
  auto image = test_image(1);
  const auto region = ImageCopy{.layer_count = 1, .dst_offset = {16, 0, 0}, .extent = {8, 8, 1}};
  EXPECT_FALSE(record_copy_image(device.command(), image, image, region, false).has_value());
  image.layout = VK_IMAGE_LAYOUT_GENERAL;
  EXPECT_TRUE(record_copy_image(device.command(), image, image, region, false).has_value());
  auto overlap = region;
  overlap.dst_offset = {4, 0, 0};
  EXPECT_FALSE(record_copy_image(device.command(), image, image, overlap, false).has_value());
  auto incompatible = test_image(2);
  incompatible.image.format = vir::Format::R8Unorm;
  EXPECT_FALSE(record_copy_image(device.command(), image, incompatible, region, false).has_value());
  EXPECT_EQ(device.capture.copies.size(), 1);
}

TEST(RenderGraphExecution, TransferCallbackUsesDeclaredSynchronization) {
  auto device = GraphDevice{};
  auto module = Module{};
  const auto src = module.import_buffer(test_buffer(1, 64), Access::HostWrite);
  const auto dst = module.import_buffer(test_buffer(2, 128), Access::None);
  const auto callback = module.declare_callback_var("copies");
  module.begin_compute({{src, Access::CopyRead}, {dst, Access::CopyWrite | Access::Clear}}).record_from(callback);
  const auto outputs = module.end_compute<2>();
  auto program = compile(module, outputs[1]);
  program.set_callback(callback, [src, dst](Recorder& recorder) -> void {
    recorder.copy_buffer(recorder.buffer(src), recorder.buffer(dst), {.size = 16});
    const auto data = std::array<u8, 4>{1, 2, 3, 4};
    recorder.update_buffer(recorder.buffer(dst), data, 16).fill_buffer(recorder.buffer(dst), 0, 20, 4);
  });
  ASSERT_TRUE(device.graph.execute(program, device.frame).has_value());
  EXPECT_EQ(device.capture.buffer_copies.size(), 1);
  EXPECT_EQ(device.capture.updates.size(), 1);
  EXPECT_EQ(device.capture.fills.size(), 1);
  ASSERT_FALSE(device.capture.memory.empty());
  EXPECT_EQ(device.capture.memory.front().srcAccessMask, VK_ACCESS_2_HOST_WRITE_BIT);
}

TEST(RenderGraphExecution, TransferCallbackRejectsCommandsInsideRendering) {
  auto device = GraphDevice{};
  auto module = Module{};
  const auto buffer = module.import_buffer(test_buffer(1), Access::None);
  const auto callback = module.declare_callback_var("invalid");
  module.begin_rendering_area({{buffer, Access::Clear}}, {16, 16}).record_from(callback);
  const auto outputs = module.end_rendering<1>();
  auto program = compile(module, outputs[0]);
  program.set_callback(callback, [buffer](Recorder& recorder) -> void {
    recorder.fill_buffer(recorder.buffer(buffer), 0);
  });
  EXPECT_FALSE(device.graph.execute(program, device.frame).has_value());
  EXPECT_TRUE(device.capture.fills.empty());
  EXPECT_TRUE(device.capture.signals.empty());
}

TEST(RenderGraphExecution, GraphicsCallbacksBindBindlessWithoutOrdinaryDescriptors) {
  auto device = GraphDevice{};
  const auto bindless = BindlessDescriptorSet{
    .index = 1,
    .layout = graph_handle<VkDescriptorSetLayout>(200),
    .set = graph_handle<VkDescriptorSet>(201),
  };
  const auto declared = device.graph.declare_graphics_pipeline({
    .shaders =
      {
        {.spirv = {0x07230203}, .layout = {.stage = vir::ShaderStage::Vertex}},
        {.spirv = {0x07230203},
         .layout =
           {
             .stage = vir::ShaderStage::Fragment,
             .bindings =
               {{.set = 1,
                 .descriptor_type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                 .is_variable_count = true,
                 .stages = vir::ShaderStage::Fragment,
                 .usage = DescriptorUsage::Sampled}},
           }},
      },
    .bindless = bindless,
    .name = "bindless callback",
  });
  ASSERT_TRUE(declared.has_value());

  auto module = Module{};
  const auto target = module.import_attachment(test_image(1), Access::None);
  const auto callback = module.declare_callback_var("draws");
  module.begin_rendering({{target, Access::ColorWrite}}).bind_graphics_pipeline(*declared).record_from(callback);
  const auto output = module.end_rendering<1>()[0];
  auto program = module.compile(device.graph.pipeline_bindings(), output);
  ASSERT_TRUE(program.has_value());
  program->set_callback(callback, [](Recorder& recorder) -> void {
    recorder.draw(3, 1).draw_indexed(3, 1).draw_indexed_indirect(test_buffer(2), 0, 1);
  });

  // check the first execution and the cached program's next frame
  for (const auto frame_index : {0_u32, 1_u32}) {
    device.capture.descriptor_sets.clear();
    device.capture.draws_after_descriptor_bindings.clear();
    ASSERT_TRUE(device.graph.execute(*program, device.frame).has_value()) << frame_index;
    ASSERT_EQ(device.capture.draws_after_descriptor_bindings.size(), 3);
    for (const auto bound_count : device.capture.draws_after_descriptor_bindings) {
      EXPECT_GT(bound_count, 0);
    }
    ASSERT_FALSE(device.capture.descriptor_sets.empty());
    for (const auto& bound : device.capture.descriptor_sets) {
      EXPECT_EQ(bound.bind_point, VK_PIPELINE_BIND_POINT_GRAPHICS);
      EXPECT_EQ(bound.layout, graph_handle<VkPipelineLayout>(101));
      EXPECT_EQ(bound.index, bindless.index);
      EXPECT_EQ(bound.handle, bindless.set);
    }
  }
}

TEST(RenderGraphExecution, GraphicsCallbacksRejectMissingOrdinaryDescriptorsBeforeDrawing) {
  auto device = GraphDevice{};
  const auto declared = device.graph.declare_graphics_pipeline({
    .shaders =
      {
        {.spirv = {0x07230203}, .layout = {.stage = vir::ShaderStage::Vertex}},
        {.spirv = {0x07230203},
         .layout =
           {
             .stage = vir::ShaderStage::Fragment,
             .bindings =
               {{.descriptor_type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                 .stages = vir::ShaderStage::Fragment,
                 .usage = DescriptorUsage::Uniform}},
           }},
      },
    .name = "missing callback descriptor",
  });
  ASSERT_TRUE(declared.has_value());

  auto module = Module{};
  const auto target = module.import_attachment(test_image(1), Access::None);
  const auto callback = module.declare_callback_var("draws");
  module.begin_rendering({{target, Access::ColorWrite}}).bind_graphics_pipeline(*declared).record_from(callback);
  auto program = compile(module, module.end_rendering<1>()[0]);
  program.set_callback(callback, [](Recorder& recorder) -> void { recorder.draw_indexed(3, 1); });

  EXPECT_FALSE(device.graph.execute(program, device.frame).has_value());
  EXPECT_TRUE(device.capture.draws_after_descriptor_bindings.empty());
  EXPECT_TRUE(device.capture.signals.empty());
}

TEST(RenderGraphExecution, ComputeCallbacksCanSkipDispatchWithIncompleteDescriptors) {
  auto device = GraphDevice{};
  const auto declared = callback_compute_pipeline(device);
  ASSERT_TRUE(declared.has_value());
  auto module = Module{};
  const auto input = module.import_buffer(test_buffer(1), Access::HostWrite);
  const auto output = module.import_buffer(test_buffer(2), Access::None);
  const auto callback = module.declare_callback_var("no skinning jobs");
  module.begin_compute({{input, Access::ComputeRead}, {output, Access::ComputeWrite}})
    .bind_compute_pipeline(*declared)
    .bind_buffer(0, 0, input)
    .record_from(callback);
  auto program = module.compile(device.graph.pipeline_bindings(), module.end_compute<2>()[1]);
  ASSERT_TRUE(program.has_value());
  auto called = false;
  program->set_callback(callback, [&called](Recorder&) -> void { called = true; });

  ASSERT_TRUE(device.graph.execute(*program, device.frame).has_value());
  EXPECT_TRUE(called);
  EXPECT_TRUE(device.capture.descriptor_buffers.empty());
  EXPECT_TRUE(device.capture.dispatches_after_descriptor_bindings.empty());
  EXPECT_EQ(device.capture.signals.size(), 1);
}

TEST(RenderGraphExecution, ComputeCallbacksResolveDynamicBufferRangesForEveryDispatch) {
  auto device = GraphDevice{};
  const auto declared = callback_compute_pipeline(device);
  ASSERT_TRUE(declared.has_value());
  auto module = Module{};
  const auto input = module.declare_buffer_var("input", Access::HostWrite);
  const auto output = module.declare_buffer_var("skinned arena", Access::None);
  const auto callback = module.declare_callback_var("skin jobs");
  module.begin_compute({{input, Access::ComputeRead}, {output, Access::ComputeWrite}})
    .bind_compute_pipeline(*declared)
    .bind_buffer(0, 0, input)
    .record_from(callback);
  auto program = module.compile(device.graph.pipeline_bindings(), module.end_compute<2>()[1]);
  ASSERT_TRUE(program.has_value());

  for (const auto frame_index : {0_u64, 1_u64}) {
    const auto source = test_buffer(1 + frame_index, 64);
    const auto arena = test_buffer(3 + frame_index, 128);
    const auto positions_bytes = 256 * (frame_index + 1);
    const auto normals_bytes = positions_bytes / 2;
    program->set(input, source);
    program->set(output, arena);
    program->set_callback(callback, [output, positions_bytes, normals_bytes](Recorder& recorder) -> void {
      const auto buffer = recorder.buffer(output);
      recorder.bind_buffer(0, 3, buffer, 0, positions_bytes)
        .bind_buffer(0, 4, buffer, positions_bytes, normals_bytes)
        .dispatch(1, 1, 1)
        .dispatch(2, 1, 1);
    });
    device.capture.descriptor_buffers.clear();
    device.capture.dispatches_after_descriptor_bindings.clear();

    ASSERT_TRUE(device.graph.execute(*program, device.frame).has_value()) << frame_index;
    EXPECT_EQ(device.capture.dispatches_after_descriptor_bindings, (std::vector<usize>{3, 6}));
    ASSERT_EQ(device.capture.descriptor_buffers.size(), 6);
    for (const auto dispatch_index : {0_u32, 1_u32}) {
      const auto& read = device.capture.descriptor_buffers[dispatch_index * 3];
      EXPECT_EQ(read.set, 0);
      EXPECT_EQ(read.binding, 0);
      EXPECT_EQ(read.type, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
      EXPECT_EQ(read.info.buffer, source.handle);
      EXPECT_EQ(read.info.offset, source.offset);
      EXPECT_EQ(read.info.range, source.size);
      const auto& positions = device.capture.descriptor_buffers[dispatch_index * 3 + 1];
      EXPECT_EQ(positions.binding, 3);
      EXPECT_EQ(positions.info.buffer, arena.handle);
      EXPECT_EQ(positions.info.offset, arena.offset);
      EXPECT_EQ(positions.info.range, positions_bytes);
      const auto& normals = device.capture.descriptor_buffers[dispatch_index * 3 + 2];
      EXPECT_EQ(normals.binding, 4);
      EXPECT_EQ(normals.info.buffer, arena.handle);
      EXPECT_EQ(normals.info.offset, arena.offset + positions_bytes);
      EXPECT_EQ(normals.info.range, normals_bytes);
    }
  }

  // a cached program still rejects an incomplete job before any dispatch reaches Vulkan
  auto called = false;
  program->set_callback(callback, [output, &called](Recorder& recorder) -> void {
    called = true;
    recorder.bind_buffer(0, 3, recorder.buffer(output), 0, 256).dispatch(1, 1, 1);
  });
  device.capture.descriptor_buffers.clear();
  device.capture.dispatches_after_descriptor_bindings.clear();
  EXPECT_FALSE(device.graph.execute(*program, device.frame).has_value());
  EXPECT_TRUE(called);
  EXPECT_TRUE(device.capture.descriptor_buffers.empty());
  EXPECT_TRUE(device.capture.dispatches_after_descriptor_bindings.empty());
  EXPECT_EQ(device.capture.signals.size(), 2);
}

TEST(RenderGraphExecution, GraphicsCallbacksResolveTransientGeometryVariablesAcrossFrames) {
  auto device = GraphDevice{};
  const auto declared = device.graph.declare_graphics_pipeline({
    .shaders =
      {
        {.spirv = {0x07230203}, .layout = {.stage = vir::ShaderStage::Vertex, .push_constant_size = sizeof(u64)}},
        {.spirv = {0x07230203}, .layout = {.stage = vir::ShaderStage::Fragment}},
      },
    .name = "transient geometry callback",
  });
  ASSERT_TRUE(declared.has_value());

  auto module = Module{};
  const auto target = module.import_attachment(test_image(1), Access::None);
  const auto vertices = module.declare_buffer_var("vertices", Access::None);
  const auto indices = module.declare_buffer_var("indices", Access::None);
  const auto callback = module.declare_callback_var("draws");
  module.begin_rendering({{target, Access::ColorRW}, {vertices, Access::VertexRead}, {indices, Access::IndexRead}})
    .bind_graphics_pipeline(*declared)
    .record_from(callback);
  auto program = module.compile(device.graph.pipeline_bindings(), module.end_rendering<3>()[0]);
  ASSERT_TRUE(program.has_value());
  ASSERT_TRUE(program->has_variable(vertices));
  ASSERT_TRUE(program->has_variable(indices));
  const auto instruction_count = program->instructions.size();
  program->set_callback(callback, [vertices, indices](Recorder& recorder) -> void {
    const auto vertex_buffer = recorder.buffer(vertices);
    const auto index_buffer = recorder.buffer(indices);
    if (vertex_buffer.is_null() || index_buffer.is_null()) {
      return;
    }
    recorder.bind_index_buffer(index_buffer, VK_INDEX_TYPE_UINT16)
      .push_constants(vertex_buffer.device_address + 60_u64)
      .draw_indexed(6, 1, 3, 0, 0);
  });

  // empty frames and failed uploads must not reuse a previous frame's geometry
  const auto frames = std::array{
    std::pair{Buffer{}, Buffer{}},
    std::pair{test_buffer(2, 16, 0x1000), test_buffer(3, 32)},
    std::pair{Buffer{}, Buffer{}},
    std::pair{test_buffer(2, 256, 0x1100), test_buffer(3, 128)},
    std::pair{test_buffer(4, 512, 0x2000), test_buffer(5, 256)},
    std::pair{Buffer{}, Buffer{}},
  };
  for (const auto& [vertex_buffer, index_buffer] : frames) {
    program->set(vertices, vertex_buffer);
    program->set(indices, index_buffer);
    device.capture.index_bindings.clear();
    device.capture.pushes.clear();
    device.capture.draws_after_descriptor_bindings.clear();
    ASSERT_TRUE(device.graph.execute(*program, device.frame).has_value());
    EXPECT_EQ(program->instructions.size(), instruction_count);
    if (vertex_buffer.is_null()) {
      EXPECT_TRUE(device.capture.index_bindings.empty());
      EXPECT_TRUE(device.capture.pushes.empty());
      EXPECT_TRUE(device.capture.draws_after_descriptor_bindings.empty());
      continue;
    }
    ASSERT_EQ(device.capture.index_bindings.size(), 1);
    EXPECT_EQ(device.capture.index_bindings.front().buffer, index_buffer.handle);
    EXPECT_EQ(device.capture.index_bindings.front().offset, index_buffer.offset);
    EXPECT_EQ(device.capture.index_bindings.front().type, VK_INDEX_TYPE_UINT16);
    ASSERT_EQ(device.capture.pushes.size(), 1);
    ASSERT_EQ(device.capture.pushes.front().bytes.data.size(), sizeof(u64));
    auto address = 0_u64;
    std::memcpy(&address, device.capture.pushes.front().bytes.data.data(), sizeof(address));
    EXPECT_EQ(address, vertex_buffer.device_address + 60_u64);
    EXPECT_EQ(device.capture.draws_after_descriptor_bindings.size(), 1);
  }
}

TEST(RenderGraphExecution, PushConstantsCombineRuntimeVariablesAndDeviceAddressesAcrossFrames) {
  auto device = GraphDevice{};
  const auto declared = device.graph.declare_graphics_pipeline({
    .shaders =
      {
        {.spirv = {0x07230203}, .layout = {.stage = vir::ShaderStage::Vertex, .push_constant_size = 32}},
        {.spirv = {0x07230203}, .layout = {.stage = vir::ShaderStage::Fragment, .push_constant_size = 32}},
      },
    .name = "mixed push constants",
  });
  ASSERT_TRUE(declared.has_value());

  auto module = Module{};
  const auto target = module.import_attachment(test_image(1), Access::None);
  const auto first_buffer = module.declare_buffer_var("first address", Access::None);
  const auto second_buffer = module.declare_buffer_var("second address", Access::None);
  const auto head = module.declare_bytes_var("head", sizeof(u64));
  const auto tail = module.declare_bytes_var("tail", sizeof(u64));
  module.begin_rendering({{target, Access::ColorWrite}})
    .bind_graphics_pipeline(*declared)
    .push_constant_address(0, first_buffer)
    .push_constants_from_at(8, head)
    .push_constant_address(16, second_buffer)
    .push_constants_from_at(24, tail)
    .draw(3, 1);
  auto program = module.compile(device.graph.pipeline_bindings(), module.end_rendering<1>()[0]);
  ASSERT_TRUE(program.has_value());
  EXPECT_TRUE(program->has_variable(first_buffer));
  EXPECT_TRUE(program->has_variable(second_buffer));
  EXPECT_TRUE(program->has_variable(head));
  EXPECT_TRUE(program->has_variable(tail));

  for (const auto frame_index : {0_u64, 1_u64}) {
    const auto
      expected = std::array<u64, 4>{0x1000 + frame_index, 17 + frame_index, 0x2000 + frame_index, 29 + frame_index};
    program->set(first_buffer, test_buffer(2, 0, expected[0]));
    program->set(second_buffer, test_buffer(3, 0, expected[2]));
    program->set_bytes(head, expected[1]);
    program->set_bytes(tail, expected[3]);
    device.capture.pushes.clear();
    ASSERT_TRUE(device.graph.execute(*program, device.frame).has_value()) << frame_index;
    ASSERT_EQ(device.capture.pushes.size(), 1);
    const auto& pushed = device.capture.pushes.front();
    EXPECT_EQ(pushed.offset, 0);
    EXPECT_EQ(pushed.stages, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    ASSERT_EQ(pushed.bytes.data.size(), sizeof(expected));
    auto actual = std::array<u64, 4>{};
    std::memcpy(actual.data(), pushed.bytes.data.data(), sizeof(actual));
    EXPECT_EQ(actual, expected);
  }
}

TEST(RenderGraphExecution, AttachmentlessMultiviewIsKeptAndExecuted) {
  auto device = GraphDevice{};
  auto module = Module{};
  module.begin_rendering_area({}, {32, 16}, 1, 0b101);
  module.end_rendering<0>();
  const auto compiled = module.compile_all(PipelineBindings::unchecked(), {});
  ASSERT_TRUE(compiled.has_value());
  ASSERT_EQ(find_all<ir::BeginRendering>(*compiled).size(), 1);
  ASSERT_TRUE(device.graph.execute(*compiled, device.frame).has_value());
  EXPECT_EQ(device.capture.color_count, 0);
  EXPECT_EQ(device.capture.view_mask, 0b101);
  EXPECT_EQ(device.capture.layer_count, 1);
  EXPECT_EQ(device.capture.rendering_ends, 1);
}

TEST(RenderGraphExecution, AttachmentLoadOpsMatchDeclaredReadsAndExplicitClears) {
  auto device = GraphDevice{};
  auto module = Module{};
  const auto attachment = [](u64 handle, vir::Format format) -> ImageAttachment {
    auto info = untyped_info();
    info.format = format;
    auto image = ImageAttachment::from_image(
      Image::from_info(graph_handle<VkImage>(handle), info),
      VK_IMAGE_LAYOUT_UNDEFINED
    );
    image.image_view = graph_handle<VkImageView>(handle + 100);
    return image;
  };
  auto color = module.import_attachment(attachment(1, FORMAT), Access::None);
  const auto depth_image = attachment(2, DEPTH_FORMAT);
  auto depth = module.import_attachment(depth_image, Access::None);
  const auto callback = module.declare_callback_var("attachment writes");
  for (const auto access : {Access::ColorWrite, Access::ColorRW}) {
    const auto depth_access = access == Access::ColorWrite ? Access::DepthStencilWrite : Access::DepthStencilRW;
    module.begin_rendering({{color, access}, {depth, depth_access}}).record_from(callback);
    std::tie(color, depth) = std::tuple_cat(module.end_rendering<2>());
  }
  color = module.clear(color, clear::WHITE);
  module.begin_rendering({{color, Access::ColorWrite}}).record_from(callback);
  auto program = compile(module, module.end_rendering<1>()[0]);
  program.set_callback(callback, [](Recorder&) -> void {});
  ASSERT_TRUE(device.graph.execute(program, device.frame).has_value());
  EXPECT_EQ(
    device.capture.color_loads,
    (std::vector<VkAttachmentLoadOp>{
      VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      VK_ATTACHMENT_LOAD_OP_LOAD,
      VK_ATTACHMENT_LOAD_OP_CLEAR,
    })
  );
  EXPECT_EQ(
    device.capture.depth_loads,
    (std::vector<VkAttachmentLoadOp>{VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_LOAD})
  );
}

TEST(RenderGraphExecution, MultiviewReachesThePipelineKey) {
  auto module = Module{};
  const auto image = module.transient_image(untyped_info());
  module.begin_rendering_area({{image, Access::ColorWrite}}, {16, 16}, 1, 0b11)
    .bind_graphics_pipeline(pipeline(0))
    .draw(3, 1);
  const auto result = module.end_rendering<1>();
  const auto program = compile(module, result[0]);
  const auto draws = find_all<ir::Draw>(program);
  ASSERT_EQ(draws.size(), 1);
  EXPECT_EQ(draws[0]->state.rendering.view_mask, 0b11);
}

TEST(RenderGraphExecution, QueueTicketsWaitWithoutHoldingTheExecutionMutex) {
  auto device = GraphDevice{};
  auto module = Module{};
  const auto output = module.fill_buffer(module.import_buffer(test_buffer(1), Access::None), 0);
  const auto program = compile(module, output);
  const auto ticket = device.graph.execute(program, device.frame);
  ASSERT_TRUE(ticket.has_value());
  auto lock = std::unique_lock(device.context->queues[0]->submit_mutex);
  // wait has no queue lock, so it is also safe while another caller is recording
  ASSERT_TRUE(device.context->wait(*ticket).has_value());
  EXPECT_TRUE(device.context->is_complete(*ticket));
  EXPECT_FALSE(device.context->wait({.queue = QueueID::Invalid, .value = 1}).has_value());
}

TEST(RenderGraphExecution, ConcurrentExecutionsUseDifferentThreadPoolsAndSerializeRecording) {
  auto device = GraphDevice{};
  auto module = Module{};
  const auto output = module.fill_buffer(module.import_buffer(test_buffer(1), Access::None), 0);
  const auto program = compile(module, output);
  auto start = std::barrier(3);
  auto results = std::array<std::expected<SubmitTicket, VkResult>, 2>{};
  auto workers = std::array<std::thread, 2>{};
  for (usize index = 0; index < workers.size(); index++) {
    workers[index] = std::thread([&device, &program, &start, &results, index]() -> void {
      start.arrive_and_wait();
      results[index] = device.graph.execute(program, device.frame);
    });
  }
  start.arrive_and_wait();
  for (auto& worker : workers) {
    worker.join();
  }
  ASSERT_TRUE(results[0].has_value());
  ASSERT_TRUE(results[1].has_value());
  EXPECT_NE(results[0]->value, results[1]->value);
  ASSERT_EQ(device.capture.pools.size(), 2);
  EXPECT_NE(device.capture.pools[0].thread, device.capture.pools[1].thread);
  EXPECT_NE(device.capture.allocated_from[0], device.capture.allocated_from[1]);
  EXPECT_EQ(device.capture.max_recording.load(), 1);
  EXPECT_EQ(device.capture.signals, (std::vector<u64>{1, 2}));
}

TEST(RenderGraphExecution, FailedSubmitDoesNotConsumeATimelineValue) {
  auto device = GraphDevice{};
  auto module = Module{};
  const auto output = module.fill_buffer(module.import_buffer(test_buffer(1), Access::None), 0);
  const auto program = compile(module, output);
  device.capture.fail_submit_at = 1;
  EXPECT_FALSE(device.graph.execute(program, device.frame).has_value());
  EXPECT_TRUE(device.frame.timeline_waits.empty());
  const auto retried = device.graph.execute(program, device.frame);
  ASSERT_TRUE(retried.has_value());
  EXPECT_EQ(retried->value, 1);
}
static auto triangle_build(u64 destination) -> AccelerationStructureBuild {
  auto build = AccelerationStructureBuild{};
  build.info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
  build.info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  build.info.flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
  build.info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  build.info.dstAccelerationStructure = graph_handle<VkAccelerationStructureKHR>(destination);
  auto geometry = VkAccelerationStructureGeometryKHR{};
  geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
  geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
  geometry.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
  geometry.geometry.triangles.vertexFormat = static_cast<VkFormat>(vir::Format::RGB32Sfloat);
  geometry.geometry.triangles.vertexStride = 12;
  geometry.geometry.triangles.maxVertex = 2;
  geometry.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
  build.geometries.push_back(geometry);
  build.ranges.push_back({.primitiveCount = 1, .primitiveOffset = 0, .firstVertex = 0, .transformOffset = 0});
  return build;
}

TEST(RenderGraphExecution, AccelerationBuildPatchesRuntimeAddressesAndInfersUsage) {
  auto device = GraphDevice{};
  auto module = Module{};
  const auto vertices = module.declare_buffer_var("vertices", Access::HostWrite);
  const auto indices = module.declare_buffer_var("indices", Access::HostWrite);
  const auto scratch = module.declare_buffer_var("scratch", Access::None);
  const auto backing = module.declare_buffer_var("backing", Access::None);
  const auto builds = module.declare_acceleration_structure_builds_var("builds");
  const auto addresses = std::array{
    AccelerationStructureAddress{.kind = AccelerationStructureAddressKind::Vertices, .buffer = vertices, .offset = 12},
    AccelerationStructureAddress{.kind = AccelerationStructureAddressKind::Indices, .buffer = indices, .offset = 4},
    AccelerationStructureAddress{.kind = AccelerationStructureAddressKind::Scratch, .buffer = scratch}
  };
  const auto resources = std::array{ResourceAccess{backing, Access::AccelerationStructureBuildWrite}};
  const auto results = module.build_acceleration_structures(builds, addresses, resources);
  ASSERT_EQ(results.size(), 1);
  auto program = compile(module, module.export_to(results[0], Access::RayTracingRead));
  EXPECT_NE(buffer_usage(program, vertices) & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, 0);
  EXPECT_NE(buffer_usage(program, vertices) & VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, 0);
  EXPECT_NE(buffer_usage(program, backing) & VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, 0);
  program.set(vertices, test_buffer(1, 64, 0x1000));
  program.set(indices, test_buffer(2, 128, 0x2000));
  program.set(scratch, test_buffer(3, 256, 0x3000));
  program.set(backing, test_buffer(4));
  auto initial = triangle_build(10);
  program.set(
    builds,
    std::make_shared<const std::vector<AccelerationStructureBuild>>(std::vector<AccelerationStructureBuild>{initial})
  );
  ASSERT_TRUE(device.graph.execute(program, device.frame).has_value());
  ASSERT_EQ(device.capture.builds.size(), 1);
  const auto& built = device.capture.builds[0];
  EXPECT_EQ(built.geometries[0].geometry.triangles.vertexData.deviceAddress, 0x100c);
  EXPECT_EQ(built.geometries[0].geometry.triangles.indexData.deviceAddress, 0x2004);
  EXPECT_EQ(built.info.scratchData.deviceAddress, 0x3000);
  EXPECT_EQ(initial.geometries[0].geometry.triangles.vertexData.deviceAddress, 0);
  program.set(vertices, test_buffer(1, 512, 0x4000));
  ASSERT_TRUE(device.graph.execute(program, device.frame).has_value());
  EXPECT_EQ(device.capture.builds.back().geometries[0].geometry.triangles.vertexData.deviceAddress, 0x400c);
  EXPECT_TRUE(std::ranges::any_of(device.capture.memory, [](const VkMemoryBarrier2& barrier) -> bool {
    return (barrier.srcAccessMask & VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR) != 0 &&
           (barrier.dstStageMask & VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR) != 0;
  }));
}

TEST(RenderGraphExecution, BuildQueryAndCompactionHaveSeparateCommandsAndBarriers) {
  auto device = GraphDevice{};
  auto module = Module{};
  const auto src = module.import_buffer(test_buffer(1), Access::None);
  const auto dst = module.import_buffer(test_buffer(2), Access::None);
  const auto builds = module.declare_acceleration_structure_builds_var("builds");
  const auto resources = std::array{ResourceAccess{src, Access::AccelerationStructureBuildWrite}};
  const auto built = module.build_acceleration_structures(builds, {}, resources);
  const auto queries = module.transient_query_pool(VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, 2);
  const auto queried = module.write_acceleration_structure_properties(
    built[0],
    graph_handle<VkAccelerationStructureKHR>(10),
    queries,
    1
  );
  const auto compacted = module.copy_acceleration_structure(
    queried,
    dst,
    graph_handle<VkAccelerationStructureKHR>(10),
    graph_handle<VkAccelerationStructureKHR>(11)
  );
  auto program = compile(module, module.export_to(compacted, Access::RayTracingRead));
  ASSERT_EQ(find_all<ir::BuildAccelerationStructures>(program).size(), 1);
  ASSERT_EQ(find_all<ir::WriteAccelerationStructureProperties>(program).size(), 1);
  ASSERT_EQ(find_all<ir::CopyAccelerationStructure>(program).size(), 1);
  auto build = triangle_build(10);
  build.info.scratchData.deviceAddress = 0x2000;
  build.geometries[0].geometry.triangles.vertexData.deviceAddress = 0x1000;
  program.set(
    builds,
    std::make_shared<const std::vector<AccelerationStructureBuild>>(std::vector<AccelerationStructureBuild>{build})
  );
  ASSERT_TRUE(device.graph.execute(program, device.frame).has_value());
  EXPECT_EQ(device.capture.builds.size(), 1);
  EXPECT_EQ(device.capture.structure_copies.size(), 1);
  EXPECT_EQ(device.capture.structure_copies[0].mode, VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR);
  ASSERT_EQ(device.capture.created_queries.size(), 1);
  EXPECT_EQ(device.capture.written_queries, (std::vector<VkQueryPool>{device.capture.created_queries[0].handle}));
  EXPECT_EQ(device.capture.reset_queries, (std::vector<std::pair<u32, u32>>{{1, 1}}));
  EXPECT_TRUE(device.capture.destroyed_queries.empty());
  ASSERT_TRUE(device.frame.recycle(1).has_value());
  EXPECT_EQ(device.capture.destroyed_queries, device.capture.written_queries);
  EXPECT_EQ(device.capture.waits.back(), 1);
  const auto barriers = memory_barriers(module, program);
  EXPECT_TRUE(std::ranges::any_of(barriers, [](const auto& barrier) -> bool {
    return intersects(barrier.first, Access::AccelerationStructureBuildWrite) &&
           intersects(barrier.second, Access::AccelerationStructureBuildRead);
  }));
  EXPECT_NE(program.dump().find("copy_acceleration_structure"), std::string::npos);
}

TEST(RenderGraphExecution, AccelerationBuildRejectsInvalidPatchAndMissingExtension) {
  auto device = GraphDevice{};
  auto module = Module{};
  const auto input = module.import_buffer(test_buffer(1, 0, 0x1000), Access::HostWrite);
  const auto backing = module.import_buffer(test_buffer(2), Access::None);
  const auto builds = module.declare_acceleration_structure_builds_var("builds");
  const auto addresses = std::array{AccelerationStructureAddress{.geometry_index = 1, .buffer = input}};
  const auto resources = std::array{ResourceAccess{backing, Access::AccelerationStructureBuildWrite}};
  const auto outputs = module.build_acceleration_structures(builds, addresses, resources);
  auto program = compile(module, outputs[0]);
  program.set(
    builds,
    std::make_shared<const std::vector<AccelerationStructureBuild>>(
      std::vector<AccelerationStructureBuild>{triangle_build(10)}
    )
  );
  EXPECT_FALSE(device.graph.execute(program, device.frame).has_value());
  EXPECT_TRUE(device.capture.builds.empty());
  device.context->fn.vkCmdBuildAccelerationStructuresKHR = nullptr;
  const auto missing = device.graph.execute(program, device.frame);
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error(), VK_ERROR_EXTENSION_NOT_PRESENT);
}

TEST(RenderGraphExecution, AccelerationBuildListSynchronizesDependentBuilds) {
  auto device = GraphDevice{};
  auto module = Module{};
  const auto backing = module.import_buffer(test_buffer(1), Access::None);
  const auto builds = module.declare_acceleration_structure_builds_var("dependent builds");
  const auto resources = std::array{ResourceAccess{backing, Access::AccelerationStructureBuildRW}};
  const auto outputs = module.build_acceleration_structures(builds, {}, resources);
  auto program = compile(module, outputs[0]);
  program.set(
    builds,
    std::make_shared<const std::vector<AccelerationStructureBuild>>(
      std::vector<AccelerationStructureBuild>{triangle_build(10), triangle_build(11)}
    )
  );
  ASSERT_TRUE(device.graph.execute(program, device.frame).has_value());
  EXPECT_EQ(device.capture.builds.size(), 2);
  EXPECT_TRUE(std::ranges::any_of(device.capture.memory, [](const VkMemoryBarrier2& barrier) -> bool {
    return (barrier.srcAccessMask & VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR) != 0 &&
           (barrier.dstAccessMask & VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR) != 0;
  }));
}

TEST(RenderGraphExecution, ProceduralAndInstanceBuildsPatchTheirOwnBuffersAndAlignedScratch) {
  auto device = GraphDevice{};
  device.context->acceleration_structure_scratch_alignment = 256;
  auto module = Module{};
  const auto aabbs = module.import_buffer(test_buffer(1, 0, 0x1000), Access::HostWrite);
  const auto instances = module.import_buffer(test_buffer(2, 0, 0x2000), Access::HostWrite);
  const auto scratch = module.declare_buffer_var("scratch", Access::None);
  const auto backing = module.import_buffer(test_buffer(3), Access::None);
  const auto builds = module.declare_acceleration_structure_builds_var("builds");
  const auto addresses = std::array{
    AccelerationStructureAddress{.kind = AccelerationStructureAddressKind::Aabbs, .buffer = aabbs, .offset = 24},
    AccelerationStructureAddress{.kind = AccelerationStructureAddressKind::Scratch, .buffer = scratch},
    AccelerationStructureAddress{
      .build_index = 1,
      .kind = AccelerationStructureAddressKind::Instances,
      .buffer = instances,
      .offset = 64
    },
    AccelerationStructureAddress{
      .build_index = 1,
      .kind = AccelerationStructureAddressKind::Scratch,
      .buffer = scratch,
      .offset = 256
    }
  };
  const auto resources = std::array{ResourceAccess{backing, Access::AccelerationStructureBuildRW}};
  const auto outputs = module.build_acceleration_structures(builds, addresses, resources);
  auto program = compile(module, outputs[0]);
  auto procedural = triangle_build(10);
  procedural.geometries[0].geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
  procedural.geometries[0].geometry.aabbs = {
    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR,
    .pNext = nullptr,
    .data = {},
    .stride = 24
  };
  auto top_level = triangle_build(11);
  top_level.info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  top_level.geometries[0].geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
  top_level.geometries[0].geometry.instances = {
    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
    .pNext = nullptr,
    .arrayOfPointers = VK_FALSE,
    .data = {}
  };
  program.set(
    builds,
    std::make_shared<const std::vector<AccelerationStructureBuild>>(
      std::vector<AccelerationStructureBuild>{procedural, top_level}
    )
  );
  program.set(scratch, test_buffer(4, 0, 0x3000));
  ASSERT_TRUE(device.graph.execute(program, device.frame).has_value());
  ASSERT_EQ(device.capture.builds.size(), 2);
  EXPECT_EQ(device.capture.builds[0].geometries[0].geometry.aabbs.data.deviceAddress, 0x1018);
  EXPECT_EQ(device.capture.builds[1].geometries[0].geometry.instances.data.deviceAddress, 0x2040);
  EXPECT_EQ(device.capture.builds[1].info.scratchData.deviceAddress, 0x3100);
  program.set(scratch, test_buffer(4, 0, 0x3004));
  EXPECT_FALSE(device.graph.execute(program, device.frame).has_value());
  EXPECT_EQ(device.capture.builds.size(), 2);
}

TEST(RenderGraphExecution, QueryPoolVariablesRemainOwnedByTheCaller) {
  auto device = GraphDevice{};
  auto module = Module{};
  const auto backing = module.import_buffer(test_buffer(1), Access::AccelerationStructureBuildWrite);
  const auto queries = module.declare_query_pool_var("queries");
  const auto output = module.write_acceleration_structure_properties(
    backing,
    graph_handle<VkAccelerationStructureKHR>(10),
    queries
  );
  auto program = compile(module, output);
  const auto owned = device.persistent.allocate_query_pool(VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, 1);
  ASSERT_TRUE(owned.has_value());
  program.set(queries, *owned);
  ASSERT_TRUE(device.graph.execute(program, device.frame).has_value());
  ASSERT_TRUE(device.frame.recycle(1).has_value());
  EXPECT_TRUE(device.capture.destroyed_queries.empty());
  device.persistent.deallocate_query_pool(*owned);
  EXPECT_EQ(device.capture.destroyed_queries.size(), 1);
}

TEST(RenderGraphExecution, QueryWritesRetireBeforeAnotherWorkerSubmits) {
  auto device = GraphDevice{};
  auto module = Module{};
  const auto backing = module.import_buffer(test_buffer(1), Access::AccelerationStructureBuildWrite);
  const auto queries = module.transient_query_pool(VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, 1);
  const auto queried = module.write_acceleration_structure_properties(
    backing,
    graph_handle<VkAccelerationStructureKHR>(10),
    queries
  );
  const auto query_program = compile(module, queried);
  device.context->fn.vkWaitSemaphores = [](VkDevice handle, const VkSemaphoreWaitInfo* info, u64) -> VkResult {
    auto& captured = graph_capture(handle);
    if (captured.waits.empty()) {
      // the pending query guards submissions while its wait releases the queue mutex
      auto unlocked = false;
      auto probe = std::jthread([&captured, &unlocked, info]() -> void {
        unlocked = captured.queue->submit_mutex.try_lock();
        if (unlocked) {
          EXPECT_EQ(captured.queue->pending_query_retirement, info->pValues[0]);
          captured.queue->submit_mutex.unlock();
        }
      });
      probe.join();
      EXPECT_TRUE(unlocked);
    }
    captured.waits.push_back(info->pValues[0]);
    return VK_SUCCESS;
  };
  const auto first = device.graph.execute(query_program, device.frame);
  ASSERT_TRUE(first);
  EXPECT_TRUE(device.capture.waits.empty());
  EXPECT_EQ(device.capture.queue->pending_query_retirement, first->value);

  auto upload_module = Module{};
  const auto uploaded = upload_module.fill_buffer(upload_module.import_buffer(test_buffer(2), Access::None), 0);
  const auto upload_program = compile(upload_module, uploaded);
  auto second = std::expected<SubmitTicket, VkResult>{};
  auto worker = std::jthread([&device, &upload_program, &second]() -> void {
    auto allocator = FrameAllocator(device.persistent);
    second = device.graph.execute(upload_program, allocator);
  });
  worker.join();
  ASSERT_TRUE(second);
  EXPECT_EQ(second->value, first->value + 1);
  EXPECT_EQ(device.capture.waits_before_submits, (std::vector<usize>{0, 1}));
  EXPECT_EQ(device.capture.queue->pending_query_retirement, 0);
}

TEST(RenderGraphExecution, FailedQueryRetirementKeepsSubmittedResourcesAlive) {
  auto device = GraphDevice{};
  auto module = Module{};
  const auto backing = module.import_buffer(test_buffer(1), Access::AccelerationStructureBuildWrite);
  const auto queries = module.transient_query_pool(VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, 1);
  const auto queried = module.write_acceleration_structure_properties(
    backing,
    graph_handle<VkAccelerationStructureKHR>(10),
    queries
  );
  const auto program = compile(module, queried);
  ASSERT_TRUE(device.graph.execute(program, device.frame));
  auto next_module = Module{};
  const auto filled = next_module.fill_buffer(next_module.import_buffer(test_buffer(2), Access::None), 0);
  const auto next_program = compile(next_module, filled);
  const auto wait = device.context->fn.vkWaitSemaphores;
  device.context->fn.vkWaitSemaphores = [](VkDevice, const VkSemaphoreWaitInfo*, u64) -> VkResult {
    return VK_ERROR_DEVICE_LOST;
  };
  const auto submitted = device.graph.execute(next_program, device.frame);
  device.context->fn.vkWaitSemaphores = wait;
  EXPECT_FALSE(submitted);
  if (!submitted) {
    EXPECT_EQ(submitted.error(), VK_ERROR_DEVICE_LOST);
  }
  EXPECT_EQ(device.capture.signals, (std::vector<u64>{1}));
  EXPECT_EQ(device.capture.queue->pending_query_retirement, 1);
  ASSERT_EQ(device.frame.timeline_waits.size(), 1);
  EXPECT_EQ(device.frame.timeline_waits.front().value, 1);
  EXPECT_TRUE(device.capture.destroyed_queries.empty());
  ASSERT_TRUE(device.frame.recycle(1));
  EXPECT_EQ(device.capture.waits, (std::vector<u64>{1}));
  EXPECT_EQ(device.capture.destroyed_queries, device.capture.written_queries);
}

TEST(RenderGraphExecution, ConcurrentWorkersWaitForTheSameQueryBeforeSubmitting) {
  auto device = GraphDevice{};
  auto module = Module{};
  const auto backing = module.import_buffer(test_buffer(1), Access::AccelerationStructureBuildWrite);
  const auto queries = module.transient_query_pool(VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, 1);
  const auto queried = module.write_acceleration_structure_properties(
    backing,
    graph_handle<VkAccelerationStructureKHR>(10),
    queries
  );
  const auto query_program = compile(module, queried);
  ASSERT_TRUE(device.graph.execute(query_program, device.frame));

  auto upload_module = Module{};
  const auto uploaded = upload_module.fill_buffer(upload_module.import_buffer(test_buffer(2), Access::None), 0);
  const auto upload_program = compile(upload_module, uploaded);
  auto rendezvous = std::barrier(2);
  device.capture.query_waiters = &rendezvous;
  device.context->fn.vkWaitSemaphores = [](VkDevice handle, const VkSemaphoreWaitInfo* info, u64) -> VkResult {
    auto& captured = graph_capture(handle);
    if (info->pValues[0] == 1) {
      captured.query_wait_count.fetch_add(1, std::memory_order_relaxed);
      captured.query_waiters->arrive_and_wait();
      captured.query_retired.store(true, std::memory_order_release);
    }
    return VK_SUCCESS;
  };
  device.context->fn.vkQueueSubmit2 = [](VkQueue handle, u32, const VkSubmitInfo2* info, VkFence) -> VkResult {
    auto& captured = graph_capture(handle);
    EXPECT_TRUE(captured.query_retired.load(std::memory_order_acquire));
    captured.signals.push_back(info->pSignalSemaphoreInfos[info->signalSemaphoreInfoCount - 1].value);
    return VK_SUCCESS;
  };
  auto results = std::array<std::expected<SubmitTicket, VkResult>, 2>{};
  auto workers = std::array<std::jthread, 2>{};
  for (usize index = 0; index < workers.size(); index++) {
    workers[index] = std::jthread([&device, &upload_program, &results, index]() -> void {
      auto allocator = FrameAllocator(device.persistent);
      results[index] = device.graph.execute(upload_program, allocator);
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  EXPECT_TRUE(results[0]);
  EXPECT_TRUE(results[1]);
  EXPECT_EQ(device.capture.query_wait_count.load(), 2);
  EXPECT_EQ(device.capture.signals, (std::vector<u64>{1, 2, 3}));
  EXPECT_EQ(device.capture.queue->pending_query_retirement, 0);
  // frame teardown also waits for the original query submission
  device.capture.query_waiters = nullptr;
  device.context->fn.vkWaitSemaphores = [](VkDevice, const VkSemaphoreWaitInfo*, u64) -> VkResult {
    return VK_SUCCESS;
  };
}

TEST(RenderGraphExecution, BufferImageRegionsValidateStridesAndBufferCapacity) {
  auto device = GraphDevice{};
  const auto image = test_image(1);
  const auto buffer = test_buffer(2, 128);
  const auto copied = BufferImageCopy{
    .buffer_offset = 16,
    .buffer_row_length = 8,
    .buffer_image_height = 8,
    .image_offset = {1, 2, 0},
    .image_extent = {4, 3, 1},
    .mip_level = 1,
    .base_layer = 1,
    .layer_count = 2
  };
  EXPECT_TRUE(record_copy_buffer_image(device.command(), buffer, image, copied, true).has_value());
  EXPECT_TRUE(record_copy_buffer_image(device.command(), buffer, image, copied, false).has_value());
  ASSERT_EQ(device.capture.uploads.size(), 1);
  EXPECT_EQ(device.capture.uploads[0].bufferOffset, 144);
  EXPECT_EQ(device.capture.uploads[0].imageSubresource.layerCount, 2);
  auto too_small = buffer;
  // 256 bytes between layers + 64 between rows + 16 bytes in the last row
  too_small.size = 16 + 256 + 64 + 15;
  EXPECT_FALSE(record_copy_buffer_image(device.command(), too_small, image, copied, true).has_value());
  auto wrong_stride = copied;
  wrong_stride.buffer_row_length = 3;
  EXPECT_FALSE(record_copy_buffer_image(device.command(), buffer, image, wrong_stride, true).has_value());
  EXPECT_EQ(device.capture.uploads.size(), 1);
}

TEST(RenderGraphExecution, CompressedCopiesCheckBlockAlignmentAndLayerFootprint) {
  auto device = GraphDevice{};
  auto info = untyped_info();
  info.extent = {8, 8, 1};
  info.format = vir::Format::BC7UnormBlock;
  const auto image = ImageAttachment::from_image(
    Image::from_info(graph_handle<VkImage>(1), info),
    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
  );
  auto buffer = test_buffer(2, 128);
  buffer.size = 64;
  EXPECT_TRUE(record_copy_buffer_image(device.command(), buffer, image, nullopt, true).has_value());
  buffer.size = 63;
  EXPECT_FALSE(record_copy_buffer_image(device.command(), buffer, image, nullopt, true).has_value());
  EXPECT_FALSE(record_copy_buffer_image(device.command(), test_buffer(2, 4), image, nullopt, true).has_value());
  const auto partial_block = BufferImageCopy{.image_extent = {3, 4, 1}, .layer_count = 1};
  EXPECT_FALSE(record_copy_buffer_image(device.command(), test_buffer(2), image, partial_block, true).has_value());
  auto edge = image;
  edge.extent.width = 7;
  edge.image.extent.width = 7;
  const auto edge_block = BufferImageCopy{.image_offset = {4, 0, 0}, .image_extent = {3, 4, 1}, .layer_count = 1};
  EXPECT_TRUE(record_copy_buffer_image(device.command(), test_buffer(2), edge, edge_block, true).has_value());
  EXPECT_EQ(device.capture.uploads.size(), 2);
}

TEST(RenderGraphExecution, TransferRecorderCoversImageCopiesResolvesUploadsAndReadbacks) {
  auto device = GraphDevice{};
  auto module = Module{};
  const auto image = module.import_attachment(test_image(1), Access::None);
  const auto src = module.import_attachment(test_image(2), Access::CopyRead);
  const auto msaa = module.import_attachment(test_image(3, vir::Samples::e4), Access::ResolveRead);
  const auto buffer = module.import_buffer(test_buffer(4), Access::HostWrite);
  const auto callback = module.declare_callback_var("image transfers");
  module
    .begin_compute(
      {{image, Access::CopyRW | Access::ResolveWrite},
       {src, Access::CopyRead},
       {msaa, Access::ResolveRead},
       {buffer, Access::CopyRW}}
    )
    .record_from(callback);
  const auto outputs = module.end_compute<4>();
  auto program = compile(module, outputs[0]);
  program.set_callback(callback, [image, src, msaa, buffer](Recorder& recorder) -> void {
    const auto region = ImageCopy{.layer_count = 1, .extent = {4, 4, 1}};
    recorder.copy_image(recorder.image(src), recorder.image(image), region);
    recorder.resolve(recorder.image(msaa), recorder.image(image), region);
    const auto copied = BufferImageCopy{.image_extent = {4, 4, 1}, .layer_count = 1};
    recorder.copy_buffer_to_image(recorder.buffer(buffer), recorder.image(image), copied);
    recorder.copy_image_to_buffer(recorder.image(image), recorder.buffer(buffer), copied);
  });
  ASSERT_TRUE(device.graph.execute(program, device.frame).has_value());
  EXPECT_EQ(device.capture.copies.size(), 1);
  EXPECT_EQ(device.capture.resolves.size(), 1);
  EXPECT_EQ(device.capture.uploads.size(), 1);
  EXPECT_EQ(device.capture.readbacks.size(), 1);
  EXPECT_GE(device.capture.memory.size(), 3);
  EXPECT_EQ(
    device.capture.transfer_layouts,
    (std::vector<std::pair<VkImageLayout, VkImageLayout>>{
      {VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL},
      {VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL}
    })
  );
}

TEST(RenderGraphExecution, BindingImageFormatOverridePreservesTheResourceRange) {
  auto module = Module{};
  auto info = untyped_info();
  info.format = vir::Format::R32Sfloat;
  info.mip_levels = 4;
  const auto image = module.transient_image(info);
  const auto ranged = module.subrange(image, 1, 1, 0, 1);
  const auto declared = Declared::with_access(
    {{0, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}},
    vir::ShaderStage::Compute,
    Access::ComputeRead
  );
  module.begin_compute({{ranged, Access::ComputeRead}})
    .bind_compute_pipeline(pipeline(0))
    .bind_image(0, 0, ranged, vir::Format::R32Uint)
    .dispatch(1, 1, 1);
  const auto results = module.end_compute<1>();
  const auto program = module.compile(declared.pipelines(), results[0]);
  ASSERT_TRUE(program.has_value());
  const auto views = find_all<ir::View>(*program);
  EXPECT_TRUE(std::ranges::any_of(views, [](const ir::View* view) -> bool {
    return view->format == vir::Format::R32Uint &&
           view->range == Subresource{.base_level = 1, .level_count = 1, .base_layer = 0, .layer_count = 1};
  }));
}

TEST(RenderGraphExecution, ProfilerHooksBracketNamedPasses) {
  auto device = GraphDevice{};
  auto events = std::vector<std::string>{};
  device.graph.callbacks = {
    .user = &events,
    .on_begin_command_buffer = [](void* user, VkCommandBuffer) -> void {
      static_cast<std::vector<std::string>*>(user)->push_back("command");
    },
    .on_begin_pass = [](void* user, std::string_view name, VkCommandBuffer) -> void* {
      static_cast<std::vector<std::string>*>(user)->emplace_back(name);
      return user;
    },
    .on_end_pass = [](void* user, void* data, VkCommandBuffer) -> void {
      EXPECT_EQ(user, data);
      static_cast<std::vector<std::string>*>(user)->push_back("end");
    }
  };
  auto module = Module{};
  const auto buffer = module.import_buffer(test_buffer(1), Access::None);
  module.begin_compute({{buffer, Access::ComputeRead}}).with_name("pass");
  const auto result = module.end_compute<1>();
  const auto program = compile(module, result[0]);
  ASSERT_TRUE(device.graph.execute(program, device.frame).has_value());
  EXPECT_EQ(events, (std::vector<std::string>{"command", "pass", "end"}));
  device.graph.callbacks.on_end_pass = nullptr;
  EXPECT_TRUE(device.graph.execute(program, device.frame).has_value());
}
TEST(RenderGraphExecution, ConsecutiveBarriersKeepDisjointMipsInOneVulkanCall) {
  auto device = GraphDevice{};
  auto module = Module{};
  const auto image = module.import_attachment(test_image(10), Access::CopyRead);
  const auto callback = module.declare_callback_var("boundary");
  module.begin_compute(
          {{module.mip(image, 0), Access::ComputeSampled}, {module.mip(image, 1), Access::ComputeWrite}}
  ).record_from(callback);
  auto executable = compile(module, module.end_compute<2>()[1]);
  executable.set_callback(callback, [](Recorder&) -> void {});
  ASSERT_TRUE(device.graph.execute(executable, device.frame).has_value());
  ASSERT_EQ(device.capture.dependency_calls.size(), 1u);
  EXPECT_EQ(device.capture.dependency_calls[0].second, 2u);
}

TEST(RenderGraphExecution, BoundAliasesSplitOnlyWhenTheirActualRangesOverlap) {
  for (const auto second_mip : {0_u32, 1_u32}) {
    SCOPED_TRACE(second_mip);
    auto device = GraphDevice{};
    auto module = Module{};
    const auto first = module.declare_image_var("first", FORMAT, Access::CopyRead);
    const auto second = module.declare_image_var("second", FORMAT, Access::ComputeRead);
    const auto callback = module.declare_callback_var("boundary");
    module.begin_compute({{first, Access::ComputeRead}, {second, Access::ComputeSampled}}).record_from(callback);
    auto executable = compile(module, module.end_compute<2>()[1]);
    auto attachment = test_image(10);
    attachment.subresource_range.levelCount = 1;
    executable.set(first, attachment);
    attachment.subresource_range.baseMipLevel = second_mip;
    attachment.layout = VK_IMAGE_LAYOUT_GENERAL;
    executable.set(second, attachment);
    executable.set_callback(callback, [](Recorder&) -> void {});
    ASSERT_TRUE(device.graph.execute(executable, device.frame).has_value());
    ASSERT_EQ(device.capture.dependency_calls.size(), second_mip == 0 ? 2u : 1u);
    EXPECT_EQ(device.capture.images.size(), 2u);
  }
}

TEST(RenderGraphExecution, ImageRunsEndAtAnyOtherInstructionAndAtProgramEnd) {
  for (const auto boundary : {0_u32, 1_u32, 2_u32, 3_u32}) {
    SCOPED_TRACE(boundary);
    auto device = GraphDevice{};
    auto module = Module{};
    const auto image = module.import_attachment(test_image(10), Access::CopyRead);
    const auto src = module.lower_access(Access::CopyRead);
    const auto dst = module.lower_access(Access::ComputeWrite);
    module.emit(
      ir::ImageBarrier{
        .src_access = src,
        .dst_access = dst,
        .old_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .new_layout = VK_IMAGE_LAYOUT_GENERAL,
        .value = image,
        .range = Subresource::mip(0),
      }
    );
    if (boundary == 1) {
      module.emit(ir::MemoryBarrier{.src_access = src, .dst_access = dst});
    } else if (boundary == 2) {
      module.emit(ir::Label{.label = LabelID{0}});
    } else if (boundary == 3) {
      module.emit(ir::Constant::from_u32(42));
    }
    module.emit(
      ir::ImageBarrier{
        .src_access = src,
        .dst_access = dst,
        .old_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .new_layout = VK_IMAGE_LAYOUT_GENERAL,
        .value = image,
        .range = Subresource::mip(1),
      }
    );
    const auto program = unoptimized_program(std::move(module));
    ASSERT_TRUE(device.graph.execute(program, device.frame).has_value());
    EXPECT_EQ(
      device.capture.dependency_calls,
      boundary == 0   ? (std::vector<std::pair<u32, u32>>{{0, 2}})
      : boundary == 1 ? (std::vector<std::pair<u32, u32>>{{0, 1}, {1, 0}, {0, 1}})
                      : (std::vector<std::pair<u32, u32>>{{0, 1}, {0, 1}})
    );
  }
}

TEST(RenderGraphExecution, OverlappingTransitionsOnOneRootUseSeparateCalls) {
  for (const auto range : {Subresource::mip(0), Subresource::whole()}) {
    auto device = GraphDevice{};
    auto module = Module{};
    const auto image = module.import_attachment(test_image(10), Access::CopyRead);
    const auto src = module.lower_access(Access::CopyRead);
    const auto written = module.lower_access(Access::ComputeWrite);
    const auto sampled = module.lower_access(Access::ComputeSampled);
    module.emit(
      ir::ImageBarrier{
        .src_access = src,
        .dst_access = written,
        .old_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .new_layout = VK_IMAGE_LAYOUT_GENERAL,
        .value = image,
        .range = range,
      }
    );
    module.emit(
      ir::ImageBarrier{
        .src_access = written,
        .dst_access = sampled,
        .old_layout = VK_IMAGE_LAYOUT_GENERAL,
        .new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .value = image,
        .range = Subresource::mip(0),
      }
    );
    const auto program = unoptimized_program(std::move(module));
    ASSERT_TRUE(device.graph.execute(program, device.frame).has_value());
    EXPECT_EQ(device.capture.dependency_calls, (std::vector<std::pair<u32, u32>>{{0, 1}, {0, 1}}));
    ASSERT_EQ(device.capture.images.size(), 2u);
    EXPECT_EQ(device.capture.images[0].newLayout, device.capture.images[1].oldLayout);
  }
}

TEST(RenderGraphExecution, MemoryRunsCombineDifferentMasksAndStopAtCommands) {
  for (const auto separate : {false, true}) {
    SCOPED_TRACE(separate);
    auto device = GraphDevice{};
    auto module = Module{};
    const auto buffer = module.import_buffer(test_buffer(1), Access::None);
    const auto host_write = module.lower_access(Access::HostWrite);
    const auto copy_write = module.lower_access(Access::CopyWrite);
    const auto attributes = module.lower_access(Access::AttributeRead);
    const auto indices = module.lower_access(Access::IndexRead);
    module.emit(ir::MemoryBarrier{.src_access = host_write, .dst_access = attributes});
    if (separate) {
      module.emit(ir::FillBuffer{.buffer = buffer});
    }
    module.emit(ir::MemoryBarrier{.src_access = copy_write, .dst_access = indices});
    const auto program = unoptimized_program(std::move(module));
    ASSERT_TRUE(device.graph.execute(program, device.frame).has_value());
    ASSERT_EQ(device.capture.memory.size(), separate ? 2u : 1u);
    ASSERT_EQ(device.capture.dependency_calls.size(), separate ? 2u : 1u);
    const auto first = CommandBuffer::memory_barrier_info(
      separate ? Access::HostWrite : Access::HostWrite | Access::CopyWrite,
      separate ? Access::AttributeRead : Access::AttributeRead | Access::IndexRead
    );
    EXPECT_EQ(device.capture.memory[0].srcStageMask, first.srcStageMask);
    EXPECT_EQ(device.capture.memory[0].srcAccessMask, first.srcAccessMask);
    EXPECT_EQ(device.capture.memory[0].dstStageMask, first.dstStageMask);
    EXPECT_EQ(device.capture.memory[0].dstAccessMask, first.dstAccessMask);
    if (separate) {
      const auto second = CommandBuffer::memory_barrier_info(Access::CopyWrite, Access::IndexRead);
      EXPECT_EQ(device.capture.memory[1].srcAccessMask, second.srcAccessMask);
      EXPECT_EQ(device.capture.memory[1].dstAccessMask, second.dstAccessMask);
    }
  }
}

TEST(RenderGraphExecution, ImageRunFlushesBeforeReturnAndSkipsUnreachableBarriers) {
  auto device = GraphDevice{};
  auto module = Module{};
  const auto image = module.import_attachment(test_image(10), Access::CopyRead);
  const auto src = module.lower_access(Access::CopyRead);
  const auto dst = module.lower_access(Access::ComputeWrite);
  const auto barrier = ir::ImageBarrier{
    .src_access = src,
    .dst_access = dst,
    .old_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    .new_layout = VK_IMAGE_LAYOUT_GENERAL,
    .value = image,
    .range = Subresource::mip(0),
  };
  module.emit(barrier);
  module.emit(ir::Return{});
  module.emit(barrier);
  const auto program = unoptimized_program(std::move(module));
  ASSERT_TRUE(device.graph.execute(program, device.frame).has_value());
  EXPECT_EQ(device.capture.dependency_calls, (std::vector<std::pair<u32, u32>>{{0, 1}}));
}

} // namespace ox
