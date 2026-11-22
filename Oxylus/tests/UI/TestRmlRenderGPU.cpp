#include <RmlUi/Core.h>
#include <SDL3/SDL_filesystem.h>
#include <VkBootstrap.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <string_view>
#include <vector>
#include <vir/Context.hpp>
#include <vir/Program.hpp>
#include <vir/RenderGraph.hpp>

#ifdef OX_PLATFORM_LINUX
  #include <dlfcn.h>
  #include <link.h>
#endif

#include "Asset/AssetFile.hpp"
#include "Core/App.hpp"
#include "Render/RenderContext.hpp"
#include "Render/RenderResources.hpp"
#include "UI/RmlRenderer.hpp"

namespace ox {
constexpr static auto SIZE = 64_u32;

static VKAPI_ATTR auto VKAPI_CALL rml_validation_message(
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

struct Pixel {
  i32 r = 0;
  i32 g = 0;
  i32 b = 0;
};

class RmlRenderGPU : public ::testing::Test {
protected:
  void SetUp() override {
    auto builder = vkb::InstanceBuilder{};
    builder.set_headless().require_api_version(1, 3, 0).enable_extension(VK_KHR_SURFACE_EXTENSION_NAME);
    if (std::getenv("OX_RMLUI_GPU_VALIDATION")) {
      builder.request_validation_layers()
        .set_debug_callback(rml_validation_message)
        .set_debug_callback_user_data_pointer(&validation_errors)
        .add_validation_feature_enable(VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT);
    }

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
    auto features11 = VkPhysicalDeviceVulkan11Features{};
    features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    features11.storageBuffer16BitAccess = VK_TRUE;
    features11.shaderDrawParameters = VK_TRUE;
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
    features12.shaderInt8 = VK_TRUE;
    features12.storageBuffer8BitAccess = VK_TRUE;
    auto features13 = VkPhysicalDeviceVulkan13Features{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.synchronization2 = VK_TRUE;
    features13.dynamicRendering = VK_TRUE;
    features13.maintenance4 = VK_TRUE;
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
      GTEST_SKIP() << "no Vulkan device supports the rmlui shaders: " << physical.error().message();
    }

    const auto get_format_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
      instance.fp_vkGetInstanceProcAddr(instance.instance, "vkGetPhysicalDeviceFormatProperties")
    );
    auto stencil_properties = VkFormatProperties{};
    get_format_properties(physical->physical_device, VK_FORMAT_D32_SFLOAT_S8_UINT, &stencil_properties);
    if ((stencil_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0) {
      GTEST_SKIP() << "the device can't render to D32_SFLOAT_S8_UINT";
    }

    const auto built_device = vkb::DeviceBuilder(*physical).build();
    ASSERT_TRUE(built_device) << built_device.error().message();
    device = *built_device;
    is_device_created = true;

    auto vir_context = vir::Context::create(
      {.instance = instance.instance,
       .physical_device = physical->physical_device,
       .device = device.device,
       .get_instance_proc_addr = instance.fp_vkGetInstanceProcAddr}
    );
    ASSERT_TRUE(vir_context);
    const auto queue = device.get_queue_index(vkb::QueueType::graphics);
    ASSERT_TRUE(queue);
    ASSERT_TRUE((*vir_context)->create_command_queue(*queue, vir::DomainFlag::Graphics));
    auto render = std::make_unique<RenderContext>();
    ASSERT_TRUE(render->initialize_graph(std::move(*vir_context)));

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
      if (shader && std::string_view(shader->module_name).starts_with("rmlui")) {
        ASSERT_TRUE(render->create_pipeline(*shader)) << shader->module_name;
      }
    }

    static char arg0[] = "TestRmlRenderGPU";
    static char* argv[] = {arg0, nullptr};
    app = std::make_unique<App>(1, argv);
    app->with_render_context(std::move(render));

    ASSERT_TRUE(Rml::Initialise());
    is_rml_initialised = true;
    renderer = std::make_unique<RmlRenderer>();
    context = Rml::CreateContext("rml gpu test", {static_cast<i32>(SIZE), static_cast<i32>(SIZE)}, renderer.get());
    ASSERT_NE(context, nullptr);
  }

  void TearDown() override {
    if (context) {
      Rml::RemoveContext(context->GetName());
      context = nullptr;
    }

    if (is_rml_initialised) {
      // what RmlUi still holds goes back through the renderer, which has to outlive this
      Rml::ReleaseRenderManagers();
      renderer.reset();
      Rml::Shutdown();
    }

    if (app) {
      App::get_rendercontext().wait();
      app.reset();
    }

    if (is_device_created) {
      retain_driver_modules();
      vkb::destroy_device(device);
    }

    if (instance.instance != VK_NULL_HANDLE) {
      vkb::destroy_instance(instance);
    }

    EXPECT_EQ(validation_errors.load(std::memory_order_relaxed), 0u);
  }

  auto load(this RmlRenderGPU& self, std::string_view body) -> Rml::ElementDocument* {
    const auto rml = std::string(R"(<rml><head><style>
      body { width: 64px; height: 64px; margin: 0; }
      div { position: absolute; display: block; }
    </style></head><body>)") +
                     std::string(body) + "</body></rml>";
    auto* document = self.context->LoadDocumentFromMemory(rml);
    EXPECT_NE(document, nullptr);
    if (document) {
      document->Show();
    }

    return document;
  }

  // one RmlUi frame, composited over opaque black
  auto render_frame(this RmlRenderGPU& self) -> std::vector<Pixel> {
    auto& render_context = App::get_rendercontext();
    self.renderer->begin_frame({static_cast<i32>(SIZE), static_cast<i32>(SIZE)});
    self.context->Update();
    self.context->Render();
    std::ignore = render_context.new_frame();
    self.renderer->render(render_context);

    const auto readback = render_context.persistent->allocate_buffer(
      {.size = SIZE * SIZE * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .location = vir::MemoryLocation::GpuToCpu}
    );
    EXPECT_TRUE(readback);
    if (!readback) {
      return {};
    }

    auto module = vir::Module{};
    const auto target = module.clear(
      module.transient_image({.extent = {SIZE, SIZE, 1}, .format = vir::Format::RGBA8Unorm, .name = "test target"}),
      vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 1.0f)
    );
    auto variables = RmlCompositeVariables{};
    const auto composited = RmlRenderer::record_composite(module, render_context, target, {SIZE, SIZE, 1}, variables);
    const auto downloaded = module.copy_image_to_buffer(composited, module.import_buffer(*readback, vir::Access::None));
    module.export_to(downloaded, vir::Access::HostRead);

    auto program = render_context.compile(std::move(module));
    EXPECT_TRUE(program);
    if (!program) {
      return {};
    }

    self.renderer->bind_composite(*program, variables);
    EXPECT_TRUE(render_context.execute_blocking(*program));
    EXPECT_TRUE(render_context.persistent->invalidate_buffer(*readback));

    auto pixels = std::vector<Pixel>(SIZE * SIZE);
    const auto* bytes = static_cast<const u8*>(readback->mapped);
    for (u32 i = 0; i < SIZE * SIZE; i++) {
      pixels[i] = {bytes[i * 4 + 0], bytes[i * 4 + 1], bytes[i * 4 + 2]};
    }

    render_context.persistent->deallocate_buffer(*readback);
    render_context.seal_retirements({});
    render_context.collect_retired();

    return pixels;
  }

