#include <RmlUi/Core.h>
#include <SDL3/SDL_keycode.h>
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
#include "Asset/AssetManager.hpp"
#include "Audio/AudioEngine.hpp"
#include "Core/App.hpp"
#include "Core/Input.hpp"
#include "Core/Keycodes.hpp"
#include "Render/RenderContext.hpp"
#include "Render/RenderResources.hpp"
#include "Render/Renderer.hpp"
#include "UI/RmlRenderer.hpp"
#include "UI/RmlUI.hpp"
#include "UI/RmlView.hpp"
#include "Utils/Timestep.hpp"

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

// the saved layer holds premultiplied colour, drawing it as straight would square a translucent shadow's alpha
TEST_F(RmlRenderGPU, KeepsATranslucentBoxShadowPremultiplied) {
  load(
    R"(<div style="left: 24px; top: 24px; width: 16px; height: 16px; background-color: #ff0000; box-shadow: #00ff0080 0px 0px 0px 4px;"></div>)"
  );

  const auto pixels = render_settled();
  ASSERT_FALSE(pixels.empty());
  EXPECT_NEAR(at(pixels, 22, 32).g, 128, 3);
  EXPECT_EQ(at(pixels, 32, 32).r, 255);
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

// RmlUI's routing needs views, and a view needs a renderer
class RmlInputRouting : public RmlRenderGPU {
protected:
  void SetUp() override {
    RmlRenderGPU::SetUp();
    if (IsSkipped() || HasFatalFailure()) {
      return;
    }

    app->with<AudioEngine>().with<AssetManager>().with<Input>().with<Renderer>().with<RmlUI>();
    back = std::make_unique<RmlView>("back");
    front = std::make_unique<RmlView>("front");
    for (auto* view : {back.get(), front.get()}) {
      auto* document = view->context()->LoadDocumentFromMemory(R"(<rml><head><style>
        body { width: 64px; height: 64px; margin: 0; pointer-events: none; }
        div { position: absolute; display: block; left: 0px; top: 0px; width: 32px; height: 64px; pointer-events: auto; }
      </style></head><body><div id="ui"></div></body></rml>)");
      ASSERT_NE(document, nullptr);
      document->Show();
    }
  }

  void TearDown() override {
    front.reset();
    back.reset();
    RmlRenderGPU::TearDown();
  }

  // the frame's `App::step`: modules update, then whoever owns a view renders it
  auto frame(this RmlInputRouting&, std::initializer_list<RmlView*> shown, f64 millis = 16.0) -> void {
    const auto timestep = Timestep(millis);
    App::mod<RmlUI>().update(timestep);
    for (auto* view : shown) {
      view->set_viewport({0, 0}, {64, 64}, true);
      view->update({64, 64});
    }
  }

  static auto press(bool down) -> bool {
    return App::mod<RmlUI>().process_mouse_button(static_cast<u8>(MouseCode::Left), down);
  }

  static auto key(u32 code, bool down, bool repeat = false) -> bool {
    return App::mod<RmlUI>().process_key(code, 0, down, repeat);
  }

  static auto button(GamepadButtonCode code, bool down) -> bool {
    return App::mod<RmlUI>().process_gamepad_button(1, code, down);
  }

  static auto tap(GamepadButtonCode code) -> bool {
    const auto is_taken = button(code, true);
    EXPECT_EQ(button(code, false), is_taken);
    return is_taken;
  }

  static auto axis(GamepadAxisCode code, f32 tilt) -> bool {
    return App::mod<RmlUI>().process_gamepad_axis(1, code, static_cast<i16>(tilt * 32767.0f));
  }

  auto open(this RmlInputRouting& self, std::string_view head, std::string_view body) -> Rml::ElementDocument* {
    const auto rml = std::string(R"(<rml><head><style>
      body { width: 64px; height: 64px; margin: 0; }
      button { display: block; width: 32px; height: 16px; tab-index: auto; nav: auto; }
    )") + std::string(head) + "</style></head><body>" +
                     std::string(body) + "</body></rml>";
    auto* document = self.front->context()->LoadDocumentFromMemory(rml);
    EXPECT_NE(document, nullptr);
    document->Show();
    self.frame({self.front.get()});
    return document;
  }

  auto open_menu(this RmlInputRouting& self) -> Rml::ElementDocument* {
    return self.open("body { nav: auto; }", R"(<button id="a"/><button id="b"/><button id="c"/>)");
  }

  auto focus_id(this RmlInputRouting& self) -> Rml::String {
    auto* focus = self.front->context()->GetFocusElement();
    return focus ? focus->GetId() : Rml::String{};
  }

  constexpr static auto ON_UI = glm::vec2(16.0f, 32.0f);
  constexpr static auto ON_GAME = glm::vec2(48.0f, 32.0f);

  std::unique_ptr<RmlView> back = {};
  std::unique_ptr<RmlView> front = {};
};

struct ClickCounter final : Rml::EventListener {
  u32 clicks = 0;
  auto ProcessEvent(Rml::Event&) -> void override { clicks++; }
};

TEST_F(RmlInputRouting, AViewNothingShowsTakesNoInput) {
  auto& rml_ui = App::mod<RmlUI>();
  frame({back.get(), front.get()});
  rml_ui.process_mouse_move(ON_UI);
  EXPECT_TRUE(front->context()->IsMouseInteracting());
  EXPECT_FALSE(back->context()->IsMouseInteracting());

  // the tab over it is switched to
  frame({back.get()});
  frame({back.get()});
  rml_ui.process_mouse_move(ON_UI);
  EXPECT_FALSE(front->context()->IsMouseInteracting());
  EXPECT_TRUE(back->context()->IsMouseInteracting());

  EXPECT_TRUE(key(SDLK_F8, true));
  EXPECT_TRUE(rml_ui.is_debugger_visible(back->context()));
  EXPECT_FALSE(rml_ui.is_debugger_visible(front->context()));
  rml_ui.hide_debugger();

  frame({});
  frame({});
  rml_ui.process_mouse_move(ON_UI);
  EXPECT_FALSE(back->context()->IsMouseInteracting());
  EXPECT_FALSE(press(true));
  EXPECT_FALSE(key(SDLK_F8, true));
}

// an owner that updates before RmlUI does places its view one update earlier
TEST_F(RmlInputRouting, AViewRenderedBeforeTheUpdateStillTakesInput) {
  for (u32 i = 0; i < 3; i++) {
    frame({back.get(), front.get()});
    App::mod<RmlUI>().update(Timestep(16.0));
    App::mod<RmlUI>().process_mouse_move(ON_UI);
    EXPECT_TRUE(front->context()->IsMouseInteracting());
    front->set_viewport({0, 0}, {64, 64}, true);
    back->set_viewport({0, 0}, {64, 64}, true);
  }
}

TEST_F(RmlInputRouting, ACoveredViewLeavesTheMouseAlone) {
  auto& rml_ui = App::mod<RmlUI>();
  frame({front.get()});
  frame({front.get()});
  front->accepts_mouse = false;
  rml_ui.process_mouse_move(ON_UI);
  EXPECT_FALSE(front->context()->IsMouseInteracting());
  EXPECT_FALSE(press(true));
  EXPECT_FALSE(press(false));
  EXPECT_FALSE(rml_ui.process_mouse_scroll({0.0f, 1.0f}));

  // what covers it doesn't end a press the view already has
  front->accepts_mouse = true;
  rml_ui.process_mouse_move(ON_UI);
  EXPECT_TRUE(press(true));
  front->accepts_mouse = false;
  EXPECT_TRUE(press(false));
}

TEST_F(RmlInputRouting, ALockedMouseIsTheGames) {
  auto& rml_ui = App::mod<RmlUI>();
  frame({front.get()});
  frame({front.get()});
  rml_ui.process_mouse_move(ON_UI);
  ASSERT_TRUE(front->context()->IsMouseInteracting());

  rml_ui.is_mouse_locked = true;
  rml_ui.process_mouse_move(ON_UI);
  EXPECT_FALSE(front->context()->IsMouseInteracting());
  EXPECT_FALSE(press(true));
  EXPECT_FALSE(press(false));
  EXPECT_FALSE(rml_ui.process_mouse_scroll({0.0f, 1.0f}));

  rml_ui.is_mouse_locked = false;
  rml_ui.process_mouse_move(ON_UI);
  EXPECT_TRUE(front->context()->IsMouseInteracting());
}

TEST_F(RmlInputRouting, AUiPressThatLocksTheMouseStillClicks) {
  auto& rml_ui = App::mod<RmlUI>();
  frame({front.get()});
  frame({front.get()});
  auto counter = ClickCounter{};
  auto* element = front->context()->GetDocument(0)->GetElementById("ui");
  element->AddEventListener("click", &counter);

  rml_ui.process_mouse_move(ON_UI);
  EXPECT_TRUE(press(true));
  rml_ui.is_mouse_locked = true;
  EXPECT_TRUE(press(false));
  EXPECT_EQ(counter.clicks, 1u);

  EXPECT_FALSE(press(true));
  EXPECT_FALSE(press(false));
  EXPECT_EQ(counter.clicks, 1u);
  element->RemoveEventListener("click", &counter);
}

TEST_F(RmlInputRouting, AGamePressThatLocksTheMouseLetsTheViewGo) {
  auto& rml_ui = App::mod<RmlUI>();
  frame({front.get()});
  frame({front.get()});
  rml_ui.process_mouse_move(ON_GAME);
  EXPECT_FALSE(press(true));

  // drifting over the UI while it is held is still the game's
  rml_ui.is_mouse_locked = true;
  rml_ui.process_mouse_move(ON_UI);
  EXPECT_FALSE(front->context()->IsMouseInteracting());
  EXPECT_FALSE(press(false));

  rml_ui.is_mouse_locked = false;
  front->accepts_mouse = false;
  rml_ui.process_mouse_move(ON_UI);
  EXPECT_FALSE(front->context()->IsMouseInteracting()) << "the press still holds the view";
}

TEST_F(RmlInputRouting, HoldingTheDebuggerKeyTogglesItOnce) {
  auto& rml_ui = App::mod<RmlUI>();
  frame({front.get()});
  frame({front.get()});

  EXPECT_TRUE(key(SDLK_F8, true));
  front->context()->Update();
  EXPECT_TRUE(key(SDLK_F8, true, true));
  EXPECT_TRUE(rml_ui.is_debugger_visible(front->context()));
  EXPECT_TRUE(key(SDLK_F8, false));

  EXPECT_TRUE(key(SDLK_F8, true));
  EXPECT_FALSE(rml_ui.is_debugger_visible(front->context()));
  EXPECT_TRUE(key(SDLK_F8, false));
}

struct KeyStopper final : Rml::EventListener {
  Rml::Input::KeyIdentifier key = Rml::Input::KI_UNKNOWN;
  u32 hits = 0;

  auto ProcessEvent(Rml::Event& event) -> void override {
    if (event.GetParameter<int>("key_identifier", 0) == key) {
      hits++;
      event.StopPropagation();
    }
  }
};

TEST_F(RmlInputRouting, AHudLeavesTheGamepadToTheGame) {
  frame({front.get()});
  ASSERT_NE(front->context()->GetFocusElement(), nullptr);
  EXPECT_FALSE(rml_wants_gamepad(nullptr));
  EXPECT_FALSE(rml_wants_gamepad(front->context()));

  for (const auto code :
       {GamepadButtonCode::DPadUp,
        GamepadButtonCode::DPadDown,
        GamepadButtonCode::DPadLeft,
        GamepadButtonCode::DPadRight,
        GamepadButtonCode::South,
        GamepadButtonCode::East,
        GamepadButtonCode::Start}) {
    EXPECT_FALSE(tap(code));
  }

  EXPECT_FALSE(axis(GamepadAxisCode::AxisLeftY, 1.0f));
  EXPECT_FALSE(axis(GamepadAxisCode::AxisRightY, 1.0f));
}

TEST_F(RmlInputRouting, TheDPadMovesFocusAndSouthClicks) {
  auto* menu = open_menu();
  auto counter = ClickCounter{};
  menu->GetElementById("b")->AddEventListener("click", &counter);

  EXPECT_TRUE(tap(GamepadButtonCode::DPadDown));
  EXPECT_EQ(focus_id(), "a");
  EXPECT_TRUE(tap(GamepadButtonCode::DPadDown));
  EXPECT_EQ(focus_id(), "b");
  EXPECT_TRUE(menu->GetElementById("b")->IsPseudoClassSet("focus-visible"));
  EXPECT_TRUE(tap(GamepadButtonCode::South));
  EXPECT_EQ(counter.clicks, 1u);
  EXPECT_TRUE(tap(GamepadButtonCode::DPadDown));
  EXPECT_EQ(focus_id(), "c");

  // nowhere further to go, and still not the game's
  EXPECT_TRUE(tap(GamepadButtonCode::DPadDown));
  EXPECT_EQ(focus_id(), "c");
  EXPECT_TRUE(tap(GamepadButtonCode::DPadUp));
  EXPECT_EQ(focus_id(), "b");
  EXPECT_EQ(counter.clicks, 1u);

  EXPECT_FALSE(tap(GamepadButtonCode::Start));
  EXPECT_FALSE(tap(GamepadButtonCode::LeftShoulder));
  EXPECT_FALSE(axis(GamepadAxisCode::AxisRightTrigger, 1.0f));
  menu->GetElementById("b")->RemoveEventListener("click", &counter);
}

// RmlUi navigates from the nearest ancestor that can be
TEST_F(RmlInputRouting, FocusInsideAMenuStillNavigates) {
  auto* menu = open("body { nav: auto; }", R"(<p id="text">text</p><button id="a"/><button id="b"/>)");
  ASSERT_TRUE(menu->GetElementById("text")->Focus());
  EXPECT_TRUE(rml_wants_gamepad(front->context()));
  EXPECT_TRUE(tap(GamepadButtonCode::DPadDown));
  EXPECT_EQ(focus_id(), "a");
}

// `data-if` hides a panel with the focus still in it
TEST_F(RmlInputRouting, AHiddenPanelLeavesTheGamepadToWhatItWasIn) {
  constexpr static auto BODY = R"(<div id="panel"><button id="a"/></div><button id="b"/>)";
  auto* menu = open("body { nav: auto; }", BODY);
  ASSERT_TRUE(menu->GetElementById("a")->Focus(true));
  menu->GetElementById("panel")->SetProperty("display", "none");
  frame({front.get()});
  EXPECT_TRUE(rml_wants_gamepad(front->context()));
  EXPECT_TRUE(tap(GamepadButtonCode::DPadDown));
  EXPECT_EQ(focus_id(), "b");
  menu->Close();
  frame({front.get()});

  auto* hud = open("", BODY);
  ASSERT_TRUE(hud->GetElementById("a")->Focus(true));
  EXPECT_TRUE(rml_wants_gamepad(front->context()));
  hud->GetElementById("panel")->SetProperty("display", "none");
  frame({front.get()});
  EXPECT_FALSE(rml_wants_gamepad(front->context()));
  EXPECT_FALSE(tap(GamepadButtonCode::DPadDown));
}

TEST_F(RmlInputRouting, AHeldDPadRepeats) {
  open_menu();
  EXPECT_TRUE(button(GamepadButtonCode::DPadDown, true));
  EXPECT_EQ(focus_id(), "a");
  frame({front.get()}, 300.0);
  EXPECT_EQ(focus_id(), "a");
  frame({front.get()}, 150.0);
  EXPECT_EQ(focus_id(), "b");
  frame({front.get()}, 60.0);
  EXPECT_EQ(focus_id(), "b");
  frame({front.get()}, 70.0);
  EXPECT_EQ(focus_id(), "c");

  EXPECT_TRUE(button(GamepadButtonCode::DPadUp, true));
  EXPECT_EQ(focus_id(), "b");
  // letting go of what was held before leaves what is held now
  EXPECT_TRUE(button(GamepadButtonCode::DPadDown, false));
  frame({front.get()}, 450.0);
  EXPECT_EQ(focus_id(), "a");

  EXPECT_TRUE(button(GamepadButtonCode::DPadUp, false));
  ASSERT_TRUE(front->context()->GetDocument(1)->GetElementById("c")->Focus(true));
  frame({front.get()}, 1000.0);
  EXPECT_EQ(focus_id(), "c");
}

TEST_F(RmlInputRouting, TheLeftStickMovesFocus) {
  open_menu();
  EXPECT_TRUE(axis(GamepadAxisCode::AxisLeftY, 0.5f));
  EXPECT_NE(focus_id(), "a");
  EXPECT_TRUE(axis(GamepadAxisCode::AxisLeftY, 0.9f));
  EXPECT_EQ(focus_id(), "a");

  // wavering, and leaning sideways, is the same tilt
  axis(GamepadAxisCode::AxisLeftY, 0.5f);
  axis(GamepadAxisCode::AxisLeftY, 0.8f);
  axis(GamepadAxisCode::AxisLeftX, 0.9f);
  axis(GamepadAxisCode::AxisLeftX, 0.0f);
  EXPECT_EQ(focus_id(), "a");

  frame({front.get()}, 450.0);
  EXPECT_EQ(focus_id(), "b");

  EXPECT_TRUE(axis(GamepadAxisCode::AxisLeftY, 0.0f));
  frame({front.get()}, 1000.0);
  EXPECT_EQ(focus_id(), "b");
  EXPECT_TRUE(axis(GamepadAxisCode::AxisLeftY, -1.0f));
  EXPECT_EQ(focus_id(), "a");
}

TEST_F(RmlInputRouting, AStickTiltedWhenTheMenuOpensWaitsToBeLetGo) {
  frame({front.get()});
  EXPECT_FALSE(axis(GamepadAxisCode::AxisLeftY, 1.0f));
  open_menu();

  EXPECT_TRUE(axis(GamepadAxisCode::AxisLeftY, 0.95f));
  frame({front.get()}, 1000.0);
  EXPECT_EQ(focus_id(), "");

  EXPECT_TRUE(axis(GamepadAxisCode::AxisLeftY, 0.0f));
  EXPECT_TRUE(axis(GamepadAxisCode::AxisLeftY, 1.0f));
  EXPECT_EQ(focus_id(), "a");
}

TEST_F(RmlInputRouting, AReleaseGoesWhereItsPressWent) {
  frame({front.get()});
  EXPECT_FALSE(button(GamepadButtonCode::South, true));
  auto* menu = open_menu();
  EXPECT_FALSE(button(GamepadButtonCode::South, false));

  EXPECT_TRUE(button(GamepadButtonCode::South, true));
  EXPECT_TRUE(button(GamepadButtonCode::DPadDown, true));
  menu->Hide();
  frame({front.get()});
  EXPECT_FALSE(rml_wants_gamepad(front->context()));
  EXPECT_TRUE(button(GamepadButtonCode::South, false));

  // what was held when it closed moves nothing in the next one
  menu->Show();
  ASSERT_TRUE(menu->GetElementById("a")->Focus(true));
  frame({front.get()}, 1000.0);
  EXPECT_EQ(focus_id(), "a");
  EXPECT_TRUE(button(GamepadButtonCode::DPadDown, false));

  // another gamepad's press is its own
  EXPECT_TRUE(button(GamepadButtonCode::South, true));
  EXPECT_FALSE(App::mod<RmlUI>().process_gamepad_button(2, GamepadButtonCode::South, false));
  EXPECT_TRUE(button(GamepadButtonCode::South, false));
}

TEST_F(RmlInputRouting, EastIsTheGamesUnlessTheUiGoesBackOnIt) {
  auto* menu = open_menu();
  EXPECT_FALSE(tap(GamepadButtonCode::East));

  auto stopper = KeyStopper{};
  stopper.key = Rml::Input::KI_ESCAPE;
  menu->AddEventListener("keydown", &stopper);
  EXPECT_TRUE(tap(GamepadButtonCode::East));
  EXPECT_EQ(stopper.hits, 1u);
  menu->RemoveEventListener("keydown", &stopper);
}

TEST_F(RmlInputRouting, ControlsWithoutNavGoAlongTheTabOrder) {
  auto* menu = open(
    "input { display: block; width: 32px; height: 16px; } input.check { width: 16px; }",
    R"(<input id="one" class="check" type="checkbox"/><input id="two" class="check" type="checkbox"/>
       <input id="range" type="range" min="0" max="10" step="1" value="5"/>)"
  );
  ASSERT_TRUE(menu->GetElementById("one")->Focus(true));

  EXPECT_TRUE(tap(GamepadButtonCode::DPadDown));
  EXPECT_EQ(focus_id(), "two");
  EXPECT_TRUE(tap(GamepadButtonCode::South));
  EXPECT_TRUE(menu->GetElementById("two")->HasAttribute("checked"));
  EXPECT_TRUE(tap(GamepadButtonCode::DPadUp));
  EXPECT_EQ(focus_id(), "one");
  EXPECT_TRUE(tap(GamepadButtonCode::DPadDown));
  EXPECT_TRUE(tap(GamepadButtonCode::DPadDown));
  EXPECT_EQ(focus_id(), "range");

  // a slider takes its own directions
  EXPECT_TRUE(tap(GamepadButtonCode::DPadRight));
  EXPECT_EQ(focus_id(), "range");
  EXPECT_EQ(menu->GetElementById("range")->GetAttribute<Rml::String>("value", ""), "6");
  EXPECT_TRUE(tap(GamepadButtonCode::DPadUp));
  EXPECT_EQ(focus_id(), "two");
}

