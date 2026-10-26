#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <vir/RenderGraph.hpp>
#include <vir/Types.hpp>

#include "Asset/AssetFile.hpp"
#include "Asset/Texture.hpp"
#include "Render/RenderContext.hpp"
#include "Render/RenderResources.hpp"
#include "Render/RendererInstance.hpp"
#include "Render/Utils/PushConstants.hpp"
#include "Render/Utils/Samplers.hpp"
#include "Render/Utils/TextureFormat.hpp"

namespace ox {
struct ContextCapture {
  vir::CommandQueue* queue = nullptr;
  u64 submitted = 0;
  u64 completed = 0;
  std::vector<u64> waits = {};
  std::vector<u32> fills = {};
  std::vector<VkBuffer> filled_buffers = {};
  std::vector<VkBufferCopy> copies = {};
  std::vector<VkMemoryBarrier2> memory = {};
  usize last_upload_barrier_count = 0;
  std::vector<VkSampler> destroyed_samplers = {};
  std::vector<VkImageView> destroyed_views = {};
  std::vector<VkDescriptorImageInfo> descriptor_images = {};
  std::vector<std::pair<u32, u32>> descriptor_indices = {};
  VkImageViewCreateInfo view_info = {};
  VkSamplerCreateInfo sampler_info = {};
  u32 handles = 100;
  u32 pools_created = 0;
  u32 pools_destroyed = 0;
  u32 layouts_destroyed = 0;
  VkResult layout_result = VK_SUCCESS;
  VkResult set_result = VK_SUCCESS;
};

template <typename T>
static auto context_capture(T handle) -> ContextCapture& {
  return *reinterpret_cast<ContextCapture*>(handle);
}

template <typename T>
static auto context_handle(u64 value) -> T {
  return reinterpret_cast<T>(value);
}

static auto context_engine_pack() -> option<AssetFile> {
  auto error = std::error_code{};
  for (const auto& directory :
       {std::filesystem::current_path(error), std::filesystem::canonical("/proc/self/exe", error).parent_path()}) {
    const auto path = directory / "Assets" / "Shaders" / "engine.oxpack";
    if (std::filesystem::exists(path, error)) {
      return AssetFile::unpack(path);
    }
  }
  return nullopt;
}

static auto stub_render_context(ContextCapture& capture) -> std::unique_ptr<vir::Context> {
  auto context = std::make_unique<vir::Context>();
  context->device = reinterpret_cast<VkDevice>(&capture);
  auto queue = std::make_unique<vir::CommandQueue>();
  queue->handle = reinterpret_cast<VkQueue>(&capture);
  queue->domain = vir::DomainFlag::Graphics;
  queue->family_index = 3;
  queue->timeline = context_handle<VkSemaphore>(1);
  capture.queue = queue.get();
  context->queues.push_back(std::move(queue));
  auto& fn = context->fn;
  fn.vkDeviceWaitIdle = [](VkDevice device) -> VkResult {
    auto& captured = context_capture(device);
    captured.completed = captured.submitted;
    return VK_SUCCESS;
  };
  fn.vkDestroySemaphore = [](VkDevice, VkSemaphore, const VkAllocationCallbacks*) -> void {
  };
  fn.vkWaitSemaphores = [](VkDevice device, const VkSemaphoreWaitInfo* info, u64) -> VkResult {
    auto& captured = context_capture(device);
    const auto unlocked = captured.queue->submit_mutex.try_lock();
    EXPECT_TRUE(unlocked);
    if (unlocked) {
      captured.queue->submit_mutex.unlock();
    }
    captured.waits.push_back(info->pValues[0]);
    captured.completed = std::max(captured.completed, info->pValues[0]);
    return VK_SUCCESS;
  };
  fn.vkGetSemaphoreCounterValue = [](VkDevice device, VkSemaphore, u64* value) -> VkResult {
    *value = context_capture(device).completed;
    return VK_SUCCESS;
  };
  fn.vkCreateCommandPool =
    [](VkDevice device, const VkCommandPoolCreateInfo*, const VkAllocationCallbacks*, VkCommandPool* pool) -> VkResult {
    *pool = context_handle<VkCommandPool>(++context_capture(device).handles);
    return VK_SUCCESS;
  };
  fn.vkDestroyCommandPool = [](VkDevice, VkCommandPool, const VkAllocationCallbacks*) -> void {
  };
  fn.vkResetCommandPool = [](VkDevice, VkCommandPool, VkCommandPoolResetFlags) -> VkResult {
    return VK_SUCCESS;
  };
  fn.vkFreeCommandBuffers = [](VkDevice, VkCommandPool, u32, const VkCommandBuffer*) -> void {
  };
  fn.vkAllocateCommandBuffers =
    [](VkDevice device, const VkCommandBufferAllocateInfo*, VkCommandBuffer* command) -> VkResult {
    *command = reinterpret_cast<VkCommandBuffer>(device);
    return VK_SUCCESS;
  };
  fn.vkBeginCommandBuffer = [](VkCommandBuffer, const VkCommandBufferBeginInfo*) -> VkResult {
    return VK_SUCCESS;
  };
  fn.vkEndCommandBuffer = [](VkCommandBuffer) -> VkResult {
    return VK_SUCCESS;
  };
  fn.vkQueueSubmit2 = [](VkQueue submitted, u32, const VkSubmitInfo2* info, VkFence) -> VkResult {
    context_capture(submitted).submitted = info->pSignalSemaphoreInfos[info->signalSemaphoreInfoCount - 1].value;
    return VK_SUCCESS;
  };
  fn.vkCmdPipelineBarrier2 = [](VkCommandBuffer command, const VkDependencyInfo* info) -> void {
    auto& captured = context_capture(command);
    for (u32 index = 0; index < info->memoryBarrierCount; index++) {
      captured.memory.push_back(info->pMemoryBarriers[index]);
    }
  };
  fn.vkCmdFillBuffer = [](VkCommandBuffer command, VkBuffer buffer, u64, u64, u32 data) -> void {
    auto& captured = context_capture(command);
    captured.fills.push_back(data);
    captured.filled_buffers.push_back(buffer);
    captured.last_upload_barrier_count = captured.memory.size();
  };
  fn.vkCmdCopyBuffer = [](VkCommandBuffer command, VkBuffer, VkBuffer, u32 count, const VkBufferCopy* regions) -> void {
    auto& captured = context_capture(command);
    for (u32 index = 0; index < count; index++) {
      captured.copies.push_back(regions[index]);
    }
    captured.last_upload_barrier_count = captured.memory.size();
  };
  fn.vkCreateDescriptorPool =
    [](VkDevice device, const VkDescriptorPoolCreateInfo*, const VkAllocationCallbacks*, VkDescriptorPool* pool)
    -> VkResult {
    auto& captured = context_capture(device);
    captured.pools_created++;
    *pool = context_handle<VkDescriptorPool>(++captured.handles);
    return VK_SUCCESS;
  };
  fn.vkDestroyDescriptorPool = [](VkDevice device, VkDescriptorPool, const VkAllocationCallbacks*) -> void {
    context_capture(device).pools_destroyed++;
  };
  fn.vkCreateDescriptorSetLayout = [](
                                     VkDevice device,
                                     const VkDescriptorSetLayoutCreateInfo*,
                                     const VkAllocationCallbacks*,
                                     VkDescriptorSetLayout* layout
                                   ) -> VkResult {
    auto& captured = context_capture(device);
    *layout = context_handle<VkDescriptorSetLayout>(++captured.handles);
    return captured.layout_result;
  };
  fn.vkDestroyDescriptorSetLayout = [](VkDevice device, VkDescriptorSetLayout, const VkAllocationCallbacks*) -> void {
    context_capture(device).layouts_destroyed++;
  };
  fn.vkAllocateDescriptorSets =
    [](VkDevice device, const VkDescriptorSetAllocateInfo*, VkDescriptorSet* set) -> VkResult {
    auto& captured = context_capture(device);
    *set = context_handle<VkDescriptorSet>(++captured.handles);
    return captured.set_result;
  };
  fn.vkUpdateDescriptorSets =
    [](VkDevice device, u32 count, const VkWriteDescriptorSet* writes, u32, const VkCopyDescriptorSet*) -> void {
    auto& captured = context_capture(device);
    for (u32 index = 0; index < count; index++) {
      captured.descriptor_images.push_back(*writes[index].pImageInfo);
      captured.descriptor_indices.emplace_back(writes[index].dstBinding, writes[index].dstArrayElement);
    }
  };
  fn.vkCreateImageView =
    [](VkDevice device, const VkImageViewCreateInfo* info, const VkAllocationCallbacks*, VkImageView* view)
    -> VkResult {
    auto& captured = context_capture(device);
    captured.view_info = *info;
    captured.view_info.pNext = nullptr;
    *view = context_handle<VkImageView>(++captured.handles);
    return VK_SUCCESS;
  };
  fn.vkDestroyImageView = [](VkDevice device, VkImageView view, const VkAllocationCallbacks*) -> void {
    context_capture(device).destroyed_views.push_back(view);
  };
  fn.vkCreateSampler =
    [](VkDevice device, const VkSamplerCreateInfo* info, const VkAllocationCallbacks*, VkSampler* sampler) -> VkResult {
    auto& captured = context_capture(device);
    captured.sampler_info = *info;
    *sampler = context_handle<VkSampler>(++captured.handles);
    return VK_SUCCESS;
  };
  fn.vkDestroySampler = [](VkDevice device, VkSampler sampler, const VkAllocationCallbacks*) -> void {
    context_capture(device).destroyed_samplers.push_back(sampler);
  };
  return context;
}

// contextcvar persists on destruction, keep each test away from the editor's preferences
struct ContextTestDirectory {
  std::filesystem::path previous = std::filesystem::current_path();
  std::filesystem::path path = std::filesystem::temp_directory_path() /
                               ("ox_render_context_" +
                                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

  ContextTestDirectory() {
    std::filesystem::create_directory(path);
    std::filesystem::current_path(path);
  }
  ~ContextTestDirectory() {
    std::filesystem::current_path(previous);
    std::filesystem::remove_all(path);
  }
};

struct ContextDevice {
  ContextTestDirectory directory = {};
  ContextCapture capture = {};
  RenderContext context = {};