  // layout settles on the first frame, decorators and shadows are generated on demand
  auto render_settled(this RmlRenderGPU& self) -> std::vector<Pixel> {
    std::ignore = self.render_frame();
    return self.render_frame();
  }

  static auto at(const std::vector<Pixel>& pixels, u32 x, u32 y) -> Pixel { return pixels[y * SIZE + x]; }

  vkb::Instance instance = {};
  vkb::Device device = {};
  bool is_device_created = false;
  bool is_rml_initialised = false;
  std::atomic<u32> validation_errors = {0};
  std::unique_ptr<App> app = {};
  std::unique_ptr<RmlRenderer> renderer = {};
  Rml::Context* context = nullptr;
};

TEST_F(RmlRenderGPU, DrawsPlainBoxesThroughTheComposite) {
  auto* document = load(
    R"(<div id="box" style="left: 0px; top: 0px; width: 32px; height: 32px; background-color: #ff0000;"></div>)"
  );
  ASSERT_NE(document, nullptr);

  auto pixels = render_settled();
  ASSERT_FALSE(pixels.empty());
  EXPECT_EQ(at(pixels, 16, 16).r, 255);
  EXPECT_EQ(at(pixels, 16, 16).g, 0);
  EXPECT_EQ(at(pixels, 48, 48).r, 0);

  // the cached program reads the frame it runs, not the one it was recorded for
  document->GetElementById("box")->SetProperty("background-color", "#0000ff");
  pixels = render_frame();
  EXPECT_EQ(at(pixels, 16, 16).r, 0);
  EXPECT_EQ(at(pixels, 16, 16).b, 255);
}