TEST_F(RmlInputRouting, TheRightStickScrollsAroundFocus) {
  auto* menu = open(
    "div { display: block; } #list { width: 64px; height: 32px; overflow-y: scroll; } #tall { height: 400px; }",
    R"(<div id="list"><button id="a"/><div id="tall"/></div>)"
  );
  auto* list = menu->GetElementById("list");
  ASSERT_TRUE(menu->GetElementById("a")->Focus(true));

  EXPECT_TRUE(axis(GamepadAxisCode::AxisRightY, 0.1f));
  frame({front.get()}, 100.0);
  EXPECT_EQ(list->GetScrollTop(), 0.0f);

  EXPECT_TRUE(axis(GamepadAxisCode::AxisRightY, 1.0f));
  frame({front.get()}, 100.0);
  const auto scrolled = list->GetScrollTop();
  EXPECT_GT(scrolled, 50.0f);

  // a fraction of a pixel a frame still adds up
  EXPECT_TRUE(axis(GamepadAxisCode::AxisRightY, -0.22f));
  for (u32 i = 0; i < 200; i++) {
    frame({front.get()}, 1.0);
  }

  EXPECT_LT(list->GetScrollTop(), scrolled);
  EXPECT_GT(list->GetScrollTop(), scrolled - 20.0f);

  EXPECT_TRUE(axis(GamepadAxisCode::AxisRightY, 0.0f));
  const auto rested = list->GetScrollTop();
  frame({front.get()}, 100.0);
  EXPECT_EQ(list->GetScrollTop(), rested);
}

TEST_F(RmlInputRouting, AViewNothingShowsTakesNoGamepad) {
  open_menu();
  EXPECT_TRUE(button(GamepadButtonCode::DPadDown, true));
  EXPECT_EQ(focus_id(), "a");
  frame({});
  frame({}, 1000.0);
  frame({}, 1000.0);
  EXPECT_EQ(focus_id(), "a");
  EXPECT_TRUE(button(GamepadButtonCode::DPadDown, false));
  EXPECT_FALSE(tap(GamepadButtonCode::DPadDown));
  EXPECT_FALSE(axis(GamepadAxisCode::AxisLeftY, 1.0f));
  EXPECT_EQ(focus_id(), "a");
}
} // namespace ox