  ContextDevice() { EXPECT_TRUE(context.initialize_graph(stub_render_context(capture))); }

  auto bindless(this ContextDevice& self, u32 capacity = 8) -> void {
    const auto bindings = std::array{
      VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_SAMPLER, capacity, VK_SHADER_STAGE_ALL, nullptr},
      VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, capacity, VK_SHADER_STAGE_ALL, nullptr},
      VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, capacity, VK_SHADER_STAGE_ALL, nullptr},
    };
    const auto flags = std::array<VkDescriptorBindingFlags, 3>{
      VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
      VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
      VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
    };
    auto descriptor = self.context.create_persistent_descriptor_set(bindings, flags);
    ASSERT_TRUE(descriptor);
    self.context.resources->descriptor_set = std::move(*descriptor);
  }
};

static auto context_fill_module(u32 data) -> vir::Module {
  auto module = vir::Module{};
  const auto buffer = vir::Buffer{.handle = context_handle<VkBuffer>(10), .size = 16};
  const auto filled = module.fill_buffer(module.import_buffer(buffer, vir::Access::None), data);
  module.export_to(filled, vir::Access::HostRead);
  return module;
}

TEST(RenderContext, ReusesProgramsAndRetiresFrameSlotsByTimeline) {
  auto device = ContextDevice{};
  auto program = device.context.compile(context_fill_module(42));
  ASSERT_TRUE(program);
  ASSERT_TRUE(device.context.execute(*program));
  EXPECT_EQ(device.capture.fills, (std::vector<u32>{42}));
  const auto first = vir::SubmitTicket{vir::QueueID{0}, 1};
  EXPECT_FALSE(device.context.is_retired(first));
  for (u32 index = 0; index < device.context.num_inflight_frames; index++) {
    ASSERT_TRUE(device.context.new_frame());
    ASSERT_TRUE(device.context.execute(*program));
  }
  EXPECT_TRUE(device.context.is_retired(first));
  EXPECT_EQ(device.capture.submitted, 4);
  EXPECT_EQ(device.context.num_frames, 3);
}

TEST(RenderContext, BlockingUploadsWaitOutsideTheSubmissionLock) {
  auto device = ContextDevice{};
  auto ticket = device.context.execute_blocking(context_fill_module(7));
  ASSERT_TRUE(ticket);
  EXPECT_EQ(ticket->value, 1);
  EXPECT_TRUE(device.context.is_retired(*ticket));
  EXPECT_EQ(device.capture.fills, (std::vector<u32>{7}));
  EXPECT_EQ(device.capture.waits.front(), 1);
}

TEST(RenderContext, ProgramsBindBuffersAndReadCurrentCallbackDataWithoutRecordingAgain) {
  auto device = ContextDevice{};
  auto module = vir::Module{};
  auto buffer = vir::Buffer{.handle = context_handle<VkBuffer>(10), .size = 16};
  auto data = 7_u32;
  const auto unused = module.declare_bytes_var("unused push constants", sizeof(u32));
  const auto target = module.declare_buffer_var("readback", vir::Access::HostRead);
  const auto condition = module.declare_bool_var("enabled", false);
  const auto body = module.declare_callback_var("fill readback", [&data, target](vir::Recorder& recorder) -> void {
    recorder.fill_buffer(recorder.buffer(target), data);
  });
  const auto written = module.set_condition(
    condition,
    [&](vir::Module& arm) -> vir::ValueID {
      return arm.begin_compute({{target, vir::Access::Clear}}).record_from(body).end_compute()[0];
    },
    [&](vir::Module&) -> vir::ValueID { return target; }
  );
  module.export_to(written, vir::Access::HostRead);
  auto program = device.context.compile(std::move(module));
  ASSERT_TRUE(program);
  const auto instruction_count = program->instructions.size();
  const auto variable_count = program->variables.size();
  EXPECT_FALSE(program->has_variable(unused));
  program->set_bytes(unused, data);
  EXPECT_EQ(program->variables.size(), variable_count);
  ASSERT_TRUE(device.context.new_frame());
  program->set(target, buffer);
  program->set(condition, true);
  ASSERT_TRUE(device.context.execute(*program));
  data = 9;
  buffer.handle = context_handle<VkBuffer>(11);
  program->set(target, buffer);
  ASSERT_TRUE(device.context.execute(*program));
  program->set(condition, false);
  ASSERT_TRUE(device.context.execute(*program));
  EXPECT_EQ(device.capture.fills, (std::vector<u32>{7, 9}));
  EXPECT_EQ(
    device.capture.filled_buffers,
    (std::vector<VkBuffer>{context_handle<VkBuffer>(10), context_handle<VkBuffer>(11)})
  );
  EXPECT_EQ(program->instructions.size(), instruction_count);
}