TEST_F(RmlRenderGPU, NothingToDrawLeavesTheTargetAlone) {
  const auto pixels = render_settled();
  ASSERT_FALSE(pixels.empty());
  EXPECT_EQ(at(pixels, 32, 32).r, 0);
  EXPECT_EQ(at(pixels, 32, 32).g, 0);
  EXPECT_EQ(at(pixels, 32, 32).b, 0);
}

TEST_F(RmlRenderGPU, DrawsLinearGradients) {
  load(
    R"(<div style="left: 0px; top: 0px; width: 64px; height: 16px; decorator: linear-gradient(to right, #000000, #ffffff);"></div>)"
  );

  const auto pixels = render_settled();
  ASSERT_FALSE(pixels.empty());
  EXPECT_LT(at(pixels, 1, 8).r, 16);
  EXPECT_GT(at(pixels, 62, 8).r, 239);
  EXPECT_NEAR(at(pixels, 32, 8).r, 131, 10);
  EXPECT_EQ(at(pixels, 32, 8).r, at(pixels, 32, 8).b);
}

// RmlUi masks a transformed element's overflow and drops the scissor, which couldn't follow the rotation
TEST_F(RmlRenderGPU, ClipsTransformedOverflowToTheMask) {
  // in flow, RmlUi only masks when the content overflows, and absolutely positioned children don't count
  load(R"(<div style="left: 16px; top: 16px; width: 32px; height: 32px; overflow: hidden; transform: rotate(45deg);">
    <div style="position: static; width: 64px; height: 64px; background-color: #ff0000;"></div>
  </div>)");

  const auto pixels = render_settled();
  ASSERT_FALSE(pixels.empty());
  EXPECT_EQ(at(pixels, 32, 32).r, 255);
  EXPECT_EQ(at(pixels, 32, 14).r, 255);
  // the child reaches here, rotated past the parent's lower corner
  EXPECT_EQ(at(pixels, 32, 60).r, 0);
}

TEST_F(RmlRenderGPU, ClipsOverflowToRoundedCorners) {
  load(R"(<div style="left: 0px; top: 0px; width: 64px; height: 64px; overflow: hidden; border-radius: 32px;">
    <div style="left: 0px; top: 0px; width: 64px; height: 64px; background-color: #ff0000;"></div>
  </div>)");

  const auto pixels = render_settled();
  ASSERT_FALSE(pixels.empty());
  EXPECT_EQ(at(pixels, 32, 32).r, 255);
  EXPECT_EQ(at(pixels, 32, 1).r, 255);
  EXPECT_EQ(at(pixels, 1, 1).r, 0);
  EXPECT_EQ(at(pixels, 62, 62).r, 0);
}

TEST_F(RmlRenderGPU, AppliesOpacityAndColorMatrixFilters) {
  load(R"(
    <div style="left: 0px; top: 0px; width: 32px; height: 32px; background-color: #ff0000; filter: opacity(0.5);"></div>
    <div style="left: 32px; top: 0px; width: 32px; height: 32px; background-color: #ff0000; filter: grayscale(1);"></div>
  )");

  const auto pixels = render_settled();
  ASSERT_FALSE(pixels.empty());
  EXPECT_NEAR(at(pixels, 16, 16).r, 128, 3);
  EXPECT_EQ(at(pixels, 16, 16).g, 0);
  EXPECT_NEAR(at(pixels, 48, 16).r, 54, 3);
  EXPECT_NEAR(at(pixels, 48, 16).g, 54, 3);
  EXPECT_NEAR(at(pixels, 48, 16).b, 54, 3);
}

TEST_F(RmlRenderGPU, BlursALayer) {
  load(
    R"(<div style="left: 24px; top: 24px; width: 16px; height: 16px; background-color: #ff0000; filter: blur(4px);"></div>)"
  );

  const auto pixels = render_settled();
  ASSERT_FALSE(pixels.empty());
  const auto center = at(pixels, 32, 32).r;
  EXPECT_GT(center, 180);
  EXPECT_LT(center, 250);
  const auto spread = at(pixels, 21, 32).r;
  EXPECT_GT(spread, 5);
  EXPECT_LT(spread, 128);
  EXPECT_LE(at(pixels, 2, 2).r, 1);
}

TEST_F(RmlRenderGPU, DropsAShadow) {
  load(R"(
    <div style="left: 8px; top: 8px; width: 16px; height: 16px; background-color: #ff0000; filter: drop-shadow(#0000ff 8px 8px 0px);"></div>
    <div style="left: 40px; top: 8px; width: 8px; height: 8px; background-color: #ff0000; filter: drop-shadow(#0000ff 8px 8px 2px);"></div>
  )");

  const auto pixels = render_settled();
  ASSERT_FALSE(pixels.empty());
  EXPECT_EQ(at(pixels, 12, 12).r, 255);
  EXPECT_EQ(at(pixels, 20, 20).r, 255);
  EXPECT_EQ(at(pixels, 28, 28).r, 0);
  EXPECT_EQ(at(pixels, 28, 28).b, 255);
  EXPECT_EQ(at(pixels, 4, 4).b, 0);

  // the blurred one still lands where it's offset to
  EXPECT_GT(at(pixels, 52, 20).b, 100);
  EXPECT_EQ(at(pixels, 52, 20).r, 0);
}

// a box shadow is drawn into a layer once and saved as a texture, every frame after reuses it
TEST_F(RmlRenderGPU, KeepsABoxShadowAcrossFrames) {
  load(
    R"(<div style="left: 24px; top: 24px; width: 16px; height: 16px; background-color: #ff0000; box-shadow: #00ff00 0px 0px 0px 4px;"></div>)"
  );

  for (const auto& pixels : {render_frame(), render_frame(), render_frame()}) {
    ASSERT_FALSE(pixels.empty());
    EXPECT_EQ(at(pixels, 32, 32).r, 255);
    EXPECT_EQ(at(pixels, 22, 32).g, 255);
    EXPECT_EQ(at(pixels, 22, 32).r, 0);
    EXPECT_EQ(at(pixels, 10, 10).g, 0);
  }
}

TEST_F(RmlRenderGPU, MasksWithAnImage) {
  load(
    R"(<div style="left: 0px; top: 0px; width: 64px; height: 16px; background-color: #ff0000; mask-image: linear-gradient(to right, #ffffffff, #ffffff00);"></div>)"
  );

  const auto pixels = render_settled();
  ASSERT_FALSE(pixels.empty());
  EXPECT_GT(at(pixels, 1, 8).r, 240);
  EXPECT_LT(at(pixels, 62, 8).r, 16);
  EXPECT_NEAR(at(pixels, 32, 8).r, 128, 20);
}
} // namespace ox
