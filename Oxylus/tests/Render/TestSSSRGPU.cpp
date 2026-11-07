#include <SDL3/SDL_filesystem.h>
#include <VkBootstrap.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <glm/gtc/packing.hpp>
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
#include "Render/BlueNoiseTables.hpp"
#include "Render/GPU/Shared.hpp"
#include "Render/RenderContext.hpp"
#include "Render/RenderResources.hpp"
#include "Render/RendererInstance.hpp"
#include "Render/Utils/PushConstants.hpp"
#include "Render/Utils/Samplers.hpp"

namespace ox {
constexpr static auto TEST_SIZE = 256_u32;
constexpr static auto SENTINEL_AMBIENT = glm::vec3(0.37f, 0.52f, 0.83f);

static VKAPI_ATTR auto VKAPI_CALL sssr_validation_message(
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

static auto retain_sssr_driver_modules() -> void {
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

static auto sssr_gpu_pack() -> option<AssetFile> {
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

struct SSSRTestDirectory {
  std::filesystem::path previous = std::filesystem::current_path();
  std::filesystem::path temporary = std::filesystem::temp_directory_path() /
                                    ("oxylus-sssr-gpu-" +
                                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

  SSSRTestDirectory() {
    std::filesystem::create_directories(temporary);
    std::filesystem::current_path(temporary);
  }

  ~SSSRTestDirectory() {
    std::filesystem::current_path(previous);
    std::filesystem::remove_all(temporary);
  }
};

struct SSSRDevice {
  std::atomic<u32> validation_errors{0};
  vkb::Instance instance = {};
  vkb::Device device = {};
  std::string device_name = {};
  std::unique_ptr<SSSRTestDirectory> directory = {};
  std::unique_ptr<RenderContext> render = {};

  ~SSSRDevice() {
    render.reset();
    retain_sssr_driver_modules();
    if (device.device != VK_NULL_HANDLE) {
      vkb::destroy_device(device);
    }

    if (instance.instance != VK_NULL_HANDLE) {
      vkb::destroy_instance(instance);
    }

    directory.reset();
  }
};

static auto create_sssr_device(std::string& skip_reason, std::string& failure_reason) -> std::unique_ptr<SSSRDevice> {
  auto result = std::make_unique<SSSRDevice>();
  auto builder = vkb::InstanceBuilder{};
  builder.set_headless().require_api_version(1, 3, 0).enable_extension(VK_KHR_SURFACE_EXTENSION_NAME);
  if (std::getenv("OX_SSSR_GPU_VALIDATION")) {
    builder.request_validation_layers()
      .set_debug_callback(sssr_validation_message)
      .set_debug_callback_user_data_pointer(&result->validation_errors)
      .add_validation_feature_enable(VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT);
  }

  const auto instance = builder.build();
  if (!instance) {
    skip_reason = "headless Vulkan instance unavailable: " + instance.error().message();
    return nullptr;
  }

  result->instance = *instance;

  auto features = VkPhysicalDeviceFeatures{};
  features.shaderInt64 = VK_TRUE;
  features.samplerAnisotropy = VK_TRUE;
  features.shaderStorageImageReadWithoutFormat = VK_TRUE;
  features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
  features.shaderStorageImageExtendedFormats = VK_TRUE;
  auto features11 = VkPhysicalDeviceVulkan11Features{};
  features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
  features11.shaderDrawParameters = VK_TRUE;
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
                          .set_required_features_11(features11)
                          .set_required_features_12(features12)
                          .set_required_features_13(features13)
                          .select();
  if (!physical) {
    skip_reason = "no Vulkan device supports the SSSR shaders: " + physical.error().message();
    return nullptr;
  }

  result->device_name = physical->properties.deviceName;

  const auto device = vkb::DeviceBuilder(*physical).build();
  if (!device) {
    skip_reason = "device creation failed: " + device.error().message();
    return nullptr;
  }

  result->device = *device;

  auto context = vir::Context::create(
    {.instance = instance->instance,
     .physical_device = physical->physical_device,
     .device = device->device,
     .get_instance_proc_addr = instance->fp_vkGetInstanceProcAddr}
  );
  if (!context) {
    skip_reason = "vir context creation failed";
    return nullptr;
  }

  const auto queue = device->get_queue_index(vkb::QueueType::graphics);
  if (!queue || !(*context)->create_command_queue(*queue, vir::DomainFlag::Graphics)) {
    skip_reason = "no graphics queue";
    return nullptr;
  }

  result->directory = std::make_unique<SSSRTestDirectory>();
  result->render = std::make_unique<RenderContext>();
  auto& render = *result->render;
  if (!render.initialize_graph(std::move(*context))) {
    skip_reason = "render graph initialization failed";
    return nullptr;
  }

  const auto sampler_binding = std::array{VkDescriptorSetLayoutBinding{
    .binding = 0,
    .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
    .descriptorCount = 4,
    .stageFlags = VK_SHADER_STAGE_ALL,
    .pImmutableSamplers = nullptr
  }};
  auto sampler_set = render.create_persistent_descriptor_set(
    sampler_binding,
    std::array<VkDescriptorBindingFlags, 1>{0}
  );
  if (!sampler_set) {
    skip_reason = "descriptor set creation failed";
    return nullptr;
  }

  render.resources->descriptor_set = std::move(*sampler_set);
  for (const auto& sampler_info : {NearestSamplerClamped, LinearSamplerClamped}) {
    const auto sampler = render.allocate_sampler(sampler_info, false);
    if (!sampler) {
      skip_reason = "sampler creation failed";
      return nullptr;
    }

    render.resources->cached_samplers.emplace_back(sampler_info, *sampler);
  }

  const auto pack = sssr_gpu_pack();
  if (!pack) {
    failure_reason = "engine.oxpack is missing, build with `xmake b -a` before running this test";
    return nullptr;
  }

  for (const auto& entry : pack->entries) {
    const auto* shader = std::get_if<ShaderPipelineData>(&entry.data);
    if (shader && (shader->module_name == "hiz" || shader->module_name.starts_with("sssr_"))) {
      if (!render.create_pipeline(*shader)) {
        failure_reason = "pipeline creation failed for " + std::string(shader->module_name);
        return nullptr;
      }
    }
  }

  return result;
}

// --- analytic scene ---

struct SceneHit {
  f32 t = std::numeric_limits<f32>::max();
  glm::vec3 position = {};
  glm::vec3 normal = {};
  f32 roughness = 0.0f;
  i32 object = -1;
};

struct ScenePlane {
  glm::vec3 normal = {};
  f32 offset = 0.0f;
  f32 roughness = 0.0f;
};

struct SceneBox {
  glm::vec3 min = {};
  glm::vec3 max = {};
  f32 roughness = 0.0f;
};

struct TestScene {
  std::vector<ScenePlane> planes = {};
  std::vector<SceneBox> boxes = {};

  auto trace(this const TestScene& self, glm::vec3 origin, glm::vec3 direction) -> SceneHit {
    auto best = SceneHit{};
    auto object = 0;
    for (const auto& plane : self.planes) {
      const auto denominator = glm::dot(plane.normal, direction);
      if (std::abs(denominator) > 1e-8f) {
        const auto t = (plane.offset - glm::dot(plane.normal, origin)) / denominator;
        if (t > 0.0f && t < best.t) {
          best =
            {t, origin + direction * t, denominator < 0.0f ? plane.normal : -plane.normal, plane.roughness, object};
        }
      }

      object++;
    }

    for (const auto& box : self.boxes) {
      auto t_near = -std::numeric_limits<f32>::max();
      auto t_far = std::numeric_limits<f32>::max();
      auto normal = glm::vec3(0.0f);
      for (auto axis = 0; axis < 3; axis++) {
        if (std::abs(direction[axis]) < 1e-8f) {
          if (origin[axis] < box.min[axis] || origin[axis] > box.max[axis]) {
            t_near = std::numeric_limits<f32>::max();
          }

          continue;
        }

        auto t0 = (box.min[axis] - origin[axis]) / direction[axis];
        auto t1 = (box.max[axis] - origin[axis]) / direction[axis];
        auto face = -1.0f;
        if (t0 > t1) {
          std::swap(t0, t1);
          face = 1.0f;
        }

        if (t0 > t_near) {
          t_near = t0;
          normal = glm::vec3(0.0f);
          normal[axis] = face;
        }

        t_far = std::min(t_far, t1);
      }

      if (t_near < t_far && t_near > 0.0f && t_near < best.t) {
        best = {t_near, origin + direction * t_near, normal, box.roughness, object};
      }

      object++;
    }

    return best;
  }
};

struct TestCamera {
  GPU::Camera gpu = {};
  glm::vec3 position = {};
  glm::vec2 jitter = {};
};

static auto make_test_camera(glm::vec3 position, glm::vec3 target, glm::vec2 jitter) -> TestCamera {
  auto camera = TestCamera{};
  camera.position = position;
  camera.jitter = jitter;
  const auto view = glm::lookAt(position, target, glm::vec3(0.0f, 1.0f, 0.0f));
  auto projection = glm::perspective(glm::radians(60.0f), 1.0f, 1000.0f, 0.1f);
  projection[1][1] *= -1.0f;
  camera.gpu.position = glm::vec4(position, 1.0f);
  camera.gpu.projection = projection;
  camera.gpu.inv_projection = glm::inverse(projection);
  camera.gpu.view = view;
  camera.gpu.inv_view = glm::inverse(view);
  camera.gpu.projection_view = projection * view;
  camera.gpu.inv_projection_view = glm::inverse(camera.gpu.projection_view);
  camera.gpu.temporalaa_jitter = jitter;
  camera.gpu.resolution = glm::vec2(static_cast<f32>(TEST_SIZE));
  return camera;
}

// raster pixel `pixel` really sees the scene at `pixel - jitter`
static auto camera_ray(const TestCamera& camera, glm::vec2 pixel) -> glm::vec3 {
  const auto uv = (pixel - camera.jitter) / static_cast<f32>(TEST_SIZE);
  const auto ndc = uv * 2.0f - 1.0f;
  auto world = camera.gpu.inv_projection_view * glm::vec4(ndc, 1.0f, 1.0f);
  world /= world.w;
  return glm::normalize(glm::vec3(world) - camera.position);
}

// the pixel the rasterizer puts the point in, which is shifted by the jitter
static auto project_to_pixel(const TestCamera& camera, glm::vec3 position) -> glm::vec3 {
  const auto clip = camera.gpu.projection_view * glm::vec4(position, 1.0f);
  const auto ndc = glm::vec3(clip) / clip.w;
  return {(glm::vec2(ndc) * 0.5f + 0.5f) * static_cast<f32>(TEST_SIZE) + camera.jitter, ndc.z};
}

static auto encode_oct(glm::vec3 v) -> glm::vec2 {
  const auto p = glm::vec2(v) * (1.0f / (std::abs(v.x) + std::abs(v.y) + std::abs(v.z)));
  const auto sign = glm::vec2(p.x >= 0.0f ? 1.0f : -1.0f, p.y >= 0.0f ? 1.0f : -1.0f);
  return v.z <= 0.0f ? (1.0f - glm::abs(glm::vec2(p.y, p.x))) * sign : p;
}

// --- gpu case ---

using RoughnessOverride = std::function<f32(u32 x, u32 y, f32 scene_roughness)>;
using LitFunction = std::function<glm::vec3(const SceneHit& hit)>;

enum class TestEnvironment { Ambient, Sky, DDGI, DDGISky };

constexpr static auto SKY_VIEW_COLOR = glm::vec3(0.2f, 0.4f, 0.9f);
constexpr static auto SKY_CUBEMAP_COLOR = glm::vec3(0.1f, 0.7f, 0.3f);
constexpr static auto DDGI_AMBIENT_COLOR = glm::vec3(0.9f, 0.1f, 0.1f);

struct CaseConfig {
  const TestScene* scene = nullptr;
  TestCamera camera = {};
  const TestCamera* previous_camera = nullptr;
  u32 samples_per_quad = 4;
  f32 depth_thickness = 0.015f;
  RoughnessOverride roughness = {};
  TestEnvironment environment = TestEnvironment::Ambient;
  LitFunction lit = {};
  f32 radiance_scale = 1.0f;
  f32 fixed_exposure = 1.0f;
  bool is_eye_adapted = false;
  f32 buffer_exposure = 1.0f;
  bool is_reset = false;
  bool is_temporal_variance_guided = false;
};

struct CaseResult {
  bool is_valid = false;
  std::vector<glm::vec4> radiance = {};
  std::vector<glm::vec4> denoised = {};
  std::vector<f32> confidence = {};
  std::vector<f32> roughness = {};
  std::vector<f32> sample_count = {};
  std::vector<f32> variance = {};
  std::array<u32, 4> counters = {};
  std::vector<SceneHit> primary = {};
  std::vector<f32> surface_roughness = {};
};

class CaseAllocations {
public:
  explicit CaseAllocations(RenderContext& render_context) : render(render_context) {}
  CaseAllocations(const CaseAllocations&) = delete;
  auto operator=(const CaseAllocations&) -> CaseAllocations& = delete;

  ~CaseAllocations() {
    for (const auto view : views) {
      render.persistent->deallocate_image_view(view);
    }

    for (const auto& image : images) {
      render.persistent->deallocate_image(image);
    }

    for (const auto& buffer : buffers) {
      render.persistent->deallocate_buffer(buffer);
    }
  }

  auto image(
    vir::Format format, vir::Extent3D extent, vir::ImageUsageFlags usage, u32 mip_levels = 1, bool is_cube = false
  ) -> option<vir::ImageAttachment> {
    const auto view_type = is_cube ? vir::ImageViewType::Cube : vir::ImageViewType::e2D;
    const auto allocated = render.persistent->allocate_image({
      .extent = extent,
      .format = format,
      .usage = usage,
      .view_type = view_type,
      .flags = is_cube ? vir::ImageCreateFlags::CubeCompatible : vir::ImageCreateFlags::None,
      .mip_levels = mip_levels,
      .array_layers = is_cube ? 6_u32 : 1_u32,
    });
    if (!allocated) {
      return nullopt;
    }

    images.push_back(*allocated);
    const auto range = allocated->subresource_range();
    const auto view = render.persistent->allocate_image_view(allocated->handle, format, view_type, range);
    if (!view) {
      return nullopt;
    }

    views.push_back(*view);
    auto attachment = vir::ImageAttachment::from_image(*allocated, VK_IMAGE_LAYOUT_UNDEFINED);
    attachment.image_view = *view;
    attachment.view_type = view_type;
    attachment.subresource_range = range;
    return attachment;
  }

  auto buffer(u64 size, VkBufferUsageFlags usage, vir::MemoryLocation location) -> option<vir::Buffer> {
    const auto allocated = render.persistent->allocate_buffer({.size = size, .usage = usage, .location = location});
    if (!allocated) {
      return nullopt;
    }

    buffers.push_back(*allocated);
    return *allocated;
  }

private:
  RenderContext& render;
  std::vector<vir::Image> images = {};
  std::vector<VkImageView> views = {};
  std::vector<vir::Buffer> buffers = {};
};

static auto upload_image(
  CaseAllocations& allocations,
  vir::Module& module,
  vir::Format format,
  vir::ImageUsageFlags usage,
  std::span<const u8> bytes,
  vir::Extent3D extent = {TEST_SIZE, TEST_SIZE, 1}
) -> option<vir::ValueID> {
  const auto upload = allocations.buffer(bytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, vir::MemoryLocation::CpuToGpu);
  const auto image = allocations.image(format, extent, usage | vir::ImageUsageFlags::TransferDst);
  if (!upload || !image || !upload->write_bytes(0, bytes)) {
    return nullopt;
  }

  return module.copy_buffer_to_image(
    module.import_buffer(*upload, vir::Access::HostWrite),
    module.import_image(image->image, VK_IMAGE_LAYOUT_UNDEFINED, vir::Access::None)
  );
}

static auto unjittered_pixel(const TestCamera& camera, glm::vec3 position) -> glm::vec2 {
  const auto clip = camera.gpu.projection_view * glm::vec4(position, 1.0f);
  return (glm::vec2(clip) / clip.w * 0.5f + 0.5f) * static_cast<f32>(TEST_SIZE);
}

static auto default_lit(const SceneHit& hit) -> glm::vec3 { return hit.position * 0.05f + 0.5f; }

// the persistent images the renderer ping pongs, swapped here exactly as bind_sssr_variables and the flip after a
// successful submit do
class SSSRSession {
public:
  explicit SSSRSession(SSSRDevice& device) : gpu(device), allocations(*device.render) {}

  auto initialize(this SSSRSession& self) -> bool {
    constexpr auto usage = vir::ImageUsageFlags::Storage | vir::ImageUsageFlags::Sampled |
                           vir::ImageUsageFlags::TransferDst | vir::ImageUsageFlags::TransferSrc;
    const auto render_extent = vir::Extent3D{TEST_SIZE, TEST_SIZE, 1};
    const auto tile_extent = vir::Extent3D{TEST_SIZE / 8, TEST_SIZE / 8, 1};
    const auto formats = std::array{
      vir::Format::R32Sfloat,
      vir::Format::RGBA16Sfloat,
      vir::Format::R16Sfloat,
      vir::Format::R16Sfloat,
      vir::Format::R8Unorm,
      vir::Format::RGBA16Sfloat,
    };
    auto module = vir::Module{};
    const auto black = vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 0.0f);
    const auto clear = [&](const option<vir::ImageAttachment>& image) -> bool {
      if (!image) {
        return false;
      }

      const auto cleared = module.clear(module.import_attachment(*image, vir::Access::None), black);
      module.export_to(cleared, SSSRContext::HISTORY_REST);
      return true;
    };
    for (auto kind = 0_sz; kind < formats.size(); kind++) {
      for (auto index = 0_sz; index < 2; index++) {
        const auto image = self.allocations.image(
          formats[kind],
          render_extent,
          usage,
          kind == 0 ? SSSRContext::PYRAMID_MIP_COUNT : 1_u32
        );
        if (!clear(image)) {
          return false;
        }

        self.pairs[kind][index] = *image;
      }
    }

    const auto average = self.allocations.image(vir::Format::RGBA16Sfloat, tile_extent, usage);
    if (!clear(average)) {
      return false;
    }

    self.average = *average;
    return static_cast<bool>(self.gpu.render->execute_blocking(std::move(module)));
  }

  auto step(this SSSRSession& self, const CaseConfig& config) -> CaseResult;

  u32 frame_index = 0;

private:
  SSSRDevice& gpu;
  CaseAllocations allocations;
  std::array<std::array<vir::ImageAttachment, 2>, 6> pairs = {};
  vir::ImageAttachment average = {};
  bool is_ping = false;
};

auto SSSRSession::step(this SSSRSession& self, const CaseConfig& config) -> CaseResult {
  auto result = CaseResult{};
  auto& render = *self.gpu.render;
  const auto& scene = *config.scene;
  const auto pixel_count = static_cast<usize>(TEST_SIZE) * TEST_SIZE;
  const auto& previous_camera = config.previous_camera ? *config.previous_camera : config.camera;

  auto depth = std::vector<f32>(pixel_count, 0.0f);
  auto normals = std::vector<glm::vec4>(pixel_count, glm::vec4(0.0f));
  auto albedo = std::vector<glm::vec4>(pixel_count, glm::vec4(0.0f));
  auto lit = std::vector<glm::vec4>(pixel_count, glm::vec4(0.0f));
  auto material = std::vector<u8>(pixel_count * 2, 0);
  auto occlusion = std::vector<u8>(pixel_count, 255);
  auto velocity = std::vector<u32>(pixel_count, 0);
  result.primary.resize(pixel_count);
  result.surface_roughness.assign(pixel_count, 0.0f);
  for (auto y = 0_u32; y < TEST_SIZE; y++) {
    for (auto x = 0_u32; x < TEST_SIZE; x++) {
      const auto index = static_cast<usize>(y) * TEST_SIZE + x;
      const auto direction = camera_ray(config.camera, glm::vec2(x + 0.5f, y + 0.5f));
      auto hit = scene.trace(config.camera.position, direction);
      if (hit.object >= 0) {
        // past the far plane the rasterizer clips the surface away, which leaves sky
        const auto clip = config.camera.gpu.projection_view * glm::vec4(hit.position, 1.0f);
        depth[index] = clip.z / clip.w;
        if (depth[index] <= 0.0f) {
          depth[index] = 0.0f;
          hit = {};
        }
      }

      result.primary[index] = hit;
      if (hit.object < 0) {
        continue;
      }

      const auto oct = encode_oct(hit.normal);
      normals[index] = glm::vec4(oct, oct);
      albedo[index] = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);
      lit[index] = glm::vec4((config.lit ? config.lit(hit) : default_lit(hit)) * config.radiance_scale, 1.0f);
      velocity[index] = glm::packHalf2x16(
        unjittered_pixel(previous_camera, hit.position) - unjittered_pixel(config.camera, hit.position)
      );
      auto roughness = hit.roughness;
      if (config.roughness) {
        roughness = config.roughness(x, y, roughness);
      }

      result.surface_roughness[index] = roughness;
      material[index * 2 + 1] = static_cast<u8>(std::lround(std::clamp(roughness, 0.0f, 1.0f) * 255.0f));
    }
  }

  CaseAllocations allocations(render);
  auto module = vir::Module{};
  auto prepared = PreparedFrame{};
  const auto is_sky = config.environment == TestEnvironment::Sky || config.environment == TestEnvironment::DDGISky;
  const auto is_ddgi = config.environment == TestEnvironment::DDGI || config.environment == TestEnvironment::DDGISky;
  const auto shape = FrameShape{
    .render_size = {TEST_SIZE, TEST_SIZE},
    .scene_flags = is_sky ? GPU::SceneFlags::HasAtmosphere : GPU::SceneFlags::None,
    .has_ddgi = is_ddgi,
    .has_sssr = true,
  };
  auto recording = FrameRecording{.shape = shape, .prepared = prepared};

  auto camera_data = config.camera.gpu;
  camera_data.previous_projection_view = previous_camera.gpu.projection_view;
  camera_data.temporalaa_jitter_prev = previous_camera.jitter;
  const auto camera_buffer = allocations.buffer(
    sizeof(GPU::Camera),
    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
    vir::MemoryLocation::CpuToGpu
  );
  if (!camera_buffer || !camera_buffer->write<GPU::Camera>(0, std::span(&camera_data, 1))) {
    ADD_FAILURE() << "camera buffer";
    return result;
  }

  recording.values.camera = module.import_buffer(*camera_buffer, vir::Access::HostWrite);

  const auto exposure_data = GPU::HistogramLuminance{.adapted_luminance = 1.0f, .exposure = config.buffer_exposure};
  const auto exposure_buffer = allocations.buffer(
    sizeof(GPU::HistogramLuminance),
    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
    vir::MemoryLocation::CpuToGpu
  );
  if (!exposure_buffer || !exposure_buffer->write<GPU::HistogramLuminance>(0, std::span(&exposure_data, 1))) {
    ADD_FAILURE() << "exposure buffer";
    return result;
  }

  recording.values.exposure = module.import_buffer(*exposure_buffer, vir::Access::HostWrite);

  constexpr auto sampled = vir::ImageUsageFlags::Sampled;
  const auto as_bytes = [](const auto& values) -> std::span<const u8> {
    return std::span(reinterpret_cast<const u8*>(values.data()), values.size() * sizeof(values[0]));
  };

  auto context = SSSRContext{};
  const auto depth_value = upload_image(allocations, module, vir::Format::D32Sfloat, sampled, as_bytes(depth));
  const auto normal_value = upload_image(allocations, module, vir::Format::RGBA32Sfloat, sampled, as_bytes(normals));
  const auto albedo_value = upload_image(allocations, module, vir::Format::RGBA32Sfloat, sampled, as_bytes(albedo));
  const auto material_value = upload_image(allocations, module, vir::Format::RG8Unorm, sampled, as_bytes(material));
  const auto occlusion_value = upload_image(allocations, module, vir::Format::R8Unorm, sampled, as_bytes(occlusion));
  const auto velocity_value = upload_image(allocations, module, vir::Format::RG16Sfloat, sampled, as_bytes(velocity));
  const auto lit_value = upload_image(
    allocations,
    module,
    vir::Format::RGBA32Sfloat,
    sampled | vir::ImageUsageFlags::ColorAttachment,
    as_bytes(lit)
  );
  if (
    !depth_value || !normal_value || !albedo_value || !material_value || !occlusion_value || !velocity_value ||
    !lit_value
  ) {
    ADD_FAILURE() << "input images";
    return result;
  }

  context.depth_attachment = *depth_value;
  context.normal_attachment = *normal_value;
  context.albedo_attachment = *albedo_value;
  context.metallic_roughness_occlusion_attachment = *material_value;
  context.ambient_occlusion_attachment = *occlusion_value;
  context.velocity_attachment = *velocity_value;
  context.final_attachment = *lit_value;

  if (is_sky) {
    auto atmosphere = GPU::Atmosphere{};
    atmosphere.sky_view_lut_size = {192, 108, 1};
    const auto atmosphere_buffer = allocations.buffer(
      sizeof(atmosphere),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      vir::MemoryLocation::CpuToGpu
    );
    if (!atmosphere_buffer || !atmosphere_buffer->write<GPU::Atmosphere>(0, std::span(&atmosphere, 1))) {
      ADD_FAILURE() << "atmosphere buffer";
      return result;
    }

    recording.values.atmosphere = module.import_buffer(*atmosphere_buffer, vir::Access::HostWrite);

    const auto lut = std::vector<glm::vec4>(192 * 108, glm::vec4(SKY_VIEW_COLOR * config.radiance_scale, 1.0f));
    const auto
      lut_value = upload_image(allocations, module, vir::Format::RGBA32Sfloat, sampled, as_bytes(lut), {192, 108, 1});
    const auto cubemap = allocations.image(
      vir::Format::RGBA16Sfloat,
      {4, 4, 1},
      sampled | vir::ImageUsageFlags::TransferDst,
      1,
      true
    );
    if (!lut_value || !cubemap) {
      ADD_FAILURE() << "sky images";
      return result;
    }

    context.sky_view_lut_attachment = *lut_value;
    const auto cube_color = SKY_CUBEMAP_COLOR * config.radiance_scale;
    context.sky_cubemap_attachment = module.clear(
      module.import_attachment(*cubemap, vir::Access::None),
      vir::ClearValue::rgba_f32(cube_color.r, cube_color.g, cube_color.b, 1.0f)
    );
  }

  if (is_ddgi) {
    const auto volumes = allocations.buffer(
      sizeof(GPU::ProbeVolume),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      vir::MemoryLocation::CpuToGpu
    );
    const auto states = allocations.buffer(
      sizeof(GPU::ProbeState),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      vir::MemoryLocation::CpuToGpu
    );
    const auto irradiance_texels = std::vector<u8>(4 * 4 * 8, 0);
    const auto distance_texels = std::vector<u8>(4 * 4 * 4, 0);
    const auto
      irradiance = upload_image(allocations, module, vir::Format::RGBA16Sfloat, sampled, irradiance_texels, {4, 4, 1});
    const auto
      distance = upload_image(allocations, module, vir::Format::RG16Sfloat, sampled, distance_texels, {4, 4, 1});
    if (!volumes || !states || !irradiance || !distance) {
      ADD_FAILURE() << "ddgi resources";
      return result;
    }

    context.probe_volumes_buffer = module.import_buffer(*volumes, vir::Access::HostWrite);
    context.probe_states_buffer = module.import_buffer(*states, vir::Access::HostWrite);
    context.irradiance_attachment = *irradiance;
    context.distance_attachment = *distance;
  }

  const auto sobol = blue_noise_sobol();
  const auto scrambling = blue_noise_scrambling();
  const auto tables = allocations.buffer(
    sobol.size_bytes() + scrambling.size_bytes(),
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
    vir::MemoryLocation::CpuToGpu
  );
  if (
    !tables || !tables->write_bytes(0, sobol) || !tables->write_bytes(sobol.size_bytes(), scrambling) ||
    !render.persistent->flush_buffer(*tables)
  ) {
    ADD_FAILURE() << "blue noise tables";
    return result;
  }

  RendererInstance::record_sssr(module, render, recording, context);

  constexpr auto pixels = static_cast<u64>(TEST_SIZE) * TEST_SIZE;
  constexpr auto counter_offset = 0_u64;
  constexpr auto radiance_offset = 256_u64;
  constexpr auto denoised_offset = radiance_offset + pixels * 8;
  constexpr auto confidence_offset = denoised_offset + pixels * 8;
  constexpr auto roughness_offset = confidence_offset + pixels;
  constexpr auto sample_count_offset = roughness_offset + pixels;
  constexpr auto variance_offset = sample_count_offset + pixels * 2;
  constexpr auto readback_size = variance_offset + pixels * 2;
  const auto readback = allocations
                          .buffer(readback_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, vir::MemoryLocation::GpuToCpu);
  if (!readback) {
    ADD_FAILURE() << "readback buffer";
    return result;
  }

  const auto extent = vir::Extent3D{TEST_SIZE, TEST_SIZE, 1};
  auto downloaded = module.import_buffer(*readback, vir::Access::None);
  downloaded = module.copy_buffer(
    context.ray_counter_buffer,
    downloaded,
    {.src_offset = 0, .dst_offset = counter_offset, .size = sizeof(result.counters)}
  );
  const auto copy_image = [&](vir::ValueID image, u64 offset) -> void {
    downloaded = module
                   .copy_image_to_buffer_region(image, downloaded, {.buffer_offset = offset, .image_extent = extent});
  };
  copy_image(context.intersection_radiance, radiance_offset);
  copy_image(context.radiance, denoised_offset);
  copy_image(context.confidence, confidence_offset);
  copy_image(context.roughness, roughness_offset);
  copy_image(context.sample_count, sample_count_offset);
  copy_image(context.variance, variance_offset);
  module.export_to(downloaded, vir::Access::HostRead);
  module.export_to(context.final_attachment, vir::Access::FragmentSampled);
  for (const auto value :
       {context.depth_hierarchy,
        context.previous_depth_hierarchy,
        context.radiance,
        context.previous_radiance,
        context.variance,
        context.previous_variance,
        context.sample_count,
        context.previous_sample_count,
        context.roughness,
        context.previous_roughness,
        context.normal,
        context.previous_normal,
        context.average_radiance}) {
    module.export_to(value, SSSRContext::HISTORY_REST);
  }

  module.export_to(context.blue_noise_tables, vir::Access::MemoryRead);

  auto program = render.compile(std::move(module));
  if (!program) {
    ADD_FAILURE() << "compile failed";
    return result;
  }

  const auto& variables = prepared.sssr;
  program->set(variables.blue_noise_tables, *tables);
  program->set_bytes(variables.blue_noise_push, PushConstants<u32>(self.frame_index));
  program->set(variables.is_reset, config.is_reset);
  program->set_bytes(
    variables.settings,
    GPU::SSSRSettings{
      .render_size = {TEST_SIZE, TEST_SIZE},
      .roughness_threshold = 0.2f,
      .depth_thickness = config.depth_thickness,
      .max_traversal_intersections = 128,
      .min_traversal_occupancy = 4,
      .most_detailed_mip = 0,
      .samples_per_quad = config.samples_per_quad,
      .is_temporal_variance_guided = config.is_temporal_variance_guided ? 1_u32 : 0_u32,
      .variance_threshold = 0.0f,
      .temporal_stability = 0.7f,
      .frame_index = self.frame_index,
      .ddgi_volume_count = 0,
      .ddgi_normal_bias = 0.0f,
      .ddgi_view_bias = 0.0f,
      .ddgi_intensity = 1.0f,
      .horizon_darkening = 1.0f,
      .sky_ambient_color = DDGI_AMBIENT_COLOR * config.radiance_scale,
      .base_ambient_color = SENTINEL_AMBIENT * config.radiance_scale,
      .sun_direction = glm::vec3(0.0f, 1.0f, 0.0f),
      .is_eye_adapted = config.is_eye_adapted ? 1_u32 : 0_u32,
      .fixed_exposure = config.fixed_exposure,
    }
  );
  const auto current = self.is_ping ? 1_sz : 0_sz;
  const auto previous = 1_sz - current;
  program->set(variables.depth_hierarchy, self.pairs[0][current]);
  program->set(variables.previous_depth_hierarchy, self.pairs[0][previous]);
  program->set(variables.radiance, self.pairs[1][current]);
  program->set(variables.previous_radiance, self.pairs[1][previous]);
  program->set(variables.variance, self.pairs[2][current]);
  program->set(variables.previous_variance, self.pairs[2][previous]);
  program->set(variables.sample_count, self.pairs[3][current]);
  program->set(variables.previous_sample_count, self.pairs[3][previous]);
  program->set(variables.roughness, self.pairs[4][current]);
  program->set(variables.previous_roughness, self.pairs[4][previous]);
  program->set(variables.normal, self.pairs[5][current]);
  program->set(variables.previous_normal, self.pairs[5][previous]);
  program->set(variables.average_radiance, self.average);

  if (!render.execute_blocking(*program)) {
    ADD_FAILURE() << "execution failed";
    return result;
  }

  if (!render.persistent->invalidate_buffer(*readback)) {
    ADD_FAILURE() << "readback invalidate";
    return result;
  }

  self.is_ping = !self.is_ping;
  self.frame_index++;

  std::memcpy(result.counters.data(), readback->mapped + counter_offset, sizeof(result.counters));
  const auto read_rgba = [&](u64 offset, std::vector<glm::vec4>& out) -> void {
    out.resize(pixel_count);
    const auto* halves = reinterpret_cast<const u16*>(readback->mapped + offset);
    for (auto index = 0_sz; index < pixel_count; index++) {
      out[index] = glm::vec4(
        glm::unpackHalf1x16(halves[index * 4 + 0]),
        glm::unpackHalf1x16(halves[index * 4 + 1]),
        glm::unpackHalf1x16(halves[index * 4 + 2]),
        glm::unpackHalf1x16(halves[index * 4 + 3])
      );
    }
  };
  const auto read_half = [&](u64 offset, std::vector<f32>& out) -> void {
    out.resize(pixel_count);
    const auto* halves = reinterpret_cast<const u16*>(readback->mapped + offset);
    for (auto index = 0_sz; index < pixel_count; index++) {
      out[index] = glm::unpackHalf1x16(halves[index]);
    }
  };
  read_rgba(radiance_offset, result.radiance);
  read_rgba(denoised_offset, result.denoised);
  read_half(sample_count_offset, result.sample_count);
  read_half(variance_offset, result.variance);

  result.confidence.resize(pixel_count);
  result.roughness.resize(pixel_count);
  for (auto index = 0_sz; index < pixel_count; index++) {
    result.confidence[index] = static_cast<f32>(readback->mapped[confidence_offset + index]) / 255.0f;
    result.roughness[index] = static_cast<f32>(readback->mapped[roughness_offset + index]) / 255.0f;
  }

  result.is_valid = true;
  return result;
}

static auto run_case(SSSRDevice& gpu, const CaseConfig& config) -> CaseResult {
  auto session = SSSRSession(gpu);
  if (!session.initialize()) {
    ADD_FAILURE() << "history initialization";
    return {};
  }

  return session.step(config);
}

class SSSRGPU : public ::testing::Test {
public:
  static auto SetUpTestSuite() -> void {
    device = create_sssr_device(skip_reason, failure_reason);
    if (device) {
      std::fprintf(stderr, "TestSSSRGPU device: %s\n", device->device_name.c_str());
    }
  }

  static auto TearDownTestSuite() -> void { device.reset(); }

  auto SetUp() -> void override {
    if (!failure_reason.empty()) {
      FAIL() << failure_reason;
    }

    if (!device) {
      GTEST_SKIP() << skip_reason;
    }
  }

  auto TearDown() -> void override {
    if (device) {
      EXPECT_EQ(device->validation_errors.load(std::memory_order_relaxed), 0u);
    }
  }

  static inline std::unique_ptr<SSSRDevice> device = {};
  static inline std::string skip_reason = {};
  static inline std::string failure_reason = {};
};

static auto mirror_scene() -> TestScene {
  auto scene = TestScene{};
  scene.planes.push_back({.normal = glm::vec3(0.0f, 1.0f, 0.0f), .offset = 0.0f, .roughness = 0.0f});
  scene.boxes.push_back({.min = glm::vec3(-1.0f, 0.0f, -2.5f), .max = glm::vec3(1.0f, 1.5f, -0.5f), .roughness = 1.0f});
  return scene;
}

static auto floor_camera(glm::vec2 jitter = glm::vec2(0.0f)) -> TestCamera {
  return make_test_camera(glm::vec3(0.0f, 2.5f, 6.0f), glm::vec3(0.0f, 0.5f, 0.0f), jitter);
}

static auto pixel_index(u32 x, u32 y) -> usize { return static_cast<usize>(y) * TEST_SIZE + x; }

static auto reflect_floor(const TestScene& scene, const SceneHit& primary, glm::vec3 view_direction) -> SceneHit {
  const auto reflected = glm::reflect(view_direction, primary.normal);
  return scene.trace(primary.position + primary.normal * 1e-3f, reflected);
}

TEST_F(SSSRGPU, ProjectionConventionMatchesTheEngine) {
  const auto camera = floor_camera();
  const auto forward = glm::normalize(glm::vec3(0.0f, -2.0f, -6.0f));
  const auto near_point = project_to_pixel(camera, camera.position + forward * 0.1f);
  const auto far_point = project_to_pixel(camera, camera.position + forward * 900.0f);
  EXPECT_NEAR(near_point.z, 1.0f, 1e-3f);
  EXPECT_LT(far_point.z, 0.01f);
  EXPECT_NEAR(near_point.x, TEST_SIZE * 0.5f, 1e-2f);
}

TEST_F(SSSRGPU, MirrorFloorHitsLandOnTheBoxAndMissesFallBackToTheEnvironment) {
  // the last one is far past what the upscaler asks for, but it makes a camera that ignores the jitter obvious
  for (const auto jitter : {glm::vec2(0.0f), glm::vec2(0.4f, -0.3f), glm::vec2(3.0f, -2.0f)}) {
    SCOPED_TRACE(testing::Message() << "jitter " << jitter.x << " " << jitter.y);
    const auto scene = mirror_scene();
    const auto camera = floor_camera(jitter);
    // a pixel here covers about 3 cm of the box, far more than the 1.5 cm the thickness defaults to for a full
    // resolution frame
    const auto result = run_case(
      *device,
      {.scene = &scene, .camera = camera, .samples_per_quad = 4, .depth_thickness = 0.25f}
    );
    ASSERT_TRUE(result.is_valid);

    auto expected_hits = 0_u32;
    auto hit_failures = 0_u32;
    auto expected_misses = 0_u32;
    auto miss_failures = 0_u32;
    auto worst_pixel_error = 0.0f;
    auto squared_error_sum = 0.0;
    auto error_sum = glm::dvec2(0.0);
    for (auto y = 0_u32; y < TEST_SIZE; y++) {
      for (auto x = 0_u32; x < TEST_SIZE; x++) {
        const auto index = pixel_index(x, y);
        const auto& primary = result.primary[index];
        if (primary.object != 0) {
          continue;
        }

        const auto pixel_center = glm::vec2(x + 0.5f, y + 0.5f);
        const auto reflected = reflect_floor(scene, primary, camera_ray(camera, pixel_center));
        const auto uv = pixel_center / static_cast<f32>(TEST_SIZE);
        const auto is_central = uv.x > 0.1f && uv.x < 0.9f && uv.y > 0.1f && uv.y < 0.9f;
        if (reflected.object == 1) {
          const auto projected = project_to_pixel(camera, reflected.position);
          const auto camera_hit = scene.trace(
            camera.position,
            camera_ray(camera, glm::floor(glm::vec2(projected)) + 0.5f)
          );
          const auto is_visible = camera_hit.object == 1 &&
                                  std::abs(camera_hit.t - glm::distance(camera.position, reflected.position)) < 0.1f;
          // hits within a couple of pixels of the origin are rejected as self reflection
          const auto is_far_enough = glm::distance(glm::vec2(projected), pixel_center) >= 4.0f;
          const auto is_on_screen = projected.x > 16.0f && projected.y > 16.0f && projected.x < TEST_SIZE - 16.0f &&
                                    projected.y < TEST_SIZE - 16.0f;
          if (!is_visible || !is_central || !is_far_enough || !is_on_screen) {
            continue;
          }

          // a ray that grazes a silhouette can fall on either side of it, so only hits with the same face all
          // around them count
          auto is_interior = true;
          const auto hit_pixel = glm::ivec2(projected);
          for (auto offset_y = -3; offset_y <= 3; offset_y++) {
            for (auto offset_x = -3; offset_x <= 3; offset_x++) {
              const auto& neighbour = result.primary[pixel_index(hit_pixel.x + offset_x, hit_pixel.y + offset_y)];
              is_interior = is_interior && neighbour.object == 1 && neighbour.normal == reflected.normal;
            }
          }

          if (!is_interior) {
            continue;
          }

          expected_hits++;
          // the output is lerp(environment, hit, confidence), and the environment is the known ambient
          const auto confidence = std::max(result.confidence[index], 1e-3f);
          const auto hit_radiance = (glm::vec3(result.radiance[index]) - (1.0f - confidence) * SENTINEL_AMBIENT) /
                                    confidence;
          const auto decoded_pixel = project_to_pixel(camera, (hit_radiance - 0.5f) / 0.05f);
          const auto pixel_error = glm::distance(glm::vec2(decoded_pixel), glm::vec2(projected));
          worst_pixel_error = std::max(worst_pixel_error, pixel_error);
          squared_error_sum += pixel_error * pixel_error;
          error_sum += glm::dvec2(decoded_pixel) - glm::dvec2(projected);
          hit_failures += (result.confidence[index] <= 0.0f || pixel_error > 2.0f) ? 1 : 0;
        } else if (reflected.object < 0 && is_central) {
          expected_misses++;
          const auto is_ambient = glm::distance(glm::vec3(result.radiance[index]), SENTINEL_AMBIENT) <= 2e-3f;
          miss_failures += (result.confidence[index] != 0.0f || !is_ambient) ? 1 : 0;
        }
      }
    }

    std::fprintf(
      stderr,
      "mirror floor: %u expected hits (%u failures, worst %.2f px, rms %.3f, mean %.3f %.3f), %u expected misses (%u "
      "failures)\n",
      expected_hits,
      hit_failures,
      worst_pixel_error,
      std::sqrt(squared_error_sum / expected_hits),
      error_sum.x / expected_hits,
      error_sum.y / expected_hits,
      expected_misses,
      miss_failures
    );
    EXPECT_GE(expected_hits, 1000u);
    EXPECT_GE(expected_misses, 10000u);
    EXPECT_LE(hit_failures, expected_hits / 50);
    EXPECT_LT(std::sqrt(squared_error_sum / expected_hits), 0.7);
    EXPECT_LE(miss_failures, expected_misses / 50);
  }
}

TEST_F(SSSRGPU, EveryProgramIsInThePack) {
  for (const auto* name :
       {"sssr_blue_noise",
        "sssr_classify_ddgi",
        "sssr_classify_sky",
        "sssr_classify_ambient",
        "sssr_indirect_args",
        "sssr_intersect_ddgi",
        "sssr_intersect_ddgi_sky",
        "sssr_intersect_sky",
        "sssr_intersect_ambient",
        "sssr_reproject",
        "sssr_prefilter",
        "sssr_resolve_temporal",
        "sssr_composite",
        "sssr_debug"}) {
    EXPECT_NE(device->render->pipeline(name), vir::PipelineID::Invalid) << name;
  }
}

TEST_F(SSSRGPU, EachEnvironmentVariantFallsBackToItsOwnEnvironment) {
  struct Variant {
    const char* name;
    TestEnvironment environment;
    glm::vec3 rough_color;
    std::array<glm::vec3, 2> miss_colors;
  };
  const auto variants = std::array{
    Variant{"sky", TestEnvironment::Sky, SKY_CUBEMAP_COLOR, {SKY_VIEW_COLOR, SKY_VIEW_COLOR}},
    Variant{"ddgi", TestEnvironment::DDGI, DDGI_AMBIENT_COLOR, {DDGI_AMBIENT_COLOR, DDGI_AMBIENT_COLOR}},
    Variant{"ddgi and sky", TestEnvironment::DDGISky, DDGI_AMBIENT_COLOR, {SKY_VIEW_COLOR, DDGI_AMBIENT_COLOR}},
  };
  const auto scene = mirror_scene();
  const auto camera = floor_camera();
  for (const auto& variant : variants) {
    SCOPED_TRACE(variant.name);
    const auto result = run_case(
      *device,
      {.scene = &scene,
       .camera = camera,
       .samples_per_quad = 4,
       .depth_thickness = 0.25f,
       .environment = variant.environment}
    );
    ASSERT_TRUE(result.is_valid);

    auto rough_pixels = 0_u32;
    auto rough_failures = 0_u32;
    auto miss_pixels = 0_u32;
    auto miss_failures = 0_u32;
    auto hit_pixels = 0_u32;
    for (auto y = 0_u32; y < TEST_SIZE; y++) {
      for (auto x = 0_u32; x < TEST_SIZE; x++) {
        const auto index = pixel_index(x, y);
        const auto& primary = result.primary[index];
        const auto color = glm::vec3(result.radiance[index]);
        if (primary.object == 1) {
          rough_pixels++;
          rough_failures += glm::distance(color, variant.rough_color) > 3e-3f ? 1 : 0;
        } else if (primary.object == 0) {
          const auto pixel_center = glm::vec2(x + 0.5f, y + 0.5f);
          const auto reflected = reflect_floor(scene, primary, camera_ray(camera, pixel_center));
          if (reflected.object < 0 && result.confidence[index] == 0.0f) {
            miss_pixels++;
            const auto is_environment = glm::distance(color, variant.miss_colors[0]) <= 3e-3f ||
                                        glm::distance(color, variant.miss_colors[1]) <= 3e-3f;
            miss_failures += is_environment ? 0 : 1;
          }

          hit_pixels += result.confidence[index] > 0.5f ? 1 : 0;
        }
      }
    }

    EXPECT_GT(rough_pixels, 1000u);
    EXPECT_GT(miss_pixels, 10000u);
    EXPECT_GT(hit_pixels, 1000u);
    EXPECT_EQ(rough_failures, 0u);
    EXPECT_LE(miss_failures, miss_pixels / 100);
  }
}

TEST_F(SSSRGPU, SkyIsNeverTracedAndStoresRoughnessOne) {
  const auto scene = mirror_scene();
  const auto result = run_case(*device, {.scene = &scene, .camera = floor_camera(), .samples_per_quad = 4});
  ASSERT_TRUE(result.is_valid);

  auto sky_pixels = 0_u32;
  auto failures = 0_u32;
  for (auto index = 0_sz; index < result.primary.size(); index++) {
    if (result.primary[index].object >= 0) {
      continue;
    }

    sky_pixels++;
    const auto& radiance = result.radiance[index];
    if (
      glm::any(glm::notEqual(radiance, glm::vec4(0.0f))) || result.confidence[index] != 0.0f ||
      result.roughness[index] != 1.0f
    ) {
      failures++;
    }
  }

  EXPECT_GT(sky_pixels, 5000u);
  EXPECT_EQ(failures, 0u);
}

TEST_F(SSSRGPU, TiltedPlaneNeverHitsItselfWithOrWithoutJitter) {
  for (const auto normal : {glm::vec3(0.3f, 1.0f, 0.25f), glm::vec3(0.1f, 1.0f, 0.9f)}) {
    for (const auto jitter : {glm::vec2(0.0f), glm::vec2(0.4f, -0.3f)}) {
      for (const auto roughness : {0.0f, 0.2f}) {
        SCOPED_TRACE(
          testing::Message() << "normal " << normal.x << " " << normal.y << " " << normal.z << " jitter " << jitter.x
                             << " " << jitter.y << " roughness " << roughness
        );
        auto scene = TestScene{};
        scene.planes.push_back({.normal = glm::normalize(normal), .offset = 0.0f, .roughness = roughness});
        const auto result = run_case(*device, {.scene = &scene, .camera = floor_camera(jitter), .samples_per_quad = 4});
        ASSERT_TRUE(result.is_valid);

        auto plane_pixels = 0_u32;
        auto hits = 0_u32;
        auto wrong_environment = 0_u32;
        for (auto index = 0_sz; index < result.primary.size(); index++) {
          if (result.primary[index].object < 0) {
            continue;
          }

          plane_pixels++;
          if (result.confidence[index] != 0.0f) {
            hits++;
          }

          if (glm::distance(glm::vec3(result.radiance[index]), SENTINEL_AMBIENT) > 2e-3f) {
            wrong_environment++;
          }
        }

        EXPECT_GT(plane_pixels, 5000u);
        EXPECT_EQ(hits, 0u);
        EXPECT_EQ(wrong_environment, 0u);
      }
    }
  }
}

TEST_F(SSSRGPU, SamplesPerQuadControlsTheRayCountAndCopiesFillTheQuad) {
  auto scene = mirror_scene();
  scene.planes[0].roughness = 0.2f;
  scene.boxes[0].roughness = 0.2f;

  for (const auto spp : {4_u32, 2_u32, 1_u32}) {
    SCOPED_TRACE(spp);
    const auto result = run_case(*device, {.scene = &scene, .camera = floor_camera(), .samples_per_quad = spp});
    ASSERT_TRUE(result.is_valid);

    // every pixel here is glossy, a non base pixel only traces on its own when the pixel it copies from is sky
    const auto is_reflective = [&](u32 x, u32 y) -> bool {
      return result.primary[pixel_index(x, y)].object >= 0;
    };
    auto expected = 0_u32;
    auto reflective = 0_u32;
    for (auto y = 0_u32; y < TEST_SIZE; y++) {
      for (auto x = 0_u32; x < TEST_SIZE; x++) {
        if (!is_reflective(x, y)) {
          continue;
        }

        reflective++;
        const auto is_base = spp == 4 || (spp == 1 ? ((x & 1u) | (y & 1u)) == 0 : (x & 1u) == (y & 1u));
        const auto is_source_reflective = spp == 1 ? is_reflective(x & ~1u, y & ~1u) : is_reflective(x ^ 1u, y);
        if (is_base || !is_source_reflective) {
          expected++;
        }
      }
    }

    const auto ratio = spp == 4 ? 1.0f : (spp == 2 ? 0.5f : 0.25f);
    EXPECT_EQ(result.counters[1], expected);
    EXPECT_GE(result.counters[1], static_cast<u32>(reflective * ratio));
    EXPECT_LE(result.counters[1], static_cast<u32>(reflective * ratio) + reflective / 50);
    std::fprintf(stderr, "spp %u: %u rays for %u reflective pixels\n", spp, result.counters[1], reflective);

    auto compared_quads = 0_u32;
    auto mismatches = 0_u32;
    for (auto y = 0_u32; y + 1 < TEST_SIZE; y += 2) {
      for (auto x = 0_u32; x + 1 < TEST_SIZE; x += 2) {
        if (
          !is_reflective(x, y) || !is_reflective(x + 1, y) || !is_reflective(x, y + 1) || !is_reflective(x + 1, y + 1)
        ) {
          continue;
        }

        compared_quads++;
        const auto base = result.radiance[pixel_index(x, y)];
        const auto horizontal = result.radiance[pixel_index(x + 1, y)];
        const auto vertical = result.radiance[pixel_index(x, y + 1)];
        const auto diagonal = result.radiance[pixel_index(x + 1, y + 1)];
        if (spp == 1) {
          mismatches += (horizontal != base) + (vertical != base) + (diagonal != base);
        } else if (spp == 2) {
          mismatches += (horizontal != base) + (vertical != diagonal);
        }
      }
    }

    EXPECT_GT(compared_quads, 1000u);
    EXPECT_EQ(mismatches, 0u);
  }
}

TEST_F(SSSRGPU, MirrorPixelsAreTracedAtEverySampleRate) {
  const auto scene = mirror_scene();
  for (const auto spp : {4_u32, 2_u32, 1_u32}) {
    SCOPED_TRACE(spp);
    const auto result = run_case(*device, {.scene = &scene, .camera = floor_camera(), .samples_per_quad = spp});
    ASSERT_TRUE(result.is_valid);

    auto mirror_pixels = 0_u32;
    for (const auto& primary : result.primary) {
      mirror_pixels += primary.object == 0 ? 1 : 0;
    }

    EXPECT_GT(mirror_pixels, 10000u);
    EXPECT_EQ(result.counters[1], mirror_pixels);
  }
}

TEST_F(SSSRGPU, GlossyPixelsWhoseBasePixelIsRoughStillGetTraced) {
  auto scene = mirror_scene();
  scene.planes[0].roughness = 0.2f;
  scene.boxes[0].roughness = 0.2f;

  struct Layout {
    const char* name;
    RoughnessOverride roughness;
  };
  const auto layouts = std::array{
    Layout{"base pixels rough", [](u32 x, u32 y, f32 glossy) -> f32 { return ((x | y) & 1u) == 0 ? 1.0f : glossy; }},
    Layout{"checkerboard", [](u32 x, u32 y, f32 glossy) -> f32 { return ((x + y) & 1u) == 0 ? 1.0f : glossy; }},
    Layout{"checkerboard of quads", [](u32 x, u32 y, f32 glossy) -> f32 {
             return (((x >> 1) + (y >> 1)) & 1u) == 0 ? 1.0f : glossy;
           }},
  };
  for (const auto& layout : layouts) {
    SCOPED_TRACE(layout.name);
    const auto result = run_case(
      *device,
      {.scene = &scene, .camera = floor_camera(), .samples_per_quad = 1, .roughness = layout.roughness}
    );
    ASSERT_TRUE(result.is_valid);

    auto glossy_pixels = 0_u32;
    auto holes = 0_u32;
    auto rough_with_rays = 0_u32;
    for (auto index = 0_sz; index < result.primary.size(); index++) {
      if (result.primary[index].object < 0) {
        continue;
      }

      const auto is_glossy = result.surface_roughness[index] < 0.5f;
      const auto& radiance = result.radiance[index];
      if (is_glossy) {
        glossy_pixels++;
        holes += (glm::vec3(radiance) == glm::vec3(0.0f) && radiance.w == 0.0f) ? 1 : 0;
      } else if (radiance.w != 0.0f) {
        rough_with_rays++;
      }
    }

    EXPECT_GT(glossy_pixels, 5000u);
    EXPECT_EQ(holes, 0u);
    EXPECT_EQ(rough_with_rays, 0u);
  }
}

TEST_F(SSSRGPU, RoughPixelsGetNoRayAndTheAmbientEnvironment) {
  auto scene = mirror_scene();
  scene.planes[0].roughness = 0.8f;
  scene.boxes[0].roughness = 0.8f;
  const auto result = run_case(*device, {.scene = &scene, .camera = floor_camera(), .samples_per_quad = 4});
  ASSERT_TRUE(result.is_valid);

  EXPECT_EQ(result.counters[1], 0u);
  auto surface_pixels = 0_u32;
  auto failures = 0_u32;
  for (auto index = 0_sz; index < result.primary.size(); index++) {
    if (result.primary[index].object < 0) {
      continue;
    }

    surface_pixels++;
    const auto& radiance = result.radiance[index];
    if (glm::distance(glm::vec3(radiance), SENTINEL_AMBIENT) > 2e-3f || radiance.w != 0.0f) {
      failures++;
    }
  }

  EXPECT_GT(surface_pixels, 5000u);
  EXPECT_EQ(failures, 0u);
}

// --- multi frame ---

static auto glossy_scene() -> TestScene {
  auto scene = mirror_scene();
  scene.planes[0].roughness = 0.2f;
  return scene;
}

static auto checker_lit(const SceneHit& hit) -> glm::vec3 {
  const auto cell = glm::ivec3(glm::floor(hit.position / 0.04f));
  return ((cell.x + cell.y + cell.z) & 1) != 0 ? glm::vec3(0.9f, 0.8f, 0.7f) : glm::vec3(0.1f, 0.2f, 0.3f);
}

// only x varies, so the lit value is linear in the screen position the tile average is taken over and the temporal pull
// toward that average cancels to first order
// steep enough that a pixel spans many fp16 steps, at 0.05 the history's 1/8 blend toward the new sample rounded away
// and the mirror drifted 1.5 px behind under motion
static auto x_gradient_lit(const SceneHit& hit) -> glm::vec3 { return {hit.position.x * 0.4f + 0.5f, 0.5f, 0.5f}; }

struct BoxHit {
  usize index = 0;
  glm::vec2 projected = {};
  glm::vec3 world = {};
};

// floor pixels whose analytic reflection lands well inside a box face that is visible to the camera
static auto collect_box_hits(const TestScene& scene, const TestCamera& camera, std::span<const SceneHit> primary)
  -> std::vector<BoxHit> {
  auto hits = std::vector<BoxHit>{};
  for (auto y = 0_u32; y < TEST_SIZE; y++) {
    for (auto x = 0_u32; x < TEST_SIZE; x++) {
      const auto index = pixel_index(x, y);
      if (primary[index].object != 0) {
        continue;
      }

      const auto pixel_center = glm::vec2(x + 0.5f, y + 0.5f);
      const auto reflected = reflect_floor(scene, primary[index], camera_ray(camera, pixel_center));
      const auto uv = pixel_center / static_cast<f32>(TEST_SIZE);
      const auto is_central = uv.x > 0.1f && uv.x < 0.9f && uv.y > 0.1f && uv.y < 0.9f;
      if (reflected.object != 1 || !is_central) {
        continue;
      }

      const auto projected = project_to_pixel(camera, reflected.position);
      const auto camera_hit = scene.trace(camera.position, camera_ray(camera, glm::floor(glm::vec2(projected)) + 0.5f));
      const auto is_visible = camera_hit.object == 1 &&
                              std::abs(camera_hit.t - glm::distance(camera.position, reflected.position)) < 0.1f;
      const auto is_far_enough = glm::distance(glm::vec2(projected), pixel_center) >= 4.0f;
      const auto is_on_screen = projected.x > 16.0f && projected.y > 16.0f && projected.x < TEST_SIZE - 16.0f &&
                                projected.y < TEST_SIZE - 16.0f;
      if (!is_visible || !is_far_enough || !is_on_screen) {
        continue;
      }

      auto is_interior = true;
      const auto hit_pixel = glm::ivec2(projected);
      for (auto offset_y = -3; offset_y <= 3; offset_y++) {
        for (auto offset_x = -3; offset_x <= 3; offset_x++) {
          const auto& neighbour = primary[pixel_index(hit_pixel.x + offset_x, hit_pixel.y + offset_y)];
          is_interior = is_interior && neighbour.object == 1 && neighbour.normal == reflected.normal;
        }
      }

      if (is_interior) {
        hits.push_back({index, glm::vec2(projected), reflected.position});
      }
    }
  }

  return hits;
}

static auto make_mask(std::span<const BoxHit> hits) -> std::vector<u8> {
  auto mask = std::vector<u8>(static_cast<usize>(TEST_SIZE) * TEST_SIZE, 0);
  for (const auto& hit : hits) {
    mask[hit.index] = 1;
  }

  return mask;
}

// the temporal pass pulls every glossy pixel toward the bilinearly read average of its 8x8 tiles, so a pixel that is
// to be compared against the analytic position needs a clean reflection all around it
static auto erode_hits(std::span<const BoxHit> hits, i32 radius) -> std::vector<BoxHit> {
  const auto mask = make_mask(hits);
  auto kept = std::vector<BoxHit>{};
  for (const auto& hit : hits) {
    const auto x = static_cast<i32>(hit.index % TEST_SIZE);
    const auto y = static_cast<i32>(hit.index / TEST_SIZE);
    auto is_clean = x >= radius && y >= radius && x + radius < static_cast<i32>(TEST_SIZE) &&
                    y + radius < static_cast<i32>(TEST_SIZE);
    for (auto offset_y = -radius; offset_y <= radius && is_clean; offset_y++) {
      for (auto offset_x = -radius; offset_x <= radius && is_clean; offset_x++) {
        is_clean = mask[pixel_index(x + offset_x, y + offset_y)] != 0;
      }
    }

    if (is_clean) {
      kept.push_back(hit);
    }
  }

  return kept;
}

static auto mean_color(std::span<const u8> mask, std::span<const glm::vec4> image) -> glm::dvec3 {
  auto sum = glm::dvec3(0.0);
  auto count = 0_u64;
  for (auto index = 0_sz; index < mask.size(); index++) {
    if (mask[index]) {
      sum += glm::dvec3(image[index]);
      count++;
    }
  }

  return sum / static_cast<f64>(std::max<u64>(count, 1));
}

// mean absolute difference between horizontal neighbours that are both in the mask
static auto mean_gradient(std::span<const u8> mask, std::span<const glm::vec4> image) -> f64 {
  auto sum = 0.0;
  auto count = 0_u64;
  for (auto y = 0_u32; y < TEST_SIZE; y++) {
    for (auto x = 0_u32; x + 1 < TEST_SIZE; x++) {
      const auto a = pixel_index(x, y);
      const auto b = pixel_index(x + 1, y);
      if (mask[a] && mask[b]) {
        const auto delta = glm::abs(glm::vec3(image[a]) - glm::vec3(image[b]));
        sum += (delta.x + delta.y + delta.z) / 3.0f;
        count++;
      }
    }
  }

  return sum / static_cast<f64>(std::max<u64>(count, 1));
}

static auto mean_difference(std::span<const u8> mask, std::span<const glm::vec4> a, std::span<const glm::vec4> b)
  -> f64 {
  auto sum = 0.0;
  auto count = 0_u64;
  for (auto index = 0_sz; index < mask.size(); index++) {
    if (mask[index]) {
      const auto delta = glm::abs(glm::vec3(a[index]) - glm::vec3(b[index]));
      sum += (delta.x + delta.y + delta.z) / 3.0f;
      count++;
    }
  }

  return sum / static_cast<f64>(std::max<u64>(count, 1));
}

static auto mean_scalar(std::span<const u8> mask, std::span<const f32> image) -> f64 {
  auto sum = 0.0;
  auto count = 0_u64;
  for (auto index = 0_sz; index < mask.size(); index++) {
    if (mask[index]) {
      sum += image[index];
      count++;
    }
  }

  return sum / static_cast<f64>(std::max<u64>(count, 1));
}

struct PositionError {
  f64 rms = 0.0;
  f32 worst = 0.0f;
  f64 mean = 0.0;
};

// the lit value of a hit encodes its world x, so the radiance says where along the screen the ray really landed
static auto decode_x_error(
  const TestCamera& camera,
  std::span<const BoxHit> hits,
  std::span<const glm::vec4> radiance,
  std::span<const f32> confidence
) -> PositionError {
  auto error = PositionError{};
  auto squared = 0.0;
  for (const auto& hit : hits) {
    const auto hit_confidence = std::max(confidence[hit.index], 1e-3f);
    const auto red = (radiance[hit.index].x - (1.0f - hit_confidence) * SENTINEL_AMBIENT.x) / hit_confidence;
    auto decoded = hit.world;
    decoded.x = (red - 0.5f) / 0.4f;
    const auto signed_distance = project_to_pixel(camera, decoded).x - hit.projected.x;
    const auto distance = std::abs(signed_distance);
    error.worst = std::max(error.worst, distance);
    squared += distance * distance;
    error.mean += signed_distance;
  }

  error.mean /= static_cast<f64>(std::max<usize>(hits.size(), 1));

  error.rms = std::sqrt(squared / static_cast<f64>(std::max<usize>(hits.size(), 1)));
  return error;
}

static auto halton(u32 index, u32 base) -> f32 {
  auto result = 0.0f;
  auto fraction = 1.0f;
  while (index > 0) {
    fraction /= static_cast<f32>(base);
    result += fraction * static_cast<f32>(index % base);
    index /= base;
  }

  return result;
}

static auto jitter_for_frame(u32 frame, f32 amplitude) -> glm::vec2 {
  return {(halton(frame % 8 + 1, 2) - 0.5f) * amplitude, (halton(frame % 8 + 1, 3) - 0.5f) * amplitude};
}

using ConfigForFrame = std::function<CaseConfig(u32 frame)>;
using ConsumeFrame = std::function<void(u32 frame, CaseResult& result)>;

static auto run_frames(SSSRSession& session, u32 frames, const ConfigForFrame& make_config, const ConsumeFrame& consume)
  -> bool {
  for (auto frame = 0_u32; frame < frames; frame++) {
    auto result = session.step(make_config(frame));
    if (!result.is_valid) {
      return false;
    }

    consume(frame, result);
  }

  return true;
}

TEST_F(SSSRGPU, GlossyFloorConvergesToALowNoiseStableSignal) {
  const auto scene = glossy_scene();
  const auto camera = floor_camera();
  auto session = SSSRSession(*device);
  ASSERT_TRUE(session.initialize());

  auto mask = std::vector<u8>{};
  auto raw_gradient = 0.0;
  auto previous = std::vector<glm::vec4>{};
  auto changes = std::vector<f64>{};
  auto sample_means = std::vector<f64>{};
  auto max_sample_count = 0.0f;
  auto last = CaseResult{};
  constexpr auto frames = 40_u32;
  ASSERT_TRUE(run_frames(
    session,
    frames,
    [&](u32) -> CaseConfig {
      return {.scene = &scene, .camera = camera, .samples_per_quad = 1, .depth_thickness = 0.25f, .lit = checker_lit};
    },
    [&](u32 frame, CaseResult& result) -> void {
      if (frame == 0) {
        mask = make_mask(collect_box_hits(scene, camera, result.primary));
        raw_gradient = mean_gradient(mask, result.radiance);
      } else {
        changes.push_back(mean_difference(mask, result.denoised, previous));
      }

      sample_means.push_back(mean_scalar(mask, result.sample_count));
      for (const auto count : result.sample_count) {
        max_sample_count = std::max(max_sample_count, count);
      }

      previous = result.denoised;
      last = std::move(result);
    }
  ));

  const auto denoised_gradient = mean_gradient(mask, last.denoised);
  std::fprintf(
    stderr,
    "convergence: neighbour gradient raw %.4f denoised %.4f (ratio %.3f), frame to frame change after frame 2 %.4f, 8 "
    "%.4f, 16 %.4f, 32 %.4f, 40 %.4f, mean sample count frame 1 %.2f, 8 %.2f, 32 %.2f, 40 %.2f (max %.2f)\n",
    raw_gradient,
    denoised_gradient,
    denoised_gradient / raw_gradient,
    changes[0],
    changes[6],
    changes[14],
    changes[30],
    changes[38],
    sample_means[0],
    sample_means[7],
    sample_means[31],
    sample_means[39],
    max_sample_count
  );
  EXPECT_GT(raw_gradient, 0.05);
  EXPECT_LT(denoised_gradient, 0.2 * raw_gradient);
  EXPECT_LT(changes[38], 0.1 * raw_gradient);
  EXPECT_LT(changes[38], 0.1 * changes[0]);
  EXPECT_GT(changes[6], changes[14]);
  EXPECT_GT(changes[14], changes[30]);
  EXPECT_NEAR(sample_means[0], 1.0, 0.05);
  // 32 * (1 - exp(-100 * alpha)) with alpha 0.04
  EXPECT_NEAR(sample_means[39], 31.4, 0.5);
  EXPECT_LE(max_sample_count, 32.0f);
}

TEST_F(SSSRGPU, DenoisingKeepsTheMeanRadiance) {
  const auto scene = glossy_scene();
  const auto camera = floor_camera();
  const auto uniform_lit = [](const SceneHit&) -> glm::vec3 {
    return glm::vec3(0.6f, 0.4f, 0.2f);
  };
  struct Signal {
    const char* name;
    LitFunction lit;
    f64 tolerance;
  };
  for (const auto& signal : {Signal{"uniform", uniform_lit, 0.04}, Signal{"checker", checker_lit, 0.04}}) {
    SCOPED_TRACE(signal.name);
    auto session = SSSRSession(*device);
    ASSERT_TRUE(session.initialize());

    auto mask = std::vector<u8>{};
    auto raw_mean = glm::dvec3(0.0);
    auto denoised_mean = glm::dvec3(0.0);
    constexpr auto frames = 24_u32;
    ASSERT_TRUE(run_frames(
      session,
      frames,
      [&](u32) -> CaseConfig {
        return {.scene = &scene, .camera = camera, .samples_per_quad = 1, .depth_thickness = 0.25f, .lit = signal.lit};
      },
      [&](u32 frame, CaseResult& result) -> void {
        if (frame == 0) {
          mask = make_mask(collect_box_hits(scene, camera, result.primary));
        }

        if (frame >= frames - 8) {
          raw_mean += mean_color(mask, result.radiance) / 8.0;
        }

        if (frame == frames - 1) {
          denoised_mean = mean_color(mask, result.denoised);
        }
      }
    ));

    const auto relative = (denoised_mean - raw_mean) / raw_mean;
    std::fprintf(
      stderr,
      "energy %s: raw mean %.4f %.4f %.4f, denoised mean %.4f %.4f %.4f, relative %.4f %.4f %.4f\n",
      signal.name,
      raw_mean.x,
      raw_mean.y,
      raw_mean.z,
      denoised_mean.x,
      denoised_mean.y,
      denoised_mean.z,
      relative.x,
      relative.y,
      relative.z
    );
    for (auto channel = 0; channel < 3; channel++) {
      EXPECT_LT(std::abs(relative[channel]), signal.tolerance);
    }
  }
}

TEST_F(SSSRGPU, MirrorStaysSharpThroughTheDenoiser) {
  const auto scene = mirror_scene();
  const auto camera = floor_camera();
  auto session = SSSRSession(*device);
  ASSERT_TRUE(session.initialize());

  auto last = CaseResult{};
  constexpr auto frames = 32_u32;
  ASSERT_TRUE(run_frames(
    session,
    frames,
    [&](u32) -> CaseConfig {
      return {
        .scene = &scene,
        .camera = camera,
        .samples_per_quad = 1,
        .depth_thickness = 0.25f,
        .lit = x_gradient_lit,
      };
    },
    [&](u32 frame, CaseResult& result) -> void {
      if (frame == frames - 1) {
        last = std::move(result);
      }
    }
  ));

  const auto hits = erode_hits(collect_box_hits(scene, camera, last.primary), 12);
  const auto mask = make_mask(hits);
  const auto raw_error = decode_x_error(camera, hits, last.radiance, last.confidence);
  const auto denoised_error = decode_x_error(camera, hits, last.denoised, last.confidence);
  const auto raw_gradient = mean_gradient(mask, last.radiance);
  const auto denoised_gradient = mean_gradient(mask, last.denoised);
  std::fprintf(
    stderr,
    "mirror sharp: %zu hits, position error against the analytic hit rms raw %.3f denoised %.3f (worst %.2f), "
    "gradient raw %.5f denoised %.5f, sample count %.2f\n",
    hits.size(),
    raw_error.rms,
    denoised_error.rms,
    denoised_error.worst,
    raw_gradient,
    denoised_gradient,
    mean_scalar(mask, last.sample_count)
  );
  EXPECT_GT(hits.size(), 300u);
  EXPECT_LT(raw_error.rms, 1.0);
  EXPECT_LT(denoised_error.rms, 1.0);
  EXPECT_LT(denoised_error.worst, 1.5f);
  EXPECT_GT(denoised_gradient, 0.9 * raw_gradient);
  EXPECT_LT(denoised_gradient, 1.1 * raw_gradient);
}

TEST_F(SSSRGPU, ResultsScaleWithTheRadianceNotTheExposure) {
  const auto scene = glossy_scene();
  const auto camera = floor_camera();
  struct Run {
    const char* name;
    f32 scale;
    f32 fixed_exposure;
    bool is_eye_adapted;
    f32 buffer_exposure;
    f32 expected_scale;
  };
  const auto runs = std::array{
    Run{"reference", 1.0f, 1.0f, false, 1.0f, 1.0f},
    Run{"fixed exposure", 10.0f, 0.1f, false, 0.1f, 10.0f},
    Run{"fixed exposure 4", 0.25f, 4.0f, false, 4.0f, 0.25f},
    Run{"eye adapted buffer", 10.0f, 123.0f, true, 0.1f, 10.0f},
    Run{"nan buffer falls back to 1", 1.0f, 7.0f, true, std::numeric_limits<f32>::quiet_NaN(), 1.0f},
    Run{"infinite buffer falls back to 1", 1.0f, 7.0f, true, std::numeric_limits<f32>::infinity(), 1.0f},
    Run{"zero buffer falls back to 1", 1.0f, 7.0f, true, 0.0f, 1.0f},
    Run{"negative fixed exposure falls back to 1", 1.0f, -3.0f, false, 1.0f, 1.0f},
    Run{"zero fixed exposure falls back to 1", 1.0f, 0.0f, false, 1.0f, 1.0f},
  };
  auto reference = std::vector<glm::vec4>{};
  auto mask = std::vector<u8>{};
  for (const auto& run : runs) {
    SCOPED_TRACE(run.name);
    auto session = SSSRSession(*device);
    ASSERT_TRUE(session.initialize());

    auto last = CaseResult{};
    constexpr auto frames = 10_u32;
    ASSERT_TRUE(run_frames(
      session,
      frames,
      [&](u32) -> CaseConfig {
        return {
          .scene = &scene,
          .camera = camera,
          .samples_per_quad = 1,
          .depth_thickness = 0.25f,
          .lit = checker_lit,
          .radiance_scale = run.scale,
          .fixed_exposure = run.fixed_exposure,
          .is_eye_adapted = run.is_eye_adapted,
          .buffer_exposure = run.buffer_exposure,
        };
      },
      [&](u32 frame, CaseResult& result) -> void {
        if (frame == frames - 1) {
          last = std::move(result);
        }
      }
    ));

    if (reference.empty()) {
      reference = last.denoised;
      mask.assign(reference.size(), 0);
      for (auto index = 0_sz; index < mask.size(); index++) {
        mask[index] = last.primary[index].object >= 0 ? 1 : 0;
      }
    }

    // two identical runs already differ on a few pixels because the traversal depends on the wave occupancy, so the
    // comparison counts outliers instead of taking the worst pixel
    auto absolute_sum = 0.0;
    auto reference_sum = 0.0;
    auto compared = 0_u32;
    auto outliers = 0_u32;
    for (auto index = 0_sz; index < mask.size(); index++) {
      if (!mask[index]) {
        continue;
      }

      const auto expected = glm::vec3(reference[index]) * run.expected_scale;
      const auto delta = glm::abs(glm::vec3(last.denoised[index]) - expected);
      absolute_sum += delta.x + delta.y + delta.z;
      reference_sum += expected.x + expected.y + expected.z;
      compared++;
      outliers += glm::compMax(delta) > 0.02f * run.expected_scale ? 1 : 0;
    }

    const auto relative = absolute_sum / reference_sum;
    const auto outlier_ratio = static_cast<f64>(outliers) / compared;
    std::fprintf(
      stderr,
      "exposure %s: relative error %.6f, %u of %u pixels off by more than 2%%\n",
      run.name,
      relative,
      outliers,
      compared
    );
    EXPECT_LT(relative, 3e-3);
    EXPECT_LT(outlier_ratio, 2e-3);
  }
}

TEST_F(SSSRGPU, MovingCameraKeepsTheMirrorOnItsAnalyticPosition) {
  const auto scene = mirror_scene();
  auto session = SSSRSession(*device);
  ASSERT_TRUE(session.initialize());

  constexpr auto frames = 16_u32;
  constexpr auto warmup = 5_u32;
  constexpr auto step = 0.1f;
  const auto camera_at = [](u32 frame) -> TestCamera {
    const auto offset = glm::vec3(step * static_cast<f32>(frame), 0.0f, 0.0f);
    return make_test_camera(glm::vec3(0.0f, 2.5f, 6.0f) + offset, glm::vec3(0.0f, 0.5f, 0.0f) + offset, {});
  };
  auto previous_denoised = std::vector<glm::vec4>{};
  auto worst_denoised = 0.0;
  auto worst_raw = 0.0;
  auto worst_stale = 1e9;
  auto previous_camera = camera_at(0);
  ASSERT_TRUE(run_frames(
    session,
    frames,
    [&](u32 frame) -> CaseConfig {
      const auto current = camera_at(frame);
      auto config = CaseConfig{
        .scene = &scene,
        .camera = current,
        .previous_camera = &previous_camera,
        .samples_per_quad = 1,
        .depth_thickness = 0.25f,
        .lit = x_gradient_lit,
      };
      return config;
    },
    [&](u32 frame, CaseResult& result) -> void {
      const auto current = camera_at(frame);
      previous_camera = current;
      if (frame >= warmup) {
        const auto hits = erode_hits(collect_box_hits(scene, current, result.primary), 12);
        const auto denoised = decode_x_error(current, hits, result.denoised, result.confidence);
        const auto raw = decode_x_error(current, hits, result.radiance, result.confidence);
        const auto stale = decode_x_error(current, hits, previous_denoised, result.confidence);
        worst_denoised = std::max(worst_denoised, denoised.rms);
        worst_raw = std::max(worst_raw, raw.rms);
        worst_stale = std::min(worst_stale, stale.rms);
        std::fprintf(
          stderr,
          "camera motion frame %u: %zu hits, position rms raw trace %.3f denoised %.3f (mean %.3f, worst %.2f), the "
          "previous frame's output would be %.3f, sample count %.2f\n",
          frame,
          hits.size(),
          raw.rms,
          denoised.rms,
          denoised.mean,
          denoised.worst,
          stale.rms,
          mean_scalar(make_mask(hits), result.sample_count)
        );
        EXPECT_GT(hits.size(), 300u);
      }

      previous_denoised = result.denoised;
    }
  ));

  std::fprintf(
    stderr,
    "camera motion: worst rms denoised %.3f px, raw trace %.3f, smallest rms of the stale output %.3f\n",
    worst_denoised,
    worst_raw,
    worst_stale
  );
  EXPECT_LT(worst_raw, 1.0);
  EXPECT_LT(worst_denoised, worst_raw + 0.4);
  EXPECT_LT(worst_denoised, 1.0);
  EXPECT_GT(worst_stale, 2.0 * worst_denoised);
}

TEST_F(SSSRGPU, DisocclusionRestartsTheHistoryWithoutLeakingIt) {
  auto scene_a = glossy_scene();
  scene_a.boxes[0].roughness = 0.2f;
  auto scene_b = scene_a;
  scene_b.boxes[0].min.x += 2.5f;
  scene_b.boxes[0].max.x += 2.5f;
  const auto camera = floor_camera();
  auto session = SSSRSession(*device);
  ASSERT_TRUE(session.initialize());

  constexpr auto settle_frames = 24_u32;
  constexpr auto change_frames = 6_u32;
  auto old_primary = std::vector<SceneHit>{};
  auto old_denoised = std::vector<glm::vec4>{};
  auto changed = std::vector<u8>{};
  auto unchanged = std::vector<u8>{};
  auto changed_floor = std::vector<u8>{};
  auto counts = std::vector<f64>{};
  auto unchanged_counts = std::vector<f64>{};
  auto first_new_error = 0.0;
  auto old_error = 0.0;
  auto changed_pixels = 0_sz;
  auto restarted_pixels = 0_sz;
  ASSERT_TRUE(run_frames(
    session,
    settle_frames + change_frames,
    [&](u32 frame) -> CaseConfig {
      return {
        .scene = frame < settle_frames ? &scene_a : &scene_b,
        .camera = camera,
        .samples_per_quad = 1,
        .depth_thickness = 0.25f,
      };
    },
    [&](u32 frame, CaseResult& result) -> void {
      if (frame == settle_frames - 1) {
        old_primary = result.primary;
        old_denoised = result.denoised;
      }

      if (frame < settle_frames) {
        return;
      }

      if (frame == settle_frames) {
        changed.assign(result.primary.size(), 0);
        unchanged.assign(result.primary.size(), 0);
        changed_floor.assign(result.primary.size(), 0);
        for (auto y = 4_u32; y + 4 < TEST_SIZE; y++) {
          for (auto x = 4_u32; x + 4 < TEST_SIZE; x++) {
            const auto index = pixel_index(x, y);
            if (result.primary[index].object != old_primary[index].object) {
              changed[index] = 1;
              changed_floor[index] = result.primary[index].object == 0 ? 1 : 0;
              continue;
            }

            auto is_clean = result.primary[index].object >= 0;
            for (auto offset_y = -4; offset_y <= 4 && is_clean; offset_y++) {
              for (auto offset_x = -4; offset_x <= 4; offset_x++) {
                const auto neighbour = pixel_index(x + offset_x, y + offset_y);
                is_clean = is_clean && result.primary[neighbour].object == old_primary[neighbour].object;
              }
            }

            unchanged[index] = is_clean ? 1 : 0;
          }
        }

        for (auto index = 0_sz; index < changed.size(); index++) {
          if (changed[index]) {
            changed_pixels++;
            restarted_pixels += result.sample_count[index] <= 1.0f ? 1 : 0;
          }
        }

        first_new_error = mean_difference(changed_floor, result.denoised, result.radiance);
        old_error = mean_difference(changed_floor, old_denoised, result.radiance);
      }

      counts.push_back(mean_scalar(changed, result.sample_count));
      unchanged_counts.push_back(mean_scalar(unchanged, result.sample_count));
    }
  ));

  std::fprintf(
    stderr,
    "disocclusion: %zu changed pixels, %zu restarted at 1, changed sample count after the change %.2f %.2f %.2f %.2f "
    "%.2f %.2f, unchanged %.2f %.2f, new frame vs raw %.4f, old history vs raw %.4f\n",
    changed_pixels,
    restarted_pixels,
    counts[0],
    counts[1],
    counts[2],
    counts[3],
    counts[4],
    counts[5],
    unchanged_counts[0],
    unchanged_counts[5],
    first_new_error,
    old_error
  );
  EXPECT_GT(changed_pixels, 3000u);
  EXPECT_GE(restarted_pixels, changed_pixels * 97 / 100);
  EXPECT_NEAR(counts[1], counts[0] + 1.0, 0.2);
  EXPECT_NEAR(counts[5], counts[0] + 5.0, 0.4);
  EXPECT_GT(unchanged_counts[0], 20.0);
  EXPECT_GT(old_error, 0.02);
  EXPECT_LT(first_new_error, 0.5 * old_error);
}

TEST_F(SSSRGPU, JitterDoesNotDisturbTheConvergedSignal) {
  const auto glossy = glossy_scene();
  const auto mirror = mirror_scene();
  struct Measurement {
    f64 change = 0.0;
    f64 samples = 0.0;
    PositionError mirror = {};
  };
  const auto measure_mirror = [&](const std::function<glm::vec2(u32 frame)>& jitter_at) -> PositionError {
    const auto camera_at = [&](u32 frame) -> TestCamera {
      return floor_camera(jitter_at(frame));
    };
    auto session = SSSRSession(*device);
    EXPECT_TRUE(session.initialize());

    auto error = PositionError{};
    auto previous_camera = camera_at(0);
    constexpr auto frames = 16_u32;
    EXPECT_TRUE(run_frames(
      session,
      frames,
      [&](u32 frame) -> CaseConfig {
        return {
          .scene = &mirror,
          .camera = camera_at(frame),
          .previous_camera = &previous_camera,
          .samples_per_quad = 1,
          .depth_thickness = 0.25f,
          .lit = x_gradient_lit,
        };
      },
      [&](u32 frame, CaseResult& result) -> void {
        const auto current = camera_at(frame);
        previous_camera = current;
        if (frame == frames - 1) {
          const auto hits = erode_hits(collect_box_hits(mirror, current, result.primary), 12);
          error = decode_x_error(current, hits, result.denoised, result.confidence);
        }
      }
    ));
    return error;
  };
  const auto measure = [&](f32 amplitude) -> Measurement {
    auto measurement = Measurement{};
    const auto camera_at = [&](u32 frame) -> TestCamera {
      return floor_camera(jitter_for_frame(frame, amplitude));
    };
    {
      auto session = SSSRSession(*device);
      EXPECT_TRUE(session.initialize());

      auto previous_camera = camera_at(0);
      auto mask = std::vector<u8>{};
      auto previous = std::vector<glm::vec4>{};
      constexpr auto frames = 32_u32;
      EXPECT_TRUE(run_frames(
        session,
        frames,
        [&](u32 frame) -> CaseConfig {
          return {
            .scene = &glossy,
            .camera = camera_at(frame),
            .previous_camera = &previous_camera,
            .samples_per_quad = 1,
            .depth_thickness = 0.25f,
            .lit = checker_lit,
          };
        },
        [&](u32 frame, CaseResult& result) -> void {
          if (frame == 0) {
            mask = make_mask(collect_box_hits(glossy, camera_at(frame), result.primary));
          }

          if (frame == frames - 1) {
            measurement.change = mean_difference(mask, result.denoised, previous);
            measurement.samples = mean_scalar(mask, result.sample_count);
          }

          previous = result.denoised;
          previous_camera = camera_at(frame);
        }
      ));
    }

    measurement.mirror = measure_mirror([&](u32 frame) -> glm::vec2 { return jitter_for_frame(frame, amplitude); });
    return measurement;
  };

  const auto steady = measure(0.0f);
  const auto jittered = measure(1.0f);
  // a zero mean sequence turns a wrong jitter term into a blur, which a linear gradient hides, a fixed offset makes
  // the same mistake accumulate in the history every frame
  const auto offset = measure_mirror([](u32) -> glm::vec2 { return {0.45f, -0.35f}; });
  std::fprintf(
    stderr,
    "jitter: converged change %.5f without, %.5f with (x%.2f), sample count %.2f without, %.2f with, mirror position "
    "rms %.3f without, %.3f with (worst %.2f), %.3f with a fixed offset (mean %.3f, worst %.2f)\n",
    steady.change,
    jittered.change,
    jittered.change / steady.change,
    steady.samples,
    jittered.samples,
    steady.mirror.rms,
    jittered.mirror.rms,
    jittered.mirror.worst,
    offset.rms,
    offset.mean,
    offset.worst
  );
  EXPECT_LT(jittered.change, 2.0 * steady.change);
  EXPECT_GT(jittered.samples, 0.9 * steady.samples);
  EXPECT_LT(jittered.mirror.rms, 1.0);
  EXPECT_LT(jittered.mirror.worst, 1.5);
  EXPECT_LT(offset.rms, steady.mirror.rms + 0.25);
  EXPECT_LT(std::abs(offset.mean - steady.mirror.mean), 0.25);
}

struct FrameDifference {
  f64 mean = 0.0;
  f64 outlier_ratio = 0.0;
  f32 worst_count = 0.0f;
};

static auto compare_frames(const CaseResult& a, const CaseResult& b) -> FrameDifference {
  auto difference = FrameDifference{};
  auto outliers = 0_u32;
  for (auto index = 0_sz; index < a.denoised.size(); index++) {
    const auto delta = glm::compMax(glm::abs(a.denoised[index] - b.denoised[index]));
    difference.mean += delta;
    outliers += delta > 1e-2f ? 1 : 0;
    difference.worst_count = std::max(difference.worst_count, std::abs(a.sample_count[index] - b.sample_count[index]));
  }

  difference.mean /= static_cast<f64>(a.denoised.size());
  difference.outlier_ratio = static_cast<f64>(outliers) / static_cast<f64>(a.denoised.size());
  return difference;
}

TEST_F(SSSRGPU, ResetClearsTheHistoryAndMatchesAFirstFrame) {
  const auto scene = glossy_scene();
  const auto camera = floor_camera();
  const auto config = CaseConfig{
    .scene = &scene,
    .camera = camera,
    .samples_per_quad = 1,
    .depth_thickness = 0.25f,
    .lit = checker_lit,
  };

  auto fresh = SSSRSession(*device);
  ASSERT_TRUE(fresh.initialize());
  const auto first = fresh.step(config);
  ASSERT_TRUE(first.is_valid);

  auto repeated = SSSRSession(*device);
  ASSERT_TRUE(repeated.initialize());
  const auto repeat = repeated.step(config);
  ASSERT_TRUE(repeat.is_valid);

  auto reset_config = config;
  reset_config.is_reset = true;
  auto session = SSSRSession(*device);
  ASSERT_TRUE(session.initialize());
  for (auto frame = 0_u32; frame < 12; frame++) {
    ASSERT_TRUE(session.step(config).is_valid);
  }

  session.frame_index = 0;
  const auto reset = session.step(reset_config);
  ASSERT_TRUE(reset.is_valid);

  auto continued_session = SSSRSession(*device);
  ASSERT_TRUE(continued_session.initialize());
  for (auto frame = 0_u32; frame < 12; frame++) {
    ASSERT_TRUE(continued_session.step(config).is_valid);
  }

  continued_session.frame_index = 0;
  const auto continued = continued_session.step(config);
  ASSERT_TRUE(continued.is_valid);

  const auto baseline = compare_frames(first, repeat);
  const auto after_reset = compare_frames(first, reset);
  const auto without_reset = compare_frames(first, continued);
  auto continued_count = 0.0f;
  for (const auto count : continued.sample_count) {
    continued_count = std::max(continued_count, count);
  }

  std::fprintf(
    stderr,
    "reset: against a first frame mean %.6f outliers %.5f sample count %.1f, two fresh frames mean %.6f outliers "
    "%.5f, without the reset mean %.6f outliers %.5f and the sample count reaches %.1f\n",
    after_reset.mean,
    after_reset.outlier_ratio,
    after_reset.worst_count,
    baseline.mean,
    baseline.outlier_ratio,
    without_reset.mean,
    without_reset.outlier_ratio,
    continued_count
  );
  EXPECT_EQ(after_reset.worst_count, 0.0f);
  EXPECT_LT(after_reset.mean, baseline.mean + 1e-4);
  EXPECT_LT(after_reset.outlier_ratio, baseline.outlier_ratio + 1e-3);
  EXPECT_GT(without_reset.mean, 10.0 * (after_reset.mean + 1e-5));
  EXPECT_GT(continued_count, 10.0f);
}

TEST_F(SSSRGPU, SkyFallbackFollowsWhatTheRayLastCrossed) {
  const auto scene = mirror_scene();
  const auto camera = floor_camera();
  const auto result = run_case(
    *device,
    {.scene = &scene,
     .camera = camera,
     .samples_per_quad = 4,
     .depth_thickness = 0.25f,
     .environment = TestEnvironment::DDGISky}
  );
  ASSERT_TRUE(result.is_valid);

  auto sky_exits = 0_u32;
  auto sky_correct = 0_u32;
  auto geometry_exits = 0_u32;
  auto geometry_correct = 0_u32;
  for (auto y = 0_u32; y < TEST_SIZE; y++) {
    for (auto x = 0_u32; x < TEST_SIZE; x++) {
      const auto index = pixel_index(x, y);
      const auto& primary = result.primary[index];
      if (primary.object != 0) {
        continue;
      }

      const auto pixel_center = glm::vec2(x + 0.5f, y + 0.5f);
      const auto reflected = glm::reflect(camera_ray(camera, pixel_center), primary.normal);
      if (scene.trace(primary.position + primary.normal * 1e-3f, reflected).object >= 0) {
        continue;
      }

      auto crossed = std::array<i32, 6>{};
      auto crossed_count = 0_sz;
      auto exit = glm::vec2(0.0f);
      auto has_exit = false;
      for (auto distance = 0.01f; distance < 5000.0f; distance *= 1.01f) {
        const auto point = primary.position + reflected * distance;
        const auto clip = camera.gpu.projection_view * glm::vec4(point, 1.0f);
        if (clip.w <= 0.0f) {
          break;
        }

        const auto projected = project_to_pixel(camera, point);
        if (projected.x < 0.0f || projected.y < 0.0f || projected.x >= TEST_SIZE || projected.y >= TEST_SIZE) {
          exit = glm::vec2(projected);
          has_exit = true;
          break;
        }

        crossed[crossed_count++ % crossed.size()] =
          result.primary[pixel_index(static_cast<u32>(projected.x), static_cast<u32>(projected.y))].object;
      }

      if (!has_exit || crossed_count < crossed.size()) {
        continue;
      }

      const auto is_all = [&](auto predicate) -> bool {
        return std::ranges::all_of(crossed, predicate);
      };
      const auto color = glm::vec3(result.radiance[index]);
      if (exit.y < 0.0f && is_all([](i32 object) -> bool { return object < 0; })) {
        sky_exits++;
        sky_correct += glm::distance(color, SKY_VIEW_COLOR) < 5e-3f ? 1 : 0;
      } else if ((exit.x < 0.0f || exit.x >= TEST_SIZE) && is_all([](i32 object) -> bool { return object >= 0; })) {
        geometry_exits++;
        geometry_correct += glm::distance(color, DDGI_AMBIENT_COLOR) < 5e-3f ? 1 : 0;
      }
    }
  }

  std::fprintf(
    stderr,
    "sky fallback: %u rays left through the top over sky, %u returned the LUT colour, %u rays left the side over "
    "geometry, %u returned the DDGI colour\n",
    sky_exits,
    sky_correct,
    geometry_exits,
    geometry_correct
  );
  EXPECT_GT(sky_exits, 500u);
  EXPECT_GT(geometry_exits, 100u);
  EXPECT_GE(sky_correct, sky_exits * 99 / 100);
  EXPECT_GE(geometry_correct, geometry_exits * 99 / 100);
}

TEST_F(SSSRGPU, PixelsTheDenoiserSkipsKeepTheirRawValuesThroughTheChain) {
  auto scene = mirror_scene();
  scene.planes[0].roughness = 0.2f;
  const auto camera = floor_camera();
  auto session = SSSRSession(*device);
  ASSERT_TRUE(session.initialize());

  auto last = CaseResult{};
  constexpr auto frames = 6_u32;
  ASSERT_TRUE(run_frames(
    session,
    frames,
    [&](u32) -> CaseConfig {
      return {.scene = &scene, .camera = camera, .samples_per_quad = 1, .fixed_exposure = 0.25f};
    },
    [&](u32 frame, CaseResult& result) -> void {
      if (frame == frames - 1) {
        last = std::move(result);
      }
    }
  ));

  struct Tally {
    u32 count = 0;
    u32 failures = 0;
    f32 worst = 0.0f;
  };
  auto rough_in_tile = Tally{};
  auto rough_outside = Tally{};
  auto sky_in_tile = Tally{};
  auto sky_outside = Tally{};
  for (auto tile_y = 0_u32; tile_y < TEST_SIZE / 8; tile_y++) {
    for (auto tile_x = 0_u32; tile_x < TEST_SIZE / 8; tile_x++) {
      auto is_in_tile = false;
      for (auto index = 0_u32; index < 64; index++) {
        const auto pixel = pixel_index(tile_x * 8 + index % 8, tile_y * 8 + index / 8);
        is_in_tile = is_in_tile || (last.primary[pixel].object >= 0 && last.surface_roughness[pixel] < 0.4f);
      }

      for (auto index = 0_u32; index < 64; index++) {
        const auto pixel = pixel_index(tile_x * 8 + index % 8, tile_y * 8 + index / 8);
        const auto is_sky = last.primary[pixel].object < 0;
        if (!is_sky && last.surface_roughness[pixel] < 0.4f) {
          continue;
        }

        auto& tally = is_sky ? (is_in_tile ? sky_in_tile : sky_outside) : (is_in_tile ? rough_in_tile : rough_outside);
        const auto expected = is_sky ? glm::vec3(0.0f) : SENTINEL_AMBIENT;
        const auto error = glm::distance(glm::vec3(last.denoised[pixel]), expected);
        tally.count++;
        tally.worst = std::max(tally.worst, error);
        tally.failures += error > 3e-3f ? 1 : 0;
      }
    }
  }

  std::fprintf(
    stderr,
    "skipped pixels: rough in a denoised tile %u (worst error %.5f), rough outside %u (%.5f), sky in a denoised tile "
    "%u (%.5f), sky outside %u (%.5f)\n",
    rough_in_tile.count,
    rough_in_tile.worst,
    rough_outside.count,
    rough_outside.worst,
    sky_in_tile.count,
    sky_in_tile.worst,
    sky_outside.count,
    sky_outside.worst
  );
  for (const auto& tally : {rough_in_tile, rough_outside, sky_in_tile, sky_outside}) {
    EXPECT_GT(tally.count, 100u);
    EXPECT_EQ(tally.failures, 0u);
  }
}
} // namespace ox