TEST(RenderContext, RecordedUploadsSynchronizeFillsAndCopiesBeforeTaskShaderReads) {
  auto device = ContextDevice{};
  auto module = vir::Module{};
  auto consumer_barrier_count = 0_sz;
  auto is_filling_var = vir::ValueID::Invalid;
  auto upload = UploadVariables{};
  const auto destination_var = module.declare_buffer_var("visibility mask", vir::Access::MemoryRW);
  const auto filled = RendererInstance::record_fill(module, "visibility mask fill", destination_var, 7, is_filling_var);
  const auto uploaded = RendererInstance::record_upload(module, "visibility mask upload", filled, upload);
  const auto consumer = module.declare_callback_var(
    "task shader read",
    [&device, &consumer_barrier_count](vir::Recorder&) -> void {
      consumer_barrier_count = device.capture.memory.size();
    }
  );
  module.export_to(
    module.begin_compute({{uploaded, vir::Access::TaskRW}}).record_from(consumer).end_compute()[0],
    vir::Access::MemoryRW
  );
  auto compiled = device.context.compile(std::move(module));
  ASSERT_TRUE(compiled.has_value());
  auto& program = *compiled;
  const auto instruction_count = program.instructions.size();

  // the same program must cover a fill, a copy, both, and a frame with nothing to upload
  for (const auto scenario : {0_u32, 1_u32, 2_u32, 3_u32}) {
    SCOPED_TRACE(scenario);
    const auto destination = vir::Buffer{.handle = context_handle<VkBuffer>(10 + scenario), .size = 64};
    const auto is_filling = scenario == 0 || scenario == 2;
    const auto is_uploading = scenario == 1 || scenario == 2;
    program.set(destination_var, destination);
    program.set(is_filling_var, is_filling);
    program.set(upload.staging, vir::Buffer{.handle = context_handle<VkBuffer>(20), .size = 16});
    program.set(upload.is_uploading, is_uploading);
    device.capture.fills.clear();
    device.capture.filled_buffers.clear();
    device.capture.copies.clear();
    device.capture.memory.clear();
    device.capture.last_upload_barrier_count = 0;
    consumer_barrier_count = 0;
    ASSERT_TRUE(device.context.execute(program).has_value());
    EXPECT_EQ(program.instructions.size(), instruction_count);

    EXPECT_EQ(device.capture.fills.size(), is_filling ? 1 : 0);
    if (is_filling) {
      EXPECT_EQ(device.capture.fills.front(), 7);
      EXPECT_EQ(device.capture.filled_buffers.front(), destination.handle);
    }

    EXPECT_EQ(device.capture.copies.size(), is_uploading ? 1 : 0);
    if (is_uploading) {
      EXPECT_EQ(device.capture.copies.front().srcOffset, 0);
      EXPECT_EQ(device.capture.copies.front().dstOffset, 0);
      EXPECT_EQ(device.capture.copies.front().size, 16);
    }

    if (scenario == 3) {
      continue;
    }

    ASSERT_GT(consumer_barrier_count, device.capture.last_upload_barrier_count);
    const auto barriers = std::span(device.capture.memory)
                            .subspan(
                              device.capture.last_upload_barrier_count,
                              consumer_barrier_count - device.capture.last_upload_barrier_count
                            );
    // the write reaches the task shader directly or through a chain, a skipped copy arm still moves the mask
    // to the copy state the taken arm leaves it in
    const auto required_stage = scenario == 0 ? VK_PIPELINE_STAGE_2_CLEAR_BIT : VK_PIPELINE_STAGE_2_COPY_BIT;
    auto reached_stages = VkPipelineStageFlags2{0};
    auto is_task_visible = false;
    for (const auto& barrier : barriers) {
      const auto is_first = (barrier.srcStageMask & required_stage) != 0 &&
                            (barrier.srcAccessMask & VK_ACCESS_2_TRANSFER_WRITE_BIT) != 0;
      if (!is_first && (barrier.srcStageMask & reached_stages) == 0) {
        continue;
      }

      reached_stages |= barrier.dstStageMask;
      if (
        (barrier.dstStageMask & VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT) != 0 &&
        (barrier.dstAccessMask & VK_ACCESS_2_SHADER_STORAGE_READ_BIT) != 0
      ) {
        is_task_visible = true;
      }
    }

    EXPECT_TRUE(is_task_visible) << program.dump();
  }
}

TEST(RenderContext, HiZUsesOriginalSpdInOneDispatch) {
  const auto pack = context_engine_pack();
  ASSERT_TRUE(pack);
  for (const auto size :
       {vir::Extent3D{257, 257, 1},
        vir::Extent3D{301, 301, 1},
        vir::Extent3D{1622, 717, 1},
        vir::Extent3D{4096, 1024, 1}}) {
    SCOPED_TRACE(size.width);
    SCOPED_TRACE(size.height);
    auto device = ContextDevice{};
    for (const auto& entry : pack->entries) {
      const auto* shader = std::get_if<ShaderPipelineData>(&entry.data);
      if (shader && shader->module_name == "hiz") {
        ASSERT_TRUE(device.context.create_pipeline(*shader));
      }
    }
    device.bindless();
    const auto sampler = device.context.allocate_sampler(NearestSamplerClamped, false);
    ASSERT_TRUE(sampler);
    device.context.resources->cached_samplers.emplace_back(NearestSamplerClamped, *sampler);
    auto module = vir::Module{};
    const auto extent = vir::Extent3D{std::bit_ceil((size.width + 1) / 2), std::bit_ceil((size.height + 1) / 2), 1};
    const auto mip_count = Texture::calculate_mip_count(extent);
    auto geometry = MainGeometryContext{
      .depth_attachment = module.clear(
        module.transient_image({.extent = size, .format = vir::Format::D32Sfloat}),
        vir::ClearValue::depth(0.0f)
      ),
      .hiz_attachment = module.transient_image(
        {.extent = extent, .format = vir::Format::R32Sfloat, .mip_levels = std::min(mip_count, 13_u32)}
      ),
    };
    RendererInstance::generate_hiz(module, device.context, geometry);
    module.export_to(geometry.hiz_attachment, vir::Access::TaskSampled);
    auto compiled = device.context.compile(std::move(module));
    ASSERT_TRUE(compiled);
    const auto& program = *compiled;
    const auto dispatches = std::ranges::count_if(program.instructions, [](const vir::Instr& instruction) -> bool {
      return std::holds_alternative<vir::ir::Dispatch>(instruction.ir);
    });
    EXPECT_EQ(dispatches, 1);
    const auto updates = std::ranges::count_if(program.instructions, [](const vir::Instr& instruction) -> bool {
      const auto* update = std::get_if<vir::ir::UpdateBuffer>(&instruction.ir);
      return update && update->data == std::vector<u8>(sizeof(u32), 0);
    });
    EXPECT_EQ(updates, 1);
    const auto access = [&program](vir::ValueID id) -> vir::Access {
      const auto instruction = std::ranges::find(program.instructions, id, &vir::Instr::id);
      return std::get<vir::ir::Constant>(instruction->ir).as_access();
    };
    auto found_consumer = false;
    for (const auto& instruction : program.instructions) {
      if (const auto* barrier = std::get_if<vir::ir::ImageBarrier>(&instruction.ir)) {
        if (
          module.resource_root(barrier->value) == module.resource_root(geometry.hiz_attachment) &&
          vir::writes(access(barrier->src_access)) && access(barrier->dst_access) == vir::Access::TaskSampled
        ) {
          found_consumer = true;
        }
      }
    }
    EXPECT_TRUE(found_consumer) << program.dump();
  }
}

