#include <VkBootstrap.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <vector>

#include "Render/Graph/Types.hpp"

#ifdef OX_PLATFORM_LINUX
  #include <dlfcn.h>
  #include <link.h>
#endif

#include "Asset/AssetFile.hpp"
#include "Asset/Texture.hpp"
#include "Core/Base.hpp"
#include "Render/Graph/RenderGraph.hpp"
#include "Render/RenderContext.hpp"
#include "Render/RenderResources.hpp"
#include "Render/RendererInstance.hpp"
#include "Render/Utils/Samplers.hpp"

namespace ox {
static VKAPI_ATTR auto VKAPI_CALL hiz_validation_message(
  VkDebugUtilsMessageSeverityFlagBitsEXT severity,
  VkDebugUtilsMessageTypeFlagsEXT,
  const VkDebugUtilsMessengerCallbackDataEXT* message,
  void* user
) -> VkBool32 {
  if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
    static_cast<std::atomic<u32>*>(user)->fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr, "%s\n", message->pMessage);
  }
  return VK_FALSE;
}

static auto retain_hiz_driver_modules() -> void {
#ifdef OX_PLATFORM_LINUX
  // keep driver process globals visible to leak checking after the Vulkan loader closes them
  dl_iterate_phdr(
    [](dl_phdr_info* info, usize, void*) -> i32 {
      const auto path = std::string_view(info->dlpi_name);
      if (path.find("libvulkan_") != std::string_view::npos || path.find("libVkLayer_") != std::string_view::npos) {
        if (auto* module = dlopen(info->dlpi_name, RTLD_LAZY | RTLD_NOLOAD | RTLD_NODELETE)) {
          dlclose(module);
        }
      }
      return 0;
    },
    nullptr
  );
#endif
}

