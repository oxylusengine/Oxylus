#include <SDL3/SDL_filesystem.h>
#include <VkBootstrap.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <vir/Types.hpp>

#ifdef OX_PLATFORM_LINUX
  #include <dlfcn.h>
  #include <link.h>
#endif

#include <vir/RenderGraph.hpp>

#include "Asset/AssetFile.hpp"
#include "Core/Base.hpp"
#include "Memory/SlotMap.hpp"
#include "Render/RenderContext.hpp"
#include "Render/RenderResources.hpp"
#include "Render/RendererInstance.hpp"
#include "Render/Utils/Samplers.hpp"

namespace ox {
constexpr static auto LUT_SIZE = GPU::COLOR_GRADING_LUT_SIZE;
constexpr static auto STRIP_SIZE = 16_u32;

// node picks below are written on a 32 grid and scaled, so they keep spanning the whole log range
static auto node(u32 index_on_32) -> u32 { return index_on_32 * (LUT_SIZE - 1) / 31; }

static auto retain_driver_modules() -> void {
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

static auto engine_pack() -> option<AssetFile> {
  auto error = std::error_code{};
  const auto* base_path = SDL_GetBasePath();
  const auto binary_dir = base_path ? std::filesystem::path(base_path) : std::filesystem::path();
  for (const auto& directory : {std::filesystem::current_path(error), binary_dir}) {
    if (directory.empty()) {
      continue;
    }

    const auto path = directory / "Assets" / "Shaders" / "engine.oxpack";
    if (std::filesystem::exists(path, error)) {
      return AssetFile::unpack(path);
    }
  }
  return nullopt;
}

static auto half_to_float(u16 half) -> f32 {
  const auto sign = (half & 0x8000_u32) << 16;
  const auto exponent = (half >> 10) & 0x1f_u32;
  const auto mantissa = half & 0x3ff_u32;
  if (exponent == 0) {
    const auto magnitude = std::ldexp(static_cast<f32>(mantissa), -24);
    return sign ? -magnitude : magnitude;
  }

  if (exponent == 31) {
    return std::bit_cast<f32>(sign | 0x7f800000_u32 | (mantissa << 13));
  }

  return std::bit_cast<f32>(sign | ((exponent + 112) << 23) | (mantissa << 13));
}

static auto srgb_encode(f32 value) -> f32 {
  value = std::clamp(value, 0.0f, 1.0f);
  return value < 0.0031308f ? 12.92f * value : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

static auto srgb_decode(f32 value) -> f32 {
  return value < 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

static auto log_encode(f32 value) -> f32 {
  return std::clamp(std::log2(std::max(value, 1e-10f) / 0.18f) / 16.0f + 0.5f, 0.0f, 1.0f);
}

static auto log_decode(u32 index) -> f32 {
  if (index == 0) {
    return 0.0f;
  }

  return 0.18f * std::exp2((static_cast<f32>(index) / static_cast<f32>(LUT_SIZE - 1) - 0.5f) * 16.0f);
}

static auto reference_filmic_curve(f32 x, const GPU::FilmicSettings& s) -> f32 {
  x = std::max(x, 0.0f);
  const auto l0 = (s.max_brightness - s.linear_start) * s.linear_length / s.contrast;
  const auto s0 = s.linear_start + l0;
  const auto s1 = s.linear_start + s.contrast * l0;
  const auto c2 = s.contrast * s.max_brightness / (s.max_brightness - s1);
  const auto cp = -c2 / s.max_brightness;
  const auto w0 = 1.0f - glm::smoothstep(0.0f, s.linear_start, x);
  const auto w2 = x >= s0 ? 1.0f : 0.0f;
  const auto w1 = 1.0f - w0 - w2;
  const auto toe = s.linear_start * std::pow(x / s.linear_start, s.black_tightness) + s.pedestal;
  const auto mid = s.linear_start + s.contrast * (x - s.linear_start);
  const auto shoulder = s.max_brightness - (s.max_brightness - s1) * std::exp(cp * (x - s0));
  return toe * w0 + mid * w1 + shoulder * w2;
}

static auto reference_filmic(glm::vec3 color, const GPU::FilmicSettings& s) -> glm::vec3 {
  const auto per_channel = glm::vec3(
    reference_filmic_curve(color.r, s),
    reference_filmic_curve(color.g, s),
    reference_filmic_curve(color.b, s)
  );
  const auto peak = std::max(color.r, std::max(color.g, color.b));
  const auto preserved = peak > 0.0f ? color * (reference_filmic_curve(peak, s) / peak) : glm::vec3(0.0f);
  return glm::mix(per_channel, preserved, std::clamp(s.hue_preservation, 0.0f, 1.0f));
}

static auto reference_agx(glm::vec3 color) -> glm::vec3 {
  const auto inset = glm::mat3(
    glm::vec3(0.856627153315983f, 0.137318972929847f, 0.11189821299995f),
    glm::vec3(0.0951212405381588f, 0.761241990602591f, 0.0767994186031903f),
    glm::vec3(0.0482516061458583f, 0.101439036467562f, 0.811302368396859f)
  );
  const auto outset = glm::mat3(
    glm::vec3(1.1271005818144368f, -0.1413297634984383f, -0.14132976349843826f),
    glm::vec3(-0.11060664309660323f, 1.157823702216272f, -0.11060664309660294f),
    glm::vec3(-0.016493938717834573f, -0.016493938717834257f, 1.2519364065950405f)
  );
  const auto rec2020_to_rec709 = glm::mat3(
    glm::vec3(1.6605f, -0.1246f, -0.0182f),
    glm::vec3(-0.5876f, 1.1329f, -0.1006f),
    glm::vec3(-0.0728f, -0.0083f, 1.1187f)
  );
  const auto rec709_to_rec2020 = glm::mat3(
    glm::vec3(0.6274f, 0.0691f, 0.0164f),
    glm::vec3(0.3293f, 0.9195f, 0.0880f),
    glm::vec3(0.0433f, 0.0113f, 0.8956f)
  );
  constexpr auto min_ev = -12.47393f;
  constexpr auto max_ev = 4.026069f;

  color = inset * (rec709_to_rec2020 * color);
  for (u32 channel = 0; channel < 3; channel++) {
    const auto encoded = std::clamp(
      (std::log2(std::max(color[channel], 1e-10f)) - min_ev) / (max_ev - min_ev),
      0.0f,
      1.0f
    );
    const auto x2 = encoded * encoded;
    const auto x4 = x2 * x2;
    color[channel] = 15.5f * x4 * x2 - 40.14f * x4 * encoded + 31.96f * x4 - 6.868f * x2 * encoded + 0.4298f * x2 +
                     0.1191f * encoded - 0.00232f;
  }

  const auto luma = glm::dot(color, glm::vec3(0.2126729f, 0.7151522f, 0.0721750f));
  for (u32 channel = 0; channel < 3; channel++) {
    const auto powered = std::pow(std::max(color[channel], 0.0f), 1.35f);
    color[channel] = std::max(luma + 1.4f * (powered - luma), 0.0f);
  }

  color = outset * color;
  for (u32 channel = 0; channel < 3; channel++) {
    color[channel] = std::pow(std::max(color[channel], 0.0f), 2.2f);
  }

  return glm::clamp(rec2020_to_rec709 * color, 0.0f, 1.0f);
}

struct BakedLut {
  std::vector<glm::vec3> texels = {};

  auto at(u32 x, u32 y, u32 z) const -> glm::vec3 { return texels[(usize(z) * LUT_SIZE + y) * LUT_SIZE + x]; }

  // the same texel-center remap the tonemap shader samples with
  auto lookup(glm::vec3 color) const -> glm::vec3 {
    const auto coordinate = glm::vec3(log_encode(color.r), log_encode(color.g), log_encode(color.b)) *
                            static_cast<f32>(LUT_SIZE - 1);
    const auto base = glm::min(glm::uvec3(glm::floor(coordinate)), glm::uvec3(LUT_SIZE - 2));
    const auto fraction = coordinate - glm::vec3(base);
    auto result = glm::vec3(0.0f);
    for (u32 corner = 0; corner < 8; corner++) {
      const auto offset = glm::uvec3(corner & 1, (corner >> 1) & 1, (corner >> 2) & 1);
      const auto weight = glm::mix(1.0f - fraction.x, fraction.x, offset.x == 1) *
                          glm::mix(1.0f - fraction.y, fraction.y, offset.y == 1) *
                          glm::mix(1.0f - fraction.z, fraction.z, offset.z == 1);
      const auto texel = base + offset;
      result += weight * at(texel.x, texel.y, texel.z);
    }
    return result;
  }
};

struct ColorGradingGPU : testing::Test {
  struct WorkingDirectory {
    std::filesystem::path previous = std::filesystem::current_path();
    std::filesystem::path temporary = std::filesystem::temp_directory_path() /
                                      ("oxylus-color-grading-gpu-" +
                                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

    WorkingDirectory() {
      std::filesystem::create_directories(temporary);
      std::filesystem::current_path(temporary);
    }
    ~WorkingDirectory() {
      std::filesystem::current_path(previous);
      std::filesystem::remove_all(temporary);
    }
  };

  vkb::Instance instance = {};
  vkb::Device device = {};
  std::unique_ptr<WorkingDirectory> directory = {};
  std::unique_ptr<RenderContext> render = {};
  bool is_device_created = false;

  auto SetUp() -> void override {
    auto builder = vkb::InstanceBuilder{};
    builder.set_headless().require_api_version(1, 3, 0).enable_extension(VK_KHR_SURFACE_EXTENSION_NAME);
    const auto built_instance = builder.build();
    if (!built_instance) {
      GTEST_SKIP() << "headless Vulkan instance unavailable: " << built_instance.error().message();
    }

    instance = *built_instance;
    auto features = VkPhysicalDeviceFeatures{};
    features.shaderInt64 = VK_TRUE;
    features.shaderInt16 = VK_TRUE;
    features.shaderStorageImageReadWithoutFormat = VK_TRUE;
    features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    features.shaderStorageImageExtendedFormats = VK_TRUE;
    auto features11 = VkPhysicalDeviceVulkan11Features{};
    features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    features11.storageBuffer16BitAccess = VK_TRUE;
    features11.uniformAndStorageBuffer16BitAccess = VK_TRUE;
    features11.storagePushConstant16 = VK_TRUE;
    auto features12 = VkPhysicalDeviceVulkan12Features{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.timelineSemaphore = VK_TRUE;
    features12.bufferDeviceAddress = VK_TRUE;
    features12.scalarBlockLayout = VK_TRUE;
    features12.vulkanMemoryModel = VK_TRUE;
    features12.vulkanMemoryModelDeviceScope = VK_TRUE;
    features12.descriptorIndexing = VK_TRUE;
    features12.runtimeDescriptorArray = VK_TRUE;
    features12.descriptorBindingPartiallyBound = VK_TRUE;
    features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    features12.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
    features12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    features12.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
    features12.shaderInt8 = VK_TRUE;
    features12.uniformAndStorageBuffer8BitAccess = VK_TRUE;
    features12.storageBuffer8BitAccess = VK_TRUE;
    auto features13 = VkPhysicalDeviceVulkan13Features{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.synchronization2 = VK_TRUE;
    features13.dynamicRendering = VK_TRUE;
    features13.maintenance4 = VK_TRUE;
    features13.robustImageAccess = VK_TRUE;
    const auto physical = vkb::PhysicalDeviceSelector(instance)
                            .require_present(false)
                            .add_required_extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME)
                            .set_minimum_version(1, 3)
                            .set_required_features(features)
                            .set_required_features_11(features11)
                            .set_required_features_12(features12)
                            .set_required_features_13(features13)
                            .select();
    if (!physical) {
      GTEST_SKIP() << "no Vulkan device supports the color grading shader: " << physical.error().message();
    }

    const auto built_device = vkb::DeviceBuilder(*physical).build();
    ASSERT_TRUE(built_device) << built_device.error().message();
    device = *built_device;
    is_device_created = true;

    auto context = vir::Context::create(
      {.instance = instance.instance,
       .physical_device = physical->physical_device,
       .device = device.device,
       .get_instance_proc_addr = instance.fp_vkGetInstanceProcAddr}
    );
    ASSERT_TRUE(context);
    const auto queue = device.get_queue_index(vkb::QueueType::graphics);
    ASSERT_TRUE(queue);
    ASSERT_TRUE((*context)->create_command_queue(*queue, vir::DomainFlag::Graphics));
    directory = std::make_unique<WorkingDirectory>();
    render = std::make_unique<RenderContext>();
    ASSERT_TRUE(render->initialize_graph(std::move(*context)));

    constexpr auto capacity = 64_u32;
    const auto bindings = std::array{
      VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_SAMPLER, capacity, VK_SHADER_STAGE_ALL, nullptr},
      VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, capacity, VK_SHADER_STAGE_ALL, nullptr},
      VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, capacity, VK_SHADER_STAGE_ALL, nullptr},
    };
    constexpr auto binding_flag = VkDescriptorBindingFlags(
      VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT
    );
    auto descriptor_set = render->create_persistent_descriptor_set(
      bindings,
      std::array<VkDescriptorBindingFlags, 3>{binding_flag, binding_flag, binding_flag}
    );
    ASSERT_TRUE(descriptor_set);
    render->resources->descriptor_set = std::move(*descriptor_set);

    const auto pack = engine_pack();
    ASSERT_TRUE(pack) << "engine.oxpack must be built before this test";
    for (const auto& entry : pack->entries) {
      const auto* shader = std::get_if<ShaderPipelineData>(&entry.data);
      if (shader && shader->module_name == "color_grading_bake") {
        ASSERT_TRUE(render->create_pipeline(*shader));
      }
    }
    ASSERT_NE(render->pipeline("color_grading_bake"), vir::PipelineID::Invalid);
  }

  auto TearDown() -> void override {
    render.reset();
    directory.reset();
    if (is_device_created) {
      retain_driver_modules();
      vkb::destroy_device(device);
    }

    if (instance.instance != VK_NULL_HANDLE) {
      vkb::destroy_instance(instance);
    }
  }

  // creates a user strip whose entry at (r, g, b) is the inverted grid coordinate, returns its bindless view index
  auto create_inverting_strip(this ColorGradingGPU& self, ImageID& image, ImageViewID& view) -> u32 {
    auto& render_context = *self.render;
    const auto width = STRIP_SIZE * STRIP_SIZE;
    auto pixels = std::vector<u8>(usize(width) * STRIP_SIZE * 4);
    for (u32 y = 0; y < STRIP_SIZE; y++) {
      for (u32 x = 0; x < width; x++) {
        const auto blue = x / STRIP_SIZE;
        const auto red = x % STRIP_SIZE;
        const auto texel = (usize(y) * width + x) * 4;
        const auto invert = [](u32 coordinate) -> u8 {
          return static_cast<u8>(std::lround((1.0f - static_cast<f32>(coordinate) / (STRIP_SIZE - 1)) * 255.0f));
        };
        pixels[texel + 0] = invert(red);
        pixels[texel + 1] = invert(y);
        pixels[texel + 2] = invert(blue);
        pixels[texel + 3] = 255;
      }
    }

    const auto allocated = render_context.allocate_image({
      .extent = {width, STRIP_SIZE, 1},
      .format = vir::Format::RGBA8Unorm,
      .usage = vir::ImageUsageFlags::Sampled | vir::ImageUsageFlags::TransferDst,
      .name = "test grading strip",
    });
    EXPECT_TRUE(allocated);
    image = *allocated;
    const auto created = render_context.image(image);
    const auto range = created.subresource_range();
    const auto allocated_view = render_context.allocate_image_view({
      .image = created.handle,
      .format = vir::Format::RGBA8Unorm,
      .view_type = vir::ImageViewType::e2D,
      .range = range,
      .usage = vir::ImageUsageFlags::Sampled,
      .name = "test grading strip view",
    });
    EXPECT_TRUE(allocated_view);
    view = *allocated_view;

    const auto upload = render_context.persistent->allocate_buffer(
      {.size = pixels.size(), .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .location = vir::MemoryLocation::CpuToGpu}
    );
    EXPECT_TRUE(upload);
    EXPECT_TRUE(upload->write<u8>(0, pixels));
    EXPECT_TRUE(render_context.persistent->flush_buffer(*upload));

    auto module = vir::Module{};
    auto strip = module.copy_buffer_to_image(
      module.import_buffer(*upload, vir::Access::HostWrite),
      module.import_image(created, VK_IMAGE_LAYOUT_UNDEFINED, vir::Access::None)
    );
    module.export_to(strip, vir::Access::FragmentSampled);
    EXPECT_TRUE(render_context.execute_blocking(std::move(module)));
    render_context.persistent->deallocate_buffer(*upload);

    return SlotMap_decode_id(view).index;
  }

  auto bake(this ColorGradingGPU& self, const GPU::ColorGradingSettings& settings, GPU::TonemapType tonemap)
    -> BakedLut {
    auto& render_context = *self.render;
    constexpr auto texel_count = LUT_SIZE * LUT_SIZE * LUT_SIZE;
    constexpr auto usage = vir::ImageUsageFlags::Sampled | vir::ImageUsageFlags::Storage |
                           vir::ImageUsageFlags::TransferSrc;

    const auto image_id = render_context.allocate_image({
      .extent = {LUT_SIZE, LUT_SIZE, LUT_SIZE},
      .format = vir::Format::RGBA16Sfloat,
      .usage = usage,
      .image_type = vir::ImageType::e3D,
      .view_type = vir::ImageViewType::e3D,
      .name = "test grading lut",
    });
    EXPECT_TRUE(image_id);
    const auto image = render_context.image(*image_id);
    const auto view_id = render_context.allocate_image_view({
      .image = image.handle,
      .format = vir::Format::RGBA16Sfloat,
      .view_type = vir::ImageViewType::e3D,
      .range = image.subresource_range(),
      .usage = usage,
      .name = "test grading lut view",
    });
    EXPECT_TRUE(view_id);
    auto attachment = vir::ImageAttachment::from_image(image, VK_IMAGE_LAYOUT_UNDEFINED);
    attachment.image_view = render_context.image_view(*view_id).handle;
    attachment.view_type = vir::ImageViewType::e3D;
    attachment.subresource_range = image.subresource_range();

    const auto readback = render_context.persistent->allocate_buffer(
      {.size = texel_count * 4 * sizeof(u16),
       .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
       .location = vir::MemoryLocation::GpuToCpu}
    );
    EXPECT_TRUE(readback);

    auto module = vir::Module{};
    const auto is_dirty = module.declare_bool_var("color grading dirty", false);
    const auto push = module.declare_bytes_var("color grading push", sizeof(GPU::ColorGradingSettings));
    const auto lut = RendererInstance::bake_color_grading(
      module,
      render_context,
      module.import_attachment(attachment, vir::Access::None),
      is_dirty,
      push,
      tonemap
    );
    auto downloaded = module.copy_image_to_buffer_region(
      lut,
      module.import_buffer(*readback, vir::Access::None),
      {.buffer_offset = 0, .image_extent = {LUT_SIZE, LUT_SIZE, LUT_SIZE}, .mip_level = 0}
    );
    module.export_to(downloaded, vir::Access::HostRead);

    auto program = render_context.compile(std::move(module));
    EXPECT_TRUE(program);
    program->set(is_dirty, true);
    program->set_bytes(push, settings);
    EXPECT_TRUE(render_context.execute_blocking(*program));
    EXPECT_TRUE(render_context.persistent->invalidate_buffer(*readback));

    auto result = BakedLut{};
    result.texels.resize(texel_count);
    const auto* halves = reinterpret_cast<const u16*>(readback->mapped);
    for (u32 index = 0; index < texel_count; index++) {
      result.texels[index] = {
        half_to_float(halves[index * 4 + 0]),
        half_to_float(halves[index * 4 + 1]),
        half_to_float(halves[index * 4 + 2]),
      };
    }

    render_context.persistent->deallocate_buffer(*readback);
    render_context.destroy_image_view(*view_id);
    render_context.destroy_image(*image_id);
    render_context.seal_retirements({});
    render_context.collect_retired();

    return result;
  }
};

TEST_F(ColorGradingGPU, NeutralSettingsAreIdentityThroughTheLookup) {
  const auto lut = bake({}, GPU::TonemapType::None);
  EXPECT_EQ(lut.at(0, 0, 0), glm::vec3(0.0f));

  for (const auto color :
       {glm::vec3(0.01f, 0.5f, 1.0f),
        glm::vec3(0.18f),
        glm::vec3(0.05f, 0.9f, 0.3f),
        glm::vec3(1.0f, 0.02f, 0.4f),
        glm::vec3(0.7f, 0.7f, 0.011f)}) {
    const auto result = lut.lookup(color);
    for (u32 channel = 0; channel < 3; channel++) {
      EXPECT_NEAR(result[channel], color[channel], color[channel] * 0.025f)
        << "channel " << channel << " of " << color.r << " " << color.g << " " << color.b;
    }
  }

  EXPECT_NEAR(lut.lookup(glm::vec3(0.0f)).r, 0.0f, 1e-3f);
  EXPECT_NEAR(lut.lookup(glm::vec3(0.0f)).g, 0.0f, 1e-3f);
  EXPECT_NEAR(lut.lookup(glm::vec3(0.0f)).b, 0.0f, 1e-3f);
}

TEST_F(ColorGradingGPU, UserStripReplacesAndBlendsInEncodedSpace) {
  auto image = ImageID::Invalid;
  auto view = ImageViewID::Invalid;
  const auto view_index = create_inverting_strip(image, view);

  const auto indices = std::array{node(3), node(10), node(17), node(24), node(31)};
  for (const auto contribution : {1.0f, 0.5f}) {
    SCOPED_TRACE(contribution);
    auto settings = GPU::ColorGradingSettings{};
    settings.user_lut_view_index = view_index;
    settings.lut_contribution = contribution;
    const auto lut = bake(settings, GPU::TonemapType::None);

    for (const auto x : indices) {
      for (const auto y : indices) {
        for (const auto z : indices) {
          const auto input = glm::vec3(log_decode(x), log_decode(y), log_decode(z));
          const auto actual = lut.at(x, y, z);
          for (u32 channel = 0; channel < 3; channel++) {
            const auto encoded = srgb_encode(input[channel]);
            const auto expected = srgb_decode(glm::mix(encoded, 1.0f - encoded, contribution));
            EXPECT_NEAR(actual[channel], expected, 0.01f + expected * 0.02f)
              << "texel " << x << " " << y << " " << z << " channel " << channel;
          }
        }
      }
    }
  }

  render->destroy_image_view(view);
  render->destroy_image(image);
  render->seal_retirements({});
  render->collect_retired();
}

TEST_F(ColorGradingGPU, ZeroSaturationBakesGrey) {
  auto settings = GPU::ColorGradingSettings{};
  settings.saturation = 0.0f;
  const auto lut = bake(settings, GPU::TonemapType::None);
  auto brightest = 0.0f;
  for (const auto& texel : lut.texels) {
    EXPECT_NEAR(texel.r, texel.g, 2e-3f + texel.g * 2e-3f);
    EXPECT_NEAR(texel.g, texel.b, 2e-3f + texel.g * 2e-3f);
    brightest = std::max(brightest, texel.g);
  }

  EXPECT_GT(brightest, 0.1f);
}

TEST_F(ColorGradingGPU, HigherTemperatureWarmsTheImage) {
  auto settings = GPU::ColorGradingSettings{};
  settings.temperature = 9000.0f;
  const auto warm = bake(settings, GPU::TonemapType::None).lookup(glm::vec3(0.18f));
  EXPECT_GT(warm.r, warm.b);

  settings.temperature = 4000.0f;
  const auto cool = bake(settings, GPU::TonemapType::None).lookup(glm::vec3(0.18f));
  EXPECT_LT(cool.r, cool.b);

  settings.temperature = 6500.0f;
  settings.tint = 1.0f;
  const auto magenta = bake(settings, GPU::TonemapType::None).lookup(glm::vec3(0.18f));
  EXPECT_GT(magenta.g, 0.0f);
  EXPECT_LT(magenta.g, std::min(magenta.r, magenta.b));
}

TEST_F(ColorGradingGPU, FilmicBakesMatchTheReferenceCurve) {
  auto custom = GPU::FilmicSettings{};
  custom.contrast = 1.3f;
  custom.black_tightness = 1.6f;
  custom.max_brightness = 0.95f;
  const auto indices = std::
    array{node(0), node(3), node(8), node(12), node(15), node(16), node(17), node(20), node(24), node(28), node(31)};
  for (const auto& filmic : {GPU::FilmicSettings{}, custom}) {
    SCOPED_TRACE(filmic.contrast);
    auto settings = GPU::ColorGradingSettings{};
    settings.filmic = filmic;
    const auto lut = bake(settings, GPU::TonemapType::Filmic);
    for (const auto x : indices) {
      for (const auto y : indices) {
        for (const auto z : indices) {
          const auto expected = reference_filmic(glm::vec3(log_decode(x), log_decode(y), log_decode(z)), filmic);
          const auto actual = lut.at(x, y, z);
          for (u32 channel = 0; channel < 3; channel++) {
            EXPECT_NEAR(actual[channel], expected[channel], 3e-4f + std::abs(expected[channel]) * 1.5e-3f)
              << "texel " << x << " " << y << " " << z << " channel " << channel;
          }
        }
      }
    }
  }
}

TEST_F(ColorGradingGPU, FilmicHuePreservationKeepsChannelRatios) {
  const auto input = glm::vec3(log_decode(node(26)), log_decode(node(21)), log_decode(node(15)));
  const auto input_rg = input.r / input.g;
  const auto input_gb = input.g / input.b;

  auto settings = GPU::ColorGradingSettings{};
  settings.filmic.hue_preservation = 1.0f;
  const auto preserved = bake(settings, GPU::TonemapType::Filmic).at(node(26), node(21), node(15));
  EXPECT_NEAR(preserved.r / preserved.g, input_rg, input_rg * 0.01f);
  EXPECT_NEAR(preserved.g / preserved.b, input_gb, input_gb * 0.01f);

  settings.filmic.hue_preservation = 0.0f;
  const auto per_channel = bake(settings, GPU::TonemapType::Filmic).at(node(26), node(21), node(15));
  EXPECT_LT(std::log(per_channel.r / per_channel.g), std::log(input_rg));
  EXPECT_LT(std::log(per_channel.g / per_channel.b), std::log(input_gb));
}

TEST_F(ColorGradingGPU, ChangingFilmicSettingsChangesTheBake) {
  auto settings = GPU::ColorGradingSettings{};
  const auto before = settings;
  settings.filmic.pedestal = 0.02f;
  EXPECT_NE(std::memcmp(&before, &settings, sizeof(GPU::ColorGradingSettings)), 0);

  const auto baked_before = bake(before, GPU::TonemapType::Filmic);
  const auto baked_after = bake(settings, GPU::TonemapType::Filmic);
  EXPECT_GT(baked_after.at(0, 0, 0).r, baked_before.at(0, 0, 0).r + 0.01f);
}

TEST_F(ColorGradingGPU, AgXBakesMatchTheReference) {
  const auto lut = bake({}, GPU::TonemapType::AgX);
  EXPECT_NEAR(lut.at(0, 0, 0).r, 0.0f, 1e-3f);
  EXPECT_NEAR(lut.at(0, 0, 0).g, 0.0f, 1e-3f);
  EXPECT_NEAR(lut.at(0, 0, 0).b, 0.0f, 1e-3f);

  const auto indices = std::
    array{node(0), node(3), node(8), node(12), node(15), node(16), node(17), node(20), node(24), node(28), node(31)};
  for (const auto x : indices) {
    for (const auto y : indices) {
      for (const auto z : indices) {
        const auto expected = reference_agx(glm::vec3(log_decode(x), log_decode(y), log_decode(z)));
        const auto actual = lut.at(x, y, z);
        for (u32 channel = 0; channel < 3; channel++) {
          EXPECT_NEAR(actual[channel], expected[channel], 2e-3f)
            << "texel " << x << " " << y << " " << z << " channel " << channel;
        }
      }
    }
  }

  auto previous = 0.0f;
  for (u32 index = 0; index < LUT_SIZE; index++) {
    const auto grey = lut.at(index, index, index);
    EXPECT_GE(grey.g, 0.0f);
    EXPECT_LE(grey.g, 1.0f);
    EXPECT_GE(grey.g, previous - 1e-3f) << "grey node " << index;
    previous = grey.g;
  }
}
} // namespace ox