TEST(RenderContext, DescriptorFailuresReleasePreviouslyCreatedObjects) {
  auto device = ContextDevice{};
  const auto bindings = std::array{
    VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_ALL, nullptr}
  };
  const auto flags = std::array<VkDescriptorBindingFlags, 1>{VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT};
  device.capture.layout_result = VK_ERROR_OUT_OF_HOST_MEMORY;
  EXPECT_FALSE(device.context.create_persistent_descriptor_set(bindings, flags));
  EXPECT_EQ(device.capture.pools_destroyed, 1);
  EXPECT_EQ(device.capture.layouts_destroyed, 0);
  device.capture.layout_result = VK_SUCCESS;
  device.capture.set_result = VK_ERROR_OUT_OF_POOL_MEMORY;
  EXPECT_FALSE(device.context.create_persistent_descriptor_set(bindings, flags));
  EXPECT_EQ(device.capture.pools_destroyed, 2);
  EXPECT_EQ(device.capture.layouts_destroyed, 1);
}

TEST(RenderContext, RetiredBindlessSlotsStayReservedUntilTheirLastSubmissionFinishes) {
  auto device = ContextDevice{};
  device.bindless(2);
  const auto sampler = device.context.allocate_sampler(LinearSamplerRepeated);
  ASSERT_TRUE(sampler);
  const auto handle = device.context.sampler(*sampler).handle;
  const auto program = device.context.compile(context_fill_module(0));
  ASSERT_TRUE(program);
  const auto ticket = device.context.execute(*program);
  ASSERT_TRUE(ticket);
  device.context.destroy_sampler(*sampler);
  device.context.destroy_sampler(*sampler);
  EXPECT_EQ(device.context.sampler(*sampler).handle, VK_NULL_HANDLE);
  device.context.collect_retired();
  EXPECT_TRUE(device.capture.destroyed_samplers.empty());
  const auto second = device.context.allocate_sampler(NearestSamplerClamped);
  ASSERT_TRUE(second);
  EXPECT_NE(SlotMap_decode_id(*sampler).index, SlotMap_decode_id(*second).index);
  EXPECT_FALSE(device.context.allocate_sampler(LinearSamplerBorder));
  // the frame recording when it was destroyed is the one that seals it
  device.context.seal_retirements(*ticket);
  ASSERT_TRUE(device.context.wait(*ticket));
  device.context.collect_retired();
  EXPECT_EQ(std::ranges::count(device.capture.destroyed_samplers, handle), 1);
  const auto reused = device.context.allocate_sampler(LinearSamplerBorder);
  ASSERT_TRUE(reused);
  EXPECT_EQ(SlotMap_decode_id(*reused).index, SlotMap_decode_id(*sampler).index);
  EXPECT_NE(*reused, *sampler);
  EXPECT_EQ(device.context.sampler(*sampler).handle, VK_NULL_HANDLE);
}

TEST(RenderContext, RetiredResourcesOutliveACompletedTimelineUntilAFrameSealsThem) {
  auto device = ContextDevice{};
  device.bindless(2);
  const auto sampler = device.context.allocate_sampler(LinearSamplerRepeated);
  ASSERT_TRUE(sampler);
  const auto handle = device.context.sampler(*sampler).handle;
  const auto program = device.context.compile(context_fill_module(0));
  ASSERT_TRUE(program);
  const auto earlier = device.context.execute(*program);
  ASSERT_TRUE(earlier);
  device.context.destroy_sampler(*sampler);
  ASSERT_TRUE(device.context.wait(*earlier));
  device.context.collect_retired();
  EXPECT_TRUE(device.capture.destroyed_samplers.empty());
  const auto frame = device.context.execute(*program);
  ASSERT_TRUE(frame);
  device.context.seal_retirements(*frame);
  ASSERT_TRUE(device.context.wait(*frame));
  device.context.collect_retired();
  EXPECT_EQ(std::ranges::count(device.capture.destroyed_samplers, handle), 1);
}

TEST(RenderContext, ImageViewsPreserveSwizzlesAndWriteBothBindlessArrays) {
  auto device = ContextDevice{};
  device.bindless();
  const auto info = vir::ImageViewInfo{
    .image = context_handle<VkImage>(20),
    .format = vir::Format::BC4UnormBlock,
    .range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 3, 0, 1},
    .components =
      {vir::ComponentSwizzle::R, vir::ComponentSwizzle::R, vir::ComponentSwizzle::R, vir::ComponentSwizzle::One},
    .usage = vir::ImageUsageFlags::Sampled | vir::ImageUsageFlags::Storage,
  };
  const auto view = device.context.allocate_image_view(info);
  ASSERT_TRUE(view);
  EXPECT_EQ(device.capture.view_info.components.g, static_cast<VkComponentSwizzle>(vir::ComponentSwizzle::R));
  EXPECT_EQ(device.capture.view_info.components.a, static_cast<VkComponentSwizzle>(vir::ComponentSwizzle::One));
  EXPECT_EQ(device.capture.descriptor_indices, (std::vector<std::pair<u32, u32>>{{1, 0}, {2, 0}}));
  ASSERT_EQ(device.capture.descriptor_images.size(), 2);
  EXPECT_EQ(device.capture.descriptor_images[0].imageLayout, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL);
  EXPECT_EQ(device.capture.descriptor_images[1].imageLayout, VK_IMAGE_LAYOUT_GENERAL);
  const auto handle = device.context.image_view(*view).handle;
  device.context.destroy_image_view(*view);
  device.context.seal_retirements({});
  device.context.collect_retired();
  EXPECT_EQ(device.capture.destroyed_views, (std::vector<VkImageView>{handle}));
}

TEST(RenderContext, SwapchainProgramsAreRejectedAfterTheGenerationChanges) {
  auto device = ContextDevice{};
  auto& swapchain = device.context.resources->swapchain;
  swapchain.handle = context_handle<VkSwapchainKHR>(50);
  swapchain.attachments.push_back(
    vir::ImageAttachment::from_image(
      vir::Image::imported(context_handle<VkImage>(51), vir::Format::RGBA8Srgb, {64, 32, 1}, vir::Samples::e1),
      VK_IMAGE_LAYOUT_UNDEFINED
    )
  );
  auto module = vir::Module{};
  module.present(device.context.acquire_next_image(module));
  auto program = device.context.compile(std::move(module));
  ASSERT_TRUE(program);
  EXPECT_EQ(program->presentation_generation, device.context.swapchain_info().generation);
  device.context.resources->swapchain_generation++;
  const auto ticket = device.context.execute(*program);
  ASSERT_FALSE(ticket);
  EXPECT_EQ(ticket.error(), VK_ERROR_OUT_OF_DATE_KHR);
  EXPECT_EQ(device.capture.submitted, 0);
  EXPECT_TRUE(device.context.execute_blocking(context_fill_module(0)));
}