static auto hiz_gpu_pack() -> option<AssetFile> {
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

static auto reference_hiz(std::span<const f32> source, glm::uvec2 source_size, glm::uvec2 destination_size)
  -> std::vector<f32> {
  auto result = std::vector<f32>(usize(destination_size.x) * destination_size.y, 1.0f);
  for (u32 y = 0; y < destination_size.y; y++) {
    for (u32 x = 0; x < destination_size.x; x++) {
      const auto begin_x = x * source_size.x / destination_size.x;
      const auto begin_y = y * source_size.y / destination_size.y;
      const auto end_x = begin_x + 2;
      const auto end_y = begin_y + 2;
      auto value = 1.0f;
      for (auto row = begin_y; row < end_y; row++) {
        for (auto column = begin_x; column < end_x; column++) {
          const auto sample = column < source_size.x && row < source_size.y
                                ? source[usize(row) * source_size.x + column]
                                : 0.0f;
          value = std::min(value, sample);
        }
      }
      result[usize(y) * destination_size.x + x] = value;
    }
  }
  return result;
}

static auto reference_hiz_base(std::span<const f32> source, glm::uvec2 source_size, glm::uvec2 destination_size)
  -> std::vector<f32> {
  auto result = std::vector<f32>(usize(destination_size.x) * destination_size.y);
  for (u32 y = 0; y < destination_size.y; y++) {
    const auto source_y = std::min((y + 1) * source_size.y / destination_size.y, source_size.y - 1);
    for (u32 x = 0; x < destination_size.x; x++) {
      const auto source_x = std::min((x + 1) * source_size.x / destination_size.x, source_size.x - 1);
      result[usize(y) * destination_size.x + x] = source[usize(source_y) * source_size.x + source_x];
    }
  }
  return result;
}

struct HiZTestDirectory {
  std::filesystem::path previous = std::filesystem::current_path();
  std::filesystem::path temporary = std::filesystem::temp_directory_path() /
                                    ("oxylus-hiz-gpu-" +
                                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

  HiZTestDirectory() {
    std::filesystem::create_directories(temporary);
    std::filesystem::current_path(temporary);
  }
  ~HiZTestDirectory() {
    std::filesystem::current_path(previous);
    std::filesystem::remove_all(temporary);
  }
};

TEST(HiZGPU, EveryMipMatchesOriginalSPDDepthReductionAcrossRepeatedExecutions) {
  auto validation_errors = std::atomic<u32>{0};
  auto builder = vkb::InstanceBuilder{};
  builder.set_headless().require_api_version(1, 3, 0).enable_extension(VK_KHR_SURFACE_EXTENSION_NAME);
  if (std::getenv("OX_HIZ_GPU_VALIDATION")) {
    builder.request_validation_layers()
      .set_debug_callback(hiz_validation_message)
      .set_debug_callback_user_data_pointer(&validation_errors)
      .add_validation_feature_enable(VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT);
  }
  const auto instance = builder.build();
  if (!instance) {
    GTEST_SKIP() << "headless Vulkan instance unavailable: " << instance.error().message();
  }
  OX_DEFER(&) { vkb::destroy_instance(*instance); };
  auto features = VkPhysicalDeviceFeatures{};
  features.shaderInt64 = VK_TRUE;
  features.shaderStorageImageReadWithoutFormat = VK_TRUE;
  features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
  auto features12 = VkPhysicalDeviceVulkan12Features{};
  features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  features12.timelineSemaphore = VK_TRUE;
  features12.bufferDeviceAddress = VK_TRUE;
  features12.scalarBlockLayout = VK_TRUE;
  features12.vulkanMemoryModel = VK_TRUE;
  features12.vulkanMemoryModelDeviceScope = VK_TRUE;
  auto features13 = VkPhysicalDeviceVulkan13Features{};
  features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  features13.synchronization2 = VK_TRUE;
  features13.dynamicRendering = VK_TRUE;
  features13.maintenance4 = VK_TRUE;
  features13.robustImageAccess = VK_TRUE;
  const auto physical = vkb::PhysicalDeviceSelector(*instance)
                          .require_present(false)
                          .add_required_extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME)
                          .set_minimum_version(1, 3)
                          .set_required_features(features)
                          .set_required_features_12(features12)
                          .set_required_features_13(features13)
                          .select();
  if (!physical) {
    GTEST_SKIP() << "no Vulkan device supports the HiZ shader: " << physical.error().message();
  }
  const auto device = vkb::DeviceBuilder(*physical).build();
  ASSERT_TRUE(device) << device.error().message();
  OX_DEFER(&) {
    retain_hiz_driver_modules();
    vkb::destroy_device(*device);
  };
  auto context = vir::Context::create(
    {.instance = instance->instance,
     .physical_device = physical->physical_device,
     .device = device->device,
     .get_instance_proc_addr = instance->fp_vkGetInstanceProcAddr}
  );
  ASSERT_TRUE(context);
  const auto queue = device->get_queue_index(vkb::QueueType::graphics);
  ASSERT_TRUE(queue);
  ASSERT_TRUE((*context)->create_command_queue(*queue, vir::DomainFlag::Graphics));
  auto directory = HiZTestDirectory{};
  auto render = RenderContext{};
  ASSERT_TRUE(render.initialize_graph(std::move(*context)));
  const auto mutable_image = render.allocate_image({
    .extent = {17, 11, 1},
    .format = vir::Format::RGBA8Unorm,
    .usage = vir::ImageUsageFlags::Sampled | vir::ImageUsageFlags::ColorAttachment,
    .mip_levels = 2,
    .view_formats = {vir::Format::RGBA8Unorm, vir::Format::RGBA8Srgb},
  });
  ASSERT_TRUE(mutable_image);
  const auto image = render.image(*mutable_image);
  EXPECT_EQ(image.extent, (vir::Extent3D{17, 11, 1}));
  const auto mutable_view = render.allocate_image_view(
    {
      .image = image.handle,
      .format = vir::Format::RGBA8Srgb,
      .view_type = vir::ImageViewType::e2D,
      .range = image.subresource_range(),
      .components =
        {vir::ComponentSwizzle::R, vir::ComponentSwizzle::G, vir::ComponentSwizzle::B, vir::ComponentSwizzle::One},
    },
    false
  );
  ASSERT_TRUE(mutable_view);
  render.destroy_image_view(*mutable_view);
  render.destroy_image(*mutable_image);
  render.seal_retirements({});
  render.collect_retired();
  EXPECT_EQ(validation_errors.load(std::memory_order_relaxed), 0u);
  const auto sampler_binding = std::array{VkDescriptorSetLayoutBinding{
    .binding = 0,
    .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
    .descriptorCount = 1,
    .stageFlags = VK_SHADER_STAGE_ALL,
    .pImmutableSamplers = nullptr
  }};
  auto sampler_set = render.create_persistent_descriptor_set(
    sampler_binding,
    std::array<VkDescriptorBindingFlags, 1>{0}
  );
  ASSERT_TRUE(sampler_set);
  render.resources->descriptor_set = std::move(*sampler_set);
  const auto sampler = render.allocate_sampler(vir::NearestSamplerClamped, false);
  ASSERT_TRUE(sampler);
  render.resources->cached_samplers.emplace_back(vir::NearestSamplerClamped, *sampler);
  const auto pack = hiz_gpu_pack();
  ASSERT_TRUE(pack) << "engine.oxpack must be built before this test";
  for (const auto& entry : pack->entries) {
    const auto* shader = std::get_if<ShaderPipelineData>(&entry.data);
    if (shader && shader->module_name == "hiz") {
      ASSERT_TRUE(render.create_pipeline(*shader));
    }
  }
  ASSERT_NE(render.pipeline("hiz"), vir::PipelineID::Invalid);

  for (const auto size :
       {glm::uvec2{257, 257},
        glm::uvec2{301, 301},
        glm::uvec2{1622, 717},
        glm::uvec2{2048, 2048},
        glm::uvec2{4096, 1024}}) {
    SCOPED_TRACE(size.x);
    SCOPED_TRACE(size.y);
    const auto extent = vir::Extent3D{std::bit_ceil((size.x + 1) / 2), std::bit_ceil((size.y + 1) / 2), 1};
    const auto levels = std::min(Texture::calculate_mip_count(extent), 13_u32);
    auto readback_size = 0_u64;
    for (u32 mip = 0; mip < levels; mip++) {
      readback_size += u64(std::max(extent.width >> mip, 1_u32)) * std::max(extent.height >> mip, 1_u32) * sizeof(f32);
    }
    const auto depth = render.persistent->allocate_image(
      {.extent = {size.x, size.y, 1},
       .format = vir::Format::D32Sfloat,
       .usage = vir::ImageUsageFlags::Sampled | vir::ImageUsageFlags::TransferDst}
    );
    ASSERT_TRUE(depth);
    OX_DEFER(&) { render.persistent->deallocate_image(*depth); };
    const auto upload = render.persistent->allocate_buffer(
      {.size = u64(size.x) * size.y * sizeof(f32),
       .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
       .location = vir::MemoryLocation::CpuToGpu}
    );
    ASSERT_TRUE(upload);
    OX_DEFER(&) { render.persistent->deallocate_buffer(*upload); };
    const auto readback = render.persistent->allocate_buffer(
      {.size = readback_size, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .location = vir::MemoryLocation::GpuToCpu}
    );
    ASSERT_TRUE(readback);
    OX_DEFER(&) { render.persistent->deallocate_buffer(*readback); };

    auto module = vir::Module{};
    const auto uploaded = module.copy_buffer_to_image(
      module.import_buffer(*upload, vir::Access::HostWrite),
      module.import_image(*depth, VK_IMAGE_LAYOUT_UNDEFINED, vir::Access::None)
    );
    auto geometry = MainGeometryContext{
      .depth_attachment = uploaded,
      .hiz_attachment = module.transient_image(
        {.extent = extent, .format = vir::Format::R32Sfloat, .mip_levels = levels}
      ),
    };
    RendererInstance::generate_hiz(module, render, geometry);
    auto downloaded = module.import_buffer(*readback, vir::Access::None);
    auto offset = 0_u64;
    for (u32 mip = 0; mip < levels; mip++) {
      const auto level_extent = vir::Extent3D{
        std::max(extent.width >> mip, 1_u32),
        std::max(extent.height >> mip, 1_u32),
        1
      };
      downloaded = module.copy_image_to_buffer_region(
        geometry.hiz_attachment,
        downloaded,
        {.buffer_offset = offset, .image_extent = level_extent, .mip_level = mip}
      );
      offset += u64(level_extent.width) * level_extent.height * sizeof(f32);
    }
    module.export_to(downloaded, vir::Access::HostRead);
    const auto program = render.compile(std::move(module));
    ASSERT_TRUE(program);
    for (const auto iteration : {0_u32, 1_u32}) {
      SCOPED_TRACE(iteration);
      auto source = std::vector<f32>(usize(size.x) * size.y);
      for (usize index = 0; index < source.size(); index++) {
        source[index] = 0.125f + static_cast<f32>((index * 73 + iteration * 131) % 997) / 1330.0f;
      }
      source.back() = iteration == 0 ? 0.0f : 0.0625f;
      ASSERT_TRUE(upload->write<f32>(0, source));
      ASSERT_TRUE(render.persistent->flush_buffer(*upload));
      ASSERT_TRUE(render.execute_blocking(*program));
      ASSERT_TRUE(render.persistent->invalidate_buffer(*readback));
      auto expected = std::move(source);
      auto previous_size = size;
      auto read_offset = 0_u64;
      for (u32 mip = 0; mip < levels; mip++) {
        SCOPED_TRACE(mip);
        const auto level_size = glm::uvec2{std::max(extent.width >> mip, 1_u32), std::max(extent.height >> mip, 1_u32)};
        expected = mip == 0 ? reference_hiz_base(expected, previous_size, level_size)
                            : reference_hiz(expected, previous_size, level_size);
        const auto* actual = reinterpret_cast<const f32*>(readback->mapped + read_offset);
        for (usize index = 0; index < expected.size(); index++) {
          ASSERT_FLOAT_EQ(actual[index], expected[index]) << "texel " << index;
        }
        read_offset += expected.size() * sizeof(f32);
        previous_size = level_size;
      }
      EXPECT_EQ(validation_errors.load(std::memory_order_relaxed), 0u);
    }
  }
}
} // namespace ox