TEST(RenderContext, ModulesRecordedBeforeAResizeCannotBeCompiledAgainstTheNewSwapchain) {
  auto device = ContextDevice{};
  auto& swapchain = device.context.resources->swapchain;
  swapchain.handle = context_handle<VkSwapchainKHR>(50);
  swapchain.generation = device.context.resources->swapchain_generation;
  swapchain.attachments.push_back(
    vir::ImageAttachment::from_image(
      vir::Image::imported(context_handle<VkImage>(51), vir::Format::RGBA8Srgb, {64, 32, 1}, vir::Samples::e1),
      VK_IMAGE_LAYOUT_UNDEFINED
    )
  );
  auto module = vir::Module{};
  module.present(device.context.acquire_next_image(module));
  device.context.resources->swapchain_generation++;
  const auto compiled = device.context.compile(std::move(module));
  ASSERT_FALSE(compiled);
  EXPECT_EQ(compiled.error(), VK_ERROR_OUT_OF_DATE_KHR);
}

TEST(RenderContext, ShutdownDestroysLiveSlotsBeyondHolesAndIsIdempotent) {
  auto device = ContextDevice{};
  device.bindless();
  const auto first = device.context.allocate_sampler(LinearSamplerRepeated);
  const auto middle = device.context.allocate_sampler(NearestSamplerClamped);
  const auto last = device.context.allocate_sampler(LinearSamplerBorder);
  ASSERT_TRUE(first);
  ASSERT_TRUE(middle);
  ASSERT_TRUE(last);
  device.context.destroy_sampler(*middle);
  device.context.collect_retired();
  device.context.destroy_context();
  EXPECT_EQ(device.capture.destroyed_samplers.size(), 3);
  EXPECT_EQ(device.capture.pools_destroyed, 1);
  EXPECT_EQ(device.capture.layouts_destroyed, 1);
  device.context.destroy_context();
  EXPECT_EQ(device.capture.destroyed_samplers.size(), 3);
}

TEST(RenderContext, PipelinesAreDeclaredByNameAndUnsupportedFeaturesAreSkipped) {
  auto device = ContextDevice{};
  auto data = ShaderPipelineData{
    .module_name = "compute",
    .entry_points = {{
      .name = "main",
      .shader_stage = ShaderStage::Compute,
      .spirv = {0x07230203, 0x00010500, 0, 100, 0},
      .layout = {.stage = vir::ShaderStage::Compute, .entry_point = "main", .local_size = {8, 4, 1}},
    }}
  };
  ASSERT_TRUE(device.context.create_pipeline(data));
  const auto pipeline = device.context.pipeline("compute");
  EXPECT_NE(pipeline, vir::PipelineID::Invalid);
  EXPECT_EQ(device.context.graph->local_size(pipeline), (std::array<u32, 3>{8, 4, 1}));
  EXPECT_FALSE(device.context.create_pipeline(data));
  EXPECT_EQ(device.context.pipeline("missing"), vir::PipelineID::Invalid);
  data.module_name = "unsupported";
  data.required_features = ShaderFeatureFlag::MeshShaders;
  EXPECT_TRUE(device.context.create_pipeline(data));
  EXPECT_EQ(device.context.pipeline("unsupported"), vir::PipelineID::Invalid);
}

TEST(RenderContext, RawFormatConversionsAndSamplerPresetsMatchTheirVulkanValues) {
  EXPECT_EQ(apply_srgb_preference(vir::Format::BC7UnormBlock, true), vir::Format::BC7SrgbBlock);
  EXPECT_EQ(apply_srgb_preference(vir::Format::BC7SrgbBlock, false), vir::Format::BC7UnormBlock);
  EXPECT_EQ(apply_srgb_preference(vir::Format::BC5UnormBlock, true), vir::Format::BC5UnormBlock);
  EXPECT_EQ(NearestSamplerClamped.min_filter, VK_FILTER_NEAREST);
  EXPECT_EQ(LinearSamplerRepeated.address_mode_u, VK_SAMPLER_ADDRESS_MODE_REPEAT);
  EXPECT_EQ(CmpDepthSampler.compare_op, VK_COMPARE_OP_GREATER_OR_EQUAL);
  const auto constants = PushConstants(u32{2}, u64{3});
  EXPECT_EQ(constants.size(), 12);
}

TEST(RenderContext, Fsr3SharpeningKeepsTheOutputAndItsPostProcessingBarriersInBothBranches) {
  const auto pack = context_engine_pack();
  ASSERT_TRUE(pack.has_value()) << "engine.oxpack was not built next to the test";
  auto device = ContextDevice{};
  device.bindless();
  for (const auto& entry : pack->entries) {
    const auto* shader = std::get_if<ShaderPipelineData>(&entry.data);
    if (shader && shader->module_name == "fsr3_rcas") {
      ASSERT_TRUE(device.context.create_pipeline(*shader));
    }
  }
  ASSERT_NE(device.context.pipeline("fsr3_rcas"), vir::PipelineID::Invalid);

  auto module = vir::Module{};
  const auto output = module.transient_image({
    .extent = {64, 32, 1},
    .format = vir::Format::RGBA16Sfloat,
    .name = "fsr3 output",
  });
  const auto constants = module.declare_buffer_var("constants", vir::Access::ComputeUniformRead);
  const auto history = module.declare_image_var("history", vir::Format::RGBA16Sfloat, vir::Access::ComputeSampled);
  const auto exposure = module.declare_buffer_var("exposure", vir::Access::ComputeUniformRead);
  const auto enabled = module.declare_bool_var("sharpening", false);
  const auto sharpness = module.declare_bytes_var("sharpness", sizeof(f32));
  const auto accumulate = module.declare_callback_var("accumulate");
  const auto
    accumulated = module.begin_compute({{output, vir::Access::ComputeRW}}).record_from(accumulate).end_compute()[0];
  const auto result = RendererInstance::sharpen_fsr3(
    module,
    device.context,
    enabled,
    sharpness,
    constants,
    history,
    exposure,
    accumulated
  );
  EXPECT_EQ(module.resource_root(result[3]), output);
  const auto compiled = module.compile(
    device.context.graph->pipeline_bindings(),
    module.export_to(result[3], vir::Access::ComputeSampled)
  );
  ASSERT_TRUE(compiled.has_value());
  EXPECT_TRUE(compiled->has_variable(enabled));

  auto found_output = false;
  auto found_sampled_transition = false;
  for (const auto& instruction : compiled->instructions) {
    if (const auto* image = std::get_if<vir::ir::ConstructImage>(&instruction.ir); image && instruction.id == output) {
      found_output = true;
      EXPECT_EQ(
        image->usage & (vir::ImageUsageFlags::Storage | vir::ImageUsageFlags::Sampled),
        vir::ImageUsageFlags::Storage | vir::ImageUsageFlags::Sampled
      );
    } else if (const auto* phi = std::get_if<vir::ir::Phi>(&instruction.ir)) {
      if (module.is_image(instruction.id) && module.resource_root(instruction.id) != history) {
        for (const auto& [incoming, label] : phi->incoming) {
          EXPECT_EQ(module.resource_root(incoming), output) << compiled->dump();
        }
      }
    } else if (const auto* barrier = std::get_if<vir::ir::ImageBarrier>(&instruction.ir)) {
      if (
        module.resource_root(barrier->value) == output && barrier->old_layout == VK_IMAGE_LAYOUT_GENERAL &&
        barrier->new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
      ) {
        found_sampled_transition = true;
      }
    }
  }
  EXPECT_TRUE(found_output);
  EXPECT_TRUE(found_sampled_transition) << compiled->dump();
}

TEST(RenderContext, LightGridRecordingDeclaresLiveInputsAndSkipsDisabledInputs) {
  const auto pack = context_engine_pack();
  ASSERT_TRUE(pack);
  auto device = ContextDevice{};
  for (const auto& entry : pack->entries) {
    const auto* shader = std::get_if<ShaderPipelineData>(&entry.data);
    if (shader && shader->module_name == "light_grid") {
      ASSERT_TRUE(device.context.create_pipeline(*shader));
    }
  }

  using LightPush = PushConstants<u32, glm::vec3>;
  const auto is_benchmarking = std::getenv("OX_FRONTEND_BENCH") != nullptr;
  const auto repetitions = is_benchmarking ? 128_u32 : 1_u32;
  for (const auto is_enabled : {false, true}) {
    auto cold_time = std::chrono::nanoseconds{};
    auto bind_time = std::chrono::nanoseconds{};
    for (auto iteration = 0_u32; iteration < repetitions; iteration++) {
      const auto start = std::chrono::steady_clock::now();
      auto module = vir::Module{};
      auto prepared = PreparedFrame{};
      auto shape = FrameShape{.has_pointspot_shadows = is_enabled};
      auto recording = FrameRecording{.shape = shape, .prepared = prepared};
      auto& values = recording.values;
      values.lights = module.declare_buffer_var("lights", vir::Access::ComputeRead);
      auto context = LightGridContext{};
      RendererInstance::build_light_grid(module, device.context, recording, context);
      module.export_to(context.light_grid_buffer, vir::Access::ComputeRead);
      auto program = device.context.compile(std::move(module));
      ASSERT_TRUE(program);
      cold_time += std::chrono::steady_clock::now() - start;
      EXPECT_EQ(program->has_variable(prepared.light_grid.push), is_enabled);
      const auto instruction_count = program->instructions.size();
      if (is_enabled) {
        const auto& variable = program->variables.at(program->slots.at(prepared.light_grid.push));
        EXPECT_EQ(variable.kind.tag, vir::ir::VariableKindTag::Bytes);
        EXPECT_EQ(variable.kind.size, sizeof(LightPush));
        program->set_bytes(prepared.light_grid.push, LightPush(1_u32, glm::vec3(0.0f)));
        const auto* storage = std::get<vir::Bytes>(variable.value).data.data();
        const auto bind_start = std::chrono::steady_clock::now();
        const auto frames = is_benchmarking ? 10000_u32 : 2_u32;
        for (auto frame = 0_u32; frame < frames; frame++) {
          program->set_bytes(prepared.light_grid.push, LightPush(frame, glm::vec3(static_cast<f32>(frame))));
        }
        bind_time += std::chrono::steady_clock::now() - bind_start;
        EXPECT_EQ(std::get<vir::Bytes>(variable.value).data.data(), storage);
        const auto expected = LightPush(frames - 1, glm::vec3(static_cast<f32>(frames - 1)));
        EXPECT_EQ(std::memcmp(std::get<vir::Bytes>(variable.value).data.data(), &expected, sizeof(expected)), 0);
      }
      EXPECT_EQ(program->instructions.size(), instruction_count);
      if (const auto* directory = std::getenv("OX_FRONTEND_DUMP"); directory && iteration == 0) {
        std::ofstream(std::filesystem::path(directory) / (is_enabled ? "light-grid-on.ir" : "light-grid-off.ir"))
          << program->dump();
      }
    }
    if (is_benchmarking) {
      std::fprintf(
        stderr,
        "light grid %s: cold %.1f us, bind %.1f ns\n",
        is_enabled ? "on" : "off",
        static_cast<f64>(cold_time.count()) / repetitions / 1000.0,
        static_cast<f64>(bind_time.count()) / repetitions / 10000.0
      );
    }
  }
}

TEST(RenderContext, PostProcessingRecordingDeclaresOnlyConsumedVariables) {
  const auto pack = context_engine_pack();
  ASSERT_TRUE(pack);
  auto device = ContextDevice{};
  device.bindless();
  for (const auto& entry : pack->entries) {
    const auto* shader = std::get_if<ShaderPipelineData>(&entry.data);
    if (
      shader && (shader->module_name.starts_with("histogram_") || shader->module_name.starts_with("bloom_") ||
                 shader->module_name == "tonemap")
    ) {
      ASSERT_TRUE(device.context.create_pipeline(*shader));
    }
  }
  for (const auto is_enabled : {false, true}) {
    auto module = vir::Module{};
    auto prepared = PreparedFrame{};
    auto shape = FrameShape{};
    shape.display_size = {64, 32};
    shape.scene_flags = is_enabled ? GPU::SceneFlags::HasBloom : GPU::SceneFlags::None;
    shape.has_bloom = is_enabled;
    shape.has_eye_adaptation = is_enabled;
    auto recording = FrameRecording{.shape = shape, .prepared = prepared};
    auto& values = recording.values;
    values.exposure = module.declare_buffer_var("exposure", vir::Access::ComputeRW);
    auto context = PostProcessContext{};
    context.final_attachment = module.transient_image({.extent = {64, 32, 1}, .format = vir::Format::RGBA16Sfloat});
    context.final_attachment = module.clear(
      context.final_attachment,
      vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 0.0f)
    );
    context.bloom_upsampled_attachment = module.transient_image(
      {.extent = {32, 16, 1}, .format = vir::Format::RGBA16Sfloat, .mip_levels = 6}
    );
    context.bloom_upsampled_attachment = module.clear(
      context.bloom_upsampled_attachment,
      vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 0.0f)
    );
    context.dst_attachment = module.transient_image({.extent = {64, 32, 1}, .format = vir::Format::RGBA8Unorm});
    if (is_enabled) {
      RendererInstance::apply_eye_adaptation(module, device.context, recording, context);
      RendererInstance::apply_bloom(module, device.context, recording, context);
    }
    const auto result = RendererInstance::apply_tonemap(module, device.context, recording, context);
    module.export_to(result, vir::Access::FragmentSampled);
    auto program = device.context.compile(std::move(module));
    ASSERT_TRUE(program);
    EXPECT_EQ(program->has_variable(prepared.post_process.histogram_generate_push), is_enabled);
    EXPECT_EQ(program->has_variable(prepared.post_process.histogram_average_push), is_enabled);
    EXPECT_EQ(program->has_variable(prepared.post_process.bloom_prefilter_push), is_enabled);
    EXPECT_EQ(program->has_variable(prepared.post_process.bloom_radius), is_enabled);
    ASSERT_TRUE(program->has_variable(prepared.post_process.tonemap_push));
    const auto& tonemap = program->variables.at(program->slots.at(prepared.post_process.tonemap_push));
    EXPECT_EQ(tonemap.kind.tag, vir::ir::VariableKindTag::Bytes);
    EXPECT_EQ(tonemap.kind.size, (sizeof(PushConstants<GPU::PostProcessSettings, glm::ivec2, f32>)));
    if (const auto* directory = std::getenv("OX_FRONTEND_DUMP")) {
      std::ofstream(std::filesystem::path(directory) / (is_enabled ? "post-on.ir" : "post-off.ir")) << program->dump();
    }
  }
}
TEST(RenderContext, EarlyAndLateGeometryShareLiveCullInputs) {
  const auto pack = context_engine_pack();
  ASSERT_TRUE(pack);
  auto device = ContextDevice{};
  device.bindless();
  for (const auto& entry : pack->entries) {
    const auto* shader = std::get_if<ShaderPipelineData>(&entry.data);
    if (shader && shader->module_name.starts_with("cull_")) {
      ASSERT_TRUE(device.context.create_pipeline(*shader));
    }
  }
  for (const auto use_mesh_shaders : {false, true}) {
    auto module = vir::Module{};
    auto prepared = PreparedFrame{};
    auto shape = FrameShape{.use_mesh_shaders = use_mesh_shaders};
    auto recording = FrameRecording{.shape = shape, .prepared = prepared};
    auto& values = recording.values;
    values.meshes = module.declare_buffer_var("meshes", vir::Access::ComputeRead);
    values.transforms_world = module.declare_buffer_var("transforms", vir::Access::ComputeRead);
    values.mesh_instances = module.declare_buffer_var("instances", vir::Access::ComputeRW);
    values.meshlet_instances = module.declare_buffer_var("meshlets", vir::Access::ComputeRW);
    values.visible_meshlet_instances_indices = module.declare_buffer_var("visible indices", vir::Access::ComputeRW);
    values.meshlet_instance_visibility_mask = module.declare_buffer_var("visibility mask", vir::Access::ComputeRW);
    values.reordered_indices = module.declare_buffer_var("indices", vir::Access::ComputeRW);
    auto context = CullGeometryContext{.use_hiz = true, .init_cull_meshes = true};
    context.hiz_attachment = module.declare_image_var("hiz", vir::Format::R32Sfloat, vir::Access::ComputeSampled);
    RendererInstance::cull_geometry(module, device.context, recording, context);
    const auto camera = prepared.cull.camera;
    const auto mesh_count = prepared.cull.mesh_instance_count;
    module.export_to(context.draw_geometry_cmd_buffer, vir::Access::IndirectRead);
    context.init_cull_meshes = false;
    context.cull_flags = GPU::CullFlag::LatePass;
    RendererInstance::cull_geometry(module, device.context, recording, context);
    module.export_to(context.draw_geometry_cmd_buffer, vir::Access::IndirectRead);
    auto program = device.context.compile(std::move(module));
    ASSERT_TRUE(program);
    EXPECT_EQ(prepared.cull.camera, camera);
    EXPECT_EQ(prepared.cull.mesh_instance_count, mesh_count);
    EXPECT_TRUE(program->has_variable(camera));
    EXPECT_TRUE(program->has_variable(mesh_count));
    EXPECT_FALSE(program->has_variable(prepared.cull.shadow_camera));
    for (const auto id : prepared.cull.pointspot_contexts) {
      EXPECT_EQ(id, vir::ValueID::Invalid);
      EXPECT_FALSE(program->has_variable(id));
    }
    const auto& camera_variable = program->variables.at(program->slots.at(camera));
    EXPECT_EQ(camera_variable.kind.size, sizeof(GPU::CullCamera));
    program->set_bytes(camera, GPU::CullCamera{.mesh_instance_count = 3});
    program->set_bytes(camera, GPU::CullCamera{.mesh_instance_count = 7});
    auto bound_camera = GPU::CullCamera{};
    std::memcpy(&bound_camera, std::get<vir::Bytes>(camera_variable.value).data.data(), sizeof(bound_camera));
    EXPECT_EQ(bound_camera.mesh_instance_count, 7);
  }
}

TEST(RenderContext, PointSpotCullingKeepsDistinctMipInputsAndReusesRepeatedMip) {
  const auto pack = context_engine_pack();
  ASSERT_TRUE(pack);
  auto device = ContextDevice{};
  device.bindless();
  for (const auto& entry : pack->entries) {
    const auto* shader = std::get_if<ShaderPipelineData>(&entry.data);
    if (
      shader && (shader->module_name.starts_with("rmvsm_pointspot_cull_") ||
                 shader->module_name == "rmvsm_pointspot_downsample_hpb" ||
                 shader->module_name == "rmvsm_pointspot_build_active_layers")
    ) {
      ASSERT_TRUE(device.context.create_pipeline(*shader));
    }
  }
  for (const auto use_mesh_shaders : {false, true}) {
    auto module = vir::Module{};
    auto prepared = PreparedFrame{};
    auto shape = FrameShape{.use_mesh_shaders = use_mesh_shaders};
    auto recording = FrameRecording{.shape = shape, .prepared = prepared};
    auto& values = recording.values;
    values.meshes = module.declare_buffer_var("meshes", vir::Access::ComputeRead);
    values.transforms_world = module.declare_buffer_var("transforms", vir::Access::ComputeRead);
    values.mesh_instances = module.declare_buffer_var("instances", vir::Access::ComputeRead);
    auto context = CullGeometryPointSpotContext{.init = true};
    context.views_buffer = module.declare_buffer_var("views", vir::Access::ComputeRead);
    context.layer_dirty_mask_buffer = module.declare_buffer_var("dirty layers", vir::Access::ComputeRead);
    context.page_table_attachment = module.transient_image({
      .extent = {RMVSMContext::POINT_SPOT_PAGE_TABLE_SIZE, RMVSMContext::POINT_SPOT_PAGE_TABLE_SIZE, 1},
      .format = vir::Format::R32Uint,
      .view_type = vir::ImageViewType::e2DArray,
      .mip_levels = RMVSMContext::POINT_SPOT_MIP_COUNT,
      .array_layers = RMVSMContext::POINT_SPOT_LAYER_COUNT,
    });
    context.page_table_attachment = module.clear(context.page_table_attachment, vir::ClearValue::rgba_u32(0, 0, 0, 0));
    auto first_context = vir::ValueID::Invalid;
    auto first_command = vir::ValueID::Invalid;
    for (const auto mip : {0_u32, 1_u32, 0_u32}) {
      context.curr_mip = mip;
      context.hpb_attachment = module.transient_image({
        .extent = {RMVSMContext::POINT_SPOT_PAGE_TABLE_SIZE >> mip, RMVSMContext::POINT_SPOT_PAGE_TABLE_SIZE >> mip, 1},
        .format = vir::Format::R8Uint,
        .view_type = vir::ImageViewType::e2DArray,
        .mip_levels = RMVSMContext::POINT_SPOT_HPB_LEVEL_COUNT - mip,
        .array_layers = RMVSMContext::POINT_SPOT_LAYER_COUNT,
      });
      RendererInstance::cull_geometry_pointspot(module, device.context, recording, context);
      module.export_to(context.draw_cmd_buffer, vir::Access::IndirectRead);
      module.export_to(context.hpb_attachment, vir::Access::ComputeSampled);
      if (context.init) {
        first_context = prepared.cull.pointspot_contexts[0];
        first_command = prepared.cull.pointspot_cull_meshes_commands[0];
        context.init = false;
      }
    }
    auto program = device.context.compile(std::move(module));
    ASSERT_TRUE(program);
    EXPECT_EQ(prepared.cull.pointspot_contexts[0], first_context);
    EXPECT_EQ(prepared.cull.pointspot_cull_meshes_commands[0], first_command);
    EXPECT_NE(prepared.cull.pointspot_contexts[0], prepared.cull.pointspot_contexts[1]);
    EXPECT_NE(prepared.cull.pointspot_cull_meshes_commands[0], prepared.cull.pointspot_cull_meshes_commands[1]);
    for (auto mip = 0_u32; mip < RMVSMContext::POINT_SPOT_MIP_COUNT; mip++) {
      EXPECT_EQ(program->has_variable(prepared.cull.pointspot_contexts[mip]), mip < 2);
      EXPECT_EQ(program->has_variable(prepared.cull.pointspot_cull_meshes_commands[mip]), mip < 2);
    }
    program->set_bytes(first_context, GPU::VSMPointSpotContext{.curr_mip = 0});
    program->set_bytes(prepared.cull.pointspot_contexts[1], GPU::VSMPointSpotContext{.curr_mip = 1});
    const auto& second = program->variables.at(program->slots.at(prepared.cull.pointspot_contexts[1]));
    auto bound_context = GPU::VSMPointSpotContext{};
    std::memcpy(&bound_context, std::get<vir::Bytes>(second.value).data.data(), sizeof(bound_context));
    EXPECT_EQ(bound_context.curr_mip, 1);
  }
}

TEST(RenderContext, ProbeDebugRecordingDeclaresDistinctVolumeInputsWithoutTraceInputs) {
  const auto pack = context_engine_pack();
  ASSERT_TRUE(pack);
  auto device = ContextDevice{};
  device.bindless();
  for (const auto& entry : pack->entries) {
    const auto* shader = std::get_if<ShaderPipelineData>(&entry.data);
    if (shader && shader->module_name == "ddgi_debug_probes") {
      ASSERT_TRUE(device.context.create_pipeline(*shader));
    }
  }
  auto module = vir::Module{};
  auto prepared = PreparedFrame{};
  auto shape = FrameShape{.probe_volume_count = 2};
  auto recording = FrameRecording{.shape = shape, .prepared = prepared};
  recording.values.camera = module.declare_buffer_var("camera", vir::Access::VertexUniformRead);
  auto context = DDGIDebugContext{};
  context.probe_volumes_buffer = module.declare_buffer_var("volumes", vir::Access::VertexRead);
  context.probe_states_buffer = module.declare_buffer_var("states", vir::Access::VertexRead);
  context.irradiance_attachment = module.declare_image_var(
    "irradiance",
    vir::Format::RGBA16Sfloat,
    vir::Access::FragmentSampled
  );
  context.depth_attachment = module.transient_image({.extent = {64, 32, 1}, .format = vir::Format::D32Sfloat});
  context.depth_attachment = module.clear(context.depth_attachment, vir::ClearValue::depth(0.0f));
  auto dst = module.transient_image({.extent = {64, 32, 1}, .format = vir::Format::RGBA16Sfloat});
  dst = module.clear(dst, vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 0.0f));
  dst = RendererInstance::draw_ddgi_probes(module, device.context, recording, context, dst);
  module.export_to(dst, vir::Access::FragmentSampled);
  auto program = device.context.compile(std::move(module));
  ASSERT_TRUE(program);
  ASSERT_EQ(prepared.ddgi.volumes.size(), 2);
  EXPECT_NE(prepared.ddgi.volumes[0].debug_push, prepared.ddgi.volumes[1].debug_push);
  EXPECT_NE(prepared.ddgi.volumes[0].debug_probe_count, prepared.ddgi.volumes[1].debug_probe_count);
  for (const auto& volume : prepared.ddgi.volumes) {
    EXPECT_TRUE(program->has_variable(volume.debug_push));
    EXPECT_TRUE(program->has_variable(volume.debug_probe_count));
    EXPECT_EQ(volume.select_push, vir::ValueID::Invalid);
    EXPECT_EQ(volume.trace_push, vir::ValueID::Invalid);
    EXPECT_EQ(volume.trace_tail_push, vir::ValueID::Invalid);
  }
  program->set(prepared.ddgi.volumes[0].debug_probe_count, 3_u32);
  program->set(prepared.ddgi.volumes[1].debug_probe_count, 7_u32);
  const auto& first = program->variables.at(program->slots.at(prepared.ddgi.volumes[0].debug_probe_count));
  const auto& second = program->variables.at(program->slots.at(prepared.ddgi.volumes[1].debug_probe_count));
  EXPECT_EQ(std::get<u32>(first.value), 3);
  EXPECT_EQ(std::get<u32>(second.value), 7);
}

TEST(RenderContext, EarlyAndLateTerrainShareLiveCullInputs) {
  const auto pack = context_engine_pack();
  ASSERT_TRUE(pack);
  auto device = ContextDevice{};
  for (const auto& entry : pack->entries) {
    const auto* shader = std::get_if<ShaderPipelineData>(&entry.data);
    if (shader && shader->module_name == "terrain_cull") {
      ASSERT_TRUE(device.context.create_pipeline(*shader));
    }
  }
  auto module = vir::Module{};
  auto prepared = PreparedFrame{};
  auto shape = FrameShape{.has_terrain = true};
  auto recording = FrameRecording{.shape = shape, .prepared = prepared};
  auto context = TerrainContext{};
  context.terrain_buffer = module.declare_buffer_var("terrain", vir::Access::ComputeUniformRead);
  context.patch_minmax_attachment = module.declare_image_var(
    "patch minmax",
    vir::Format::RG16Unorm,
    vir::Access::ComputeSampled
  );
  context.hiz_attachment = module.declare_image_var("hiz", vir::Format::R32Sfloat, vir::Access::ComputeSampled);
  context.visible_patches_buffer = module.declare_buffer_var("visible patches", vir::Access::ComputeRW);
  context.patch_visibility_mask_buffer = module.declare_buffer_var("visibility mask", vir::Access::ComputeRW);
  RendererInstance::cull_terrain(module, device.context, recording, context);
  const auto camera = prepared.terrain.cull_camera;
  const auto patch_count = prepared.terrain.patch_total;
  module.export_to(context.draw_cmd_buffer, vir::Access::IndirectRead);
  context.cull_flags = GPU::CullFlag::LatePass;
  RendererInstance::cull_terrain(module, device.context, recording, context);
  module.export_to(context.draw_cmd_buffer, vir::Access::IndirectRead);
  auto program = device.context.compile(std::move(module));
  ASSERT_TRUE(program);
  EXPECT_EQ(prepared.terrain.cull_camera, camera);
  EXPECT_EQ(prepared.terrain.patch_total, patch_count);
  EXPECT_TRUE(program->has_variable(camera));
  EXPECT_TRUE(program->has_variable(patch_count));
  EXPECT_EQ(prepared.terrain.brush_params, vir::ValueID::Invalid);
  program->set(patch_count, 13_u32);
  program->set_bytes(camera, GPU::CullCamera{.mesh_instance_count = 13});
  const auto& count_variable = program->variables.at(program->slots.at(patch_count));
  EXPECT_EQ(std::get<u32>(count_variable.value), 13);
}

} // namespace ox
