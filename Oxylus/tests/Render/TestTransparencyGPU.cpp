#include <SDL3/SDL_filesystem.h>
#include <VkBootstrap.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>
#include <gtest/gtest.h>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <vir/Allocator.hpp>
#include <vir/Context.hpp>
#include <vir/RenderGraph.hpp>

#ifdef OX_PLATFORM_LINUX
  #include <dlfcn.h>
  #include <link.h>
#endif

#include "Asset/AssetFile.hpp"
#include "Core/Base.hpp"
#include "Render/GPU/Shared.hpp"
#include "Render/RenderContext.hpp"
#include "Render/RenderResources.hpp"
#include "Render/RendererInstance.hpp"

namespace ox {
static VKAPI_ATTR auto VKAPI_CALL transparency_validation_message(
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

static auto retain_transparency_driver_modules() -> void {
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

// the build puts the pack next to the test binary, which may not be where it was started from
static auto transparency_gpu_pack() -> option<AssetFile> {
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

struct TransparencyTestDirectory {
  std::filesystem::path previous = std::filesystem::current_path();
  std::filesystem::path temporary = std::filesystem::temp_directory_path() /
                                    ("oxylus-transparency-gpu-" +
                                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

  TransparencyTestDirectory() {
    std::filesystem::create_directories(temporary);
    std::filesystem::current_path(temporary);
  }
  ~TransparencyTestDirectory() {
    std::filesystem::current_path(previous);
    std::filesystem::remove_all(temporary);
  }
};

struct WboitLayer {
  glm::vec3 color = {};
  f32 alpha = 0.0f;
  f32 distance = 0.0f;
};

struct WboitPixel {
  glm::vec4 accumulation = glm::vec4(0.0f);
  f32 revealage = 1.0f;
};

static auto wboit_weight(f32 distance) -> f32 {
  return std::clamp(10.0f / (1e-5f + std::pow(distance / 5.0f, 2.0f) + std::pow(distance / 200.0f, 6.0f)), 1e-2f, 3e3f);
}

static auto to_half(f32 value) -> f32 { return glm::unpackHalf1x16(glm::packHalf1x16(value)); }

static auto to_unorm8(f32 value) -> f32 { return std::round(std::clamp(value, 0.0f, 1.0f) * 255.0f) / 255.0f; }

// blending adds into fp16 and multiplies into r8 one layer at a time, so the reference rounds the same way
static auto wboit_accumulate(std::span<const WboitLayer> layers) -> WboitPixel {
  auto pixel = WboitPixel{};
  for (const auto& layer : layers) {
    const auto term = glm::vec4(layer.color * layer.alpha, layer.alpha) * wboit_weight(layer.distance);
    for (auto channel = 0; channel < 4; channel++) {
      pixel.accumulation[channel] = to_half(pixel.accumulation[channel] + to_half(term[channel]));
    }

    pixel.revealage = to_unorm8(pixel.revealage * (1.0f - layer.alpha));
  }

  return pixel;
}

static auto wboit_composite(const WboitPixel& pixel, const glm::vec4& background) -> glm::vec4 {
  if (pixel.revealage >= 1.0f) {
    return background;
  }

  const auto coverage = 1.0f - pixel.revealage;
  const auto average = glm::vec3(pixel.accumulation) / std::max(pixel.accumulation.a, 1e-5f);
  return glm::vec4(average * coverage, coverage) + background * (1.0f - coverage);
}

constexpr static auto TEST_MESHLET_COUNTS = std::array{3_u32, 5_u32};
constexpr static auto TEST_INSTANCE_COUNT = 200_u32;
constexpr static auto TEST_MESHLETS_PER_GROUP = 64_u32;

struct CullResults {
  std::vector<GPU::MeshletInstance> opaque = {};
  std::vector<GPU::MeshletInstance> transparent = {};
  std::vector<GPU::MeshInstance> mesh_instances = {};
  GPU::MeshletInstanceVisibility visibility = {};
  std::array<VkDispatchIndirectCommand, 3> commands = {};
  std::array<std::vector<u32>, 2> visible_indices = {};
  std::array<u32, 2> triangle_commands = {};
};

class TransparencyGPUTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto builder = vkb::InstanceBuilder{};
    builder.set_headless().require_api_version(1, 3, 0).enable_extension(VK_KHR_SURFACE_EXTENSION_NAME);
    if (std::getenv("OX_TRANSPARENCY_GPU_VALIDATION")) {
      builder.request_validation_layers()
        .set_debug_callback(transparency_validation_message)
        .set_debug_callback_user_data_pointer(&validation_errors)
        .add_validation_feature_enable(VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT);
    }

    const auto instance = builder.build();
    if (!instance) {
      GTEST_SKIP() << "headless Vulkan instance unavailable: " << instance.error().message();
    }
    vkb_instance = *instance;

    auto features = VkPhysicalDeviceFeatures{};
    features.shaderInt64 = VK_TRUE;
    features.shaderInt16 = VK_TRUE;
    features.independentBlend = VK_TRUE;
    features.shaderStorageImageReadWithoutFormat = VK_TRUE;
    features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    auto features11 = VkPhysicalDeviceVulkan11Features{};
    features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    features11.storageBuffer16BitAccess = VK_TRUE;
    features11.uniformAndStorageBuffer16BitAccess = VK_TRUE;
    features11.shaderDrawParameters = VK_TRUE;
    auto features12 = VkPhysicalDeviceVulkan12Features{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.timelineSemaphore = VK_TRUE;
    features12.shaderInt8 = VK_TRUE;
    features12.storageBuffer8BitAccess = VK_TRUE;
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
    const auto physical = vkb::PhysicalDeviceSelector(vkb_instance)
                            .require_present(false)
                            .add_required_extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME)
                            .set_minimum_version(1, 3)
                            .set_required_features(features)
                            .set_required_features_11(features11)
                            .set_required_features_12(features12)
                            .set_required_features_13(features13)
                            .select();
    if (!physical) {
      GTEST_SKIP() << "no Vulkan device supports the culling shaders: " << physical.error().message();
    }

    const auto device = vkb::DeviceBuilder(*physical).build();
    ASSERT_TRUE(device) << device.error().message();
    vkb_device = *device;

    auto context = vir::Context::create(
      {.instance = vkb_instance.instance,
       .physical_device = physical->physical_device,
       .device = vkb_device.device,
       .get_instance_proc_addr = vkb_instance.fp_vkGetInstanceProcAddr}
    );
    ASSERT_TRUE(context);
    const auto queue = vkb_device.get_queue_index(vkb::QueueType::graphics);
    ASSERT_TRUE(queue);
    ASSERT_TRUE((*context)->create_command_queue(*queue, vir::DomainFlag::Graphics));

    directory = std::make_unique<TransparencyTestDirectory>();
    render = std::make_unique<RenderContext>();
    ASSERT_TRUE(render->initialize_graph(std::move(*context)));

    const auto sampler_binding = std::array{VkDescriptorSetLayoutBinding{
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_ALL,
      .pImmutableSamplers = nullptr
    }};
    auto sampler_set = render->create_persistent_descriptor_set(
      sampler_binding,
      std::array<VkDescriptorBindingFlags, 1>{0}
    );
    ASSERT_TRUE(sampler_set);
    render->resources->descriptor_set = std::move(*sampler_set);

    const auto pack = transparency_gpu_pack();
    ASSERT_TRUE(pack) << "engine.oxpack must be built before this test";
    for (const auto& entry : pack->entries) {
      const auto* shader = std::get_if<ShaderPipelineData>(&entry.data);
      if (
        shader && (shader->module_name == "cull_meshes" || shader->module_name == "cull_meshlets_hiz" ||
                   shader->module_name == "cull_triangles" || shader->module_name == "wboit_resolve")
      ) {
        ASSERT_TRUE(render->create_pipeline(*shader));
      }
    }
    ASSERT_NE(render->pipeline("cull_meshes"), vir::PipelineID::Invalid);
    ASSERT_NE(render->pipeline("cull_meshlets_hiz"), vir::PipelineID::Invalid);
    ASSERT_NE(render->pipeline("cull_triangles"), vir::PipelineID::Invalid);
    ASSERT_NE(render->pipeline("wboit_resolve"), vir::PipelineID::Invalid);

    build_scene();
  }

  void TearDown() override {
    if (render) {
      for (const auto& buffer : buffers) {
        render->persistent->deallocate_buffer(buffer);
      }

      buffers.clear();
      render->seal_retirements({});
      render->collect_retired();
    }

    EXPECT_EQ(validation_errors.load(std::memory_order_relaxed), 0u);
    render.reset();
    directory.reset();
    if (vkb_device.device != VK_NULL_HANDLE) {
      retain_transparency_driver_modules();
      vkb::destroy_device(vkb_device);
    }

    if (vkb_instance.instance != VK_NULL_HANDLE) {
      vkb::destroy_instance(vkb_instance);
    }
  }

  auto allocate(u64 size) -> vir::Buffer {
    const auto buffer = render->persistent->allocate_buffer({
      .size = std::max<u64>(size, 16),
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      .location = vir::MemoryLocation::CpuToGpu,
    });
    EXPECT_TRUE(buffer);
    if (!buffer) {
      return {};
    }

    buffers.push_back(*buffer);
    return *buffer;
  }

  template <typename T>
  auto upload(std::span<const T> data) -> vir::Buffer {
    auto buffer = allocate(data.size_bytes());
    EXPECT_TRUE(buffer.template write<T>(0, data));
    EXPECT_TRUE(render->persistent->flush_buffer(buffer));
    return buffer;
  }

  template <typename T>
  auto read(const vir::Buffer& buffer, usize count) -> std::vector<T> {
    EXPECT_TRUE(render->persistent->invalidate_buffer(buffer));
    const auto* data = reinterpret_cast<const T*>(buffer.mapped);
    return std::vector<T>(data, data + count);
  }

  template <typename T>
  auto fill(const vir::Buffer& buffer, usize count, const T& value = {}) -> void {
    const auto zeros = std::vector<T>(count, value);
    EXPECT_TRUE(buffer.template write<T>(0, zeros));
    EXPECT_TRUE(render->persistent->flush_buffer(buffer));
  }

  auto build_scene() -> void {
    auto bounds = std::array<GPU::MeshletBounds, 8>{};
    for (auto& meshlet_bounds : bounds) {
      meshlet_bounds.aabb_center = {glm::packHalf1x16(0.0f), glm::packHalf1x16(0.0f), glm::packHalf1x16(0.0f)};
      meshlet_bounds.aabb_extent = {glm::packHalf1x16(1.0f), glm::packHalf1x16(1.0f), glm::packHalf1x16(1.0f)};
      // a cutoff of 1.0 skips the cone test
      meshlet_bounds.cone_cutoff = 127;
    }
    bounds_buffer = upload<GPU::MeshletBounds>(bounds);

    auto lods = std::array<GPU::MeshLOD, TEST_MESHLET_COUNTS.size()>{};
    auto meshes = std::array<GPU::Mesh, TEST_MESHLET_COUNTS.size()>{};
    lod_buffer = allocate(sizeof(lods));
    for (auto index = 0_sz; index < lods.size(); index++) {
      lods[index].meshlet_count = TEST_MESHLET_COUNTS[index];
      lods[index].meshlet_bounds = bounds_buffer.device_address;
      lods[index].meshlet_bounds_count = TEST_MESHLET_COUNTS[index];
      meshes[index].lod_count = 1;
      meshes[index].lods = lod_buffer.device_address + index * sizeof(GPU::MeshLOD);
      meshes[index].bounds = {.aabb_center = glm::vec3(0.0f), .aabb_extent = glm::vec3(1.0f)};
    }
    EXPECT_TRUE(lod_buffer.write<GPU::MeshLOD>(0, lods));
    EXPECT_TRUE(render->persistent->flush_buffer(lod_buffer));
    meshes_buffer = upload<GPU::Mesh>(meshes);

    auto materials = std::array<GPU::Material, 3>{};
    materials[0].flags = GPU::MaterialFlag::AlphaOpaque;
    materials[1].flags = GPU::MaterialFlag::AlphaBlend;
    materials[2].flags = GPU::MaterialFlag::AlphaOpaque | GPU::MaterialFlag::Water;
    materials_buffer = upload<GPU::Material>(materials);

    auto transforms = std::vector<GPU::TransformWorld>(TEST_INSTANCE_COUNT);
    mesh_instances.resize(TEST_INSTANCE_COUNT);
    for (auto index = 0_u32; index < TEST_INSTANCE_COUNT; index++) {
      const auto position = glm::vec3(static_cast<f32>(index % 10) * 2.0f, static_cast<f32>(index / 10), 10.0f);
      transforms[index].world = glm::translate(glm::mat4(1.0f), position);
      mesh_instances[index] = {
        .mesh_index = index % 2,
        .material_index = (index / 2) % 3,
        .transform_index = index,
        .flags = index % 7 == 0 ? GPU::MeshInstanceFlag::None : GPU::MeshInstanceFlag::CastShadows,
      };
      capacity += TEST_MESHLET_COUNTS[mesh_instances[index].mesh_index];
    }
    transforms_buffer = upload<GPU::TransformWorld>(transforms);
  }

  // every world position lands well inside this, and depth stays in front of the near plane
  static auto camera(u32 capacity_value) -> GPU::CullCamera {
    return {
      .projection_view = glm::scale(glm::mat4(1.0f), glm::vec3(0.01f)),
      .near_clip = 0.0f,
      .mesh_instance_count = TEST_INSTANCE_COUNT,
      .meshlet_instance_capacity = capacity_value,
    };
  }

  auto run(GPU::CullFlag flags, bool is_running_visibility_cull) -> CullResults {
    auto mesh_instances_buffer = upload<GPU::MeshInstance>(mesh_instances);
    auto meshlet_instances_buffer = allocate(capacity * sizeof(GPU::MeshletInstance));
    auto transparent_buffer = allocate(capacity * sizeof(GPU::MeshletInstance));
    auto visibility_buffer = allocate(sizeof(GPU::MeshletInstanceVisibility));
    auto commands_buffer = std::array{allocate(16), allocate(16), allocate(16)};
    auto visible_buffers = std::array{allocate(capacity * sizeof(u32)), allocate(capacity * sizeof(u32))};
    auto triangle_commands_buffer = std::array{allocate(16), allocate(16)};
    auto mask_buffer = allocate(16);
    fill<GPU::MeshletInstance>(meshlet_instances_buffer, capacity);
    fill<GPU::MeshletInstance>(transparent_buffer, capacity);
    fill<GPU::MeshletInstanceVisibility>(visibility_buffer, 1);
    for (const auto& command : commands_buffer) {
      fill<VkDispatchIndirectCommand>(command, 1, {.x = 0, .y = 1, .z = 1});
    }
    for (const auto& command : triangle_commands_buffer) {
      fill<VkDispatchIndirectCommand>(command, 1, {.x = 0, .y = 1, .z = 1});
    }

    auto module = vir::Module{};
    auto meshes = module.import_buffer(meshes_buffer, vir::Access::HostWrite);
    auto transforms = module.import_buffer(transforms_buffer, vir::Access::HostWrite);
    auto instances = module.import_buffer(mesh_instances_buffer, vir::Access::HostWrite);
    auto opaque = module.import_buffer(meshlet_instances_buffer, vir::Access::HostWrite);
    auto visibility = module.import_buffer(visibility_buffer, vir::Access::HostWrite);
    auto opaque_command = module.import_buffer(commands_buffer[0], vir::Access::HostWrite);
    auto materials = module.import_buffer(materials_buffer, vir::Access::HostWrite);
    auto transparent = module.import_buffer(transparent_buffer, vir::Access::HostWrite);
    auto blend_command = module.import_buffer(commands_buffer[1], vir::Access::HostWrite);
    auto refractor_command = module.import_buffer(commands_buffer[2], vir::Access::HostWrite);

    const auto cull_camera = camera(capacity);
    std::tie(
      meshes,
      transforms,
      instances,
      opaque,
      visibility,
      opaque_command,
      materials,
      transparent,
      blend_command,
      refractor_command
    ) = std::tuple_cat( //
      module
        .begin_compute(
          {{meshes, vir::Access::ComputeRead},
           {transforms, vir::Access::ComputeRead},
           {instances, vir::Access::ComputeRW},
           {opaque, vir::Access::ComputeRW},
           {visibility, vir::Access::ComputeRW},
           {opaque_command, vir::Access::ComputeRW},
           {materials, vir::Access::ComputeRead},
           {transparent, vir::Access::ComputeWrite},
           {blend_command, vir::Access::ComputeRW},
           {refractor_command, vir::Access::ComputeRW}}
        )
        .with_name("cull meshes")
        .bind_compute_pipeline(render->pipeline("cull_meshes"))
        .bind_buffer(0, 0, meshes)
        .bind_buffer(0, 1, transforms)
        .bind_buffer(0, 2, instances)
        .bind_buffer(0, 3, opaque)
        .bind_buffer(0, 4, visibility)
        .bind_buffer(0, 5, opaque_command)
        .bind_buffer(0, 6, materials)
        .bind_buffer(0, 7, transparent)
        .bind_buffer(0, 8, blend_command)
        .bind_buffer(0, 9, refractor_command)
        .specialize_constant(0, std::to_underlying(flags))
        .push_constants(cull_camera)
        .dispatch((TEST_INSTANCE_COUNT + 63) / 64, 1, 1)
        .end_compute()
    );

    auto visible = std::array<vir::ValueID, 2>{};
    auto triangle_commands = std::array<vir::ValueID, 2>{};
    if (is_running_visibility_cull) {
      auto hiz = module.transient_image(
        {.extent = {4, 4, 1}, .format = vir::Format::R32Sfloat, .name = "transparency hiz"}
      );
      hiz = module.clear(hiz, vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 0.0f));
      auto mask = module.import_buffer(mask_buffer, vir::Access::HostWrite);
      const auto lists = std::array{blend_command, refractor_command};
      for (auto list = 0_sz; list < 2; list++) {
        visible[list] = module.import_buffer(visible_buffers[list], vir::Access::HostWrite);
        triangle_commands[list] = module.import_buffer(triangle_commands_buffer[list], vir::Access::HostWrite);
        const auto list_flag = list == 1 ? GPU::CullFlag::RefractorList : GPU::CullFlag::None;
        const auto cull_flags = GPU::CullFlag::Transparent | GPU::CullFlag::TestFrustum | list_flag;
        auto list_command = lists[list];
        std::tie(
          list_command,
          hiz,
          meshes,
          instances,
          transparent,
          transforms,
          visibility,
          visible[list],
          mask,
          triangle_commands[list],
          materials
        ) = std::tuple_cat( //
          module
            .begin_compute(
              {{list_command, vir::Access::IndirectRead},
               {hiz, vir::Access::ComputeSampled},
               {meshes, vir::Access::ComputeRead},
               {instances, vir::Access::ComputeRead},
               {transparent, vir::Access::ComputeRead},
               {transforms, vir::Access::ComputeRead},
               {visibility, vir::Access::ComputeRW},
               {visible[list], vir::Access::ComputeRW},
               {mask, vir::Access::ComputeRW},
               {triangle_commands[list], vir::Access::ComputeRW},
               {materials, vir::Access::ComputeRead}}
            )
            .with_name(list == 1 ? "cull refractor meshlets" : "cull blend meshlets")
            .bind_compute_pipeline(render->pipeline("cull_meshlets_hiz"))
            .bind_image(0, 0, hiz)
            .bind_buffer(0, 1, meshes)
            .bind_buffer(0, 2, instances)
            .bind_buffer(0, 3, transparent)
            .bind_buffer(0, 4, transforms)
            .bind_buffer(0, 5, visibility)
            .bind_buffer(0, 6, visible[list])
            .bind_buffer(0, 7, mask)
            .bind_buffer(0, 8, triangle_commands[list])
            .bind_buffer(0, 9, materials)
            .specialize_constant(0, std::to_underlying(cull_flags))
            .push_constants(cull_camera)
            .dispatch_indirect(list_command)
            .end_compute()
        );
        (list == 1 ? refractor_command : blend_command) = list_command;
      }
      module.export_to(mask, vir::Access::HostRead);
      for (auto list = 0_sz; list < 2; list++) {
        module.export_to(visible[list], vir::Access::HostRead);
        module.export_to(triangle_commands[list], vir::Access::HostRead);
      }
    }

    module.export_to(instances, vir::Access::HostRead);
    module.export_to(opaque, vir::Access::HostRead);
    module.export_to(visibility, vir::Access::HostRead);
    module.export_to(opaque_command, vir::Access::HostRead);
    module.export_to(transparent, vir::Access::HostRead);
    module.export_to(blend_command, vir::Access::HostRead);
    module.export_to(refractor_command, vir::Access::HostRead);

    const auto program = render->compile(std::move(module));
    EXPECT_TRUE(program);
    if (!program) {
      return {};
    }

    EXPECT_TRUE(render->execute_blocking(*program));

    auto results = CullResults{};
    results.mesh_instances = read<GPU::MeshInstance>(mesh_instances_buffer, TEST_INSTANCE_COUNT);
    results.opaque = read<GPU::MeshletInstance>(meshlet_instances_buffer, capacity);
    results.transparent = read<GPU::MeshletInstance>(transparent_buffer, capacity);
    results.visibility = read<GPU::MeshletInstanceVisibility>(visibility_buffer, 1)[0];
    for (auto index = 0_sz; index < commands_buffer.size(); index++) {
      results.commands[index] = read<VkDispatchIndirectCommand>(commands_buffer[index], 1)[0];
    }

    if (is_running_visibility_cull) {
      results.visible_indices[0] = read<u32>(visible_buffers[0], capacity);
      results.visible_indices[1] = read<u32>(visible_buffers[1], capacity);
      results.triangle_commands[0] = read<VkDispatchIndirectCommand>(triangle_commands_buffer[0], 1)[0].x;
      results.triangle_commands[1] = read<VkDispatchIndirectCommand>(triangle_commands_buffer[1], 1)[0].x;
    }

    return results;
  }

  struct FacingScene {
    vir::Buffer positions = {};
    vir::Buffer normals = {};
    vir::Buffer triangle_indices = {};
    vir::Buffer vertex_indices = {};
    vir::Buffer meshlets = {};
    vir::Buffer bounds = {};
    vir::Buffer lod = {};
    vir::Buffer mesh = {};
    vir::Buffer materials = {};
    vir::Buffer transforms = {};
    vir::Buffer mesh_instances = {};
    vir::Buffer meshlet_instances = {};
    vir::Buffer visibility = {};
  };

  // one meshlet with a counter-clockwise and a clockwise triangle over the same three vertices,
  // and a normal cone pointing along `cone_axis_z`. material 0 is single sided, material 1 double sided
  auto build_facing_scene(i8 cone_axis_z) -> FacingScene {
    auto result = FacingScene{};
    const auto positions = std::array{
      glm::u16vec4(glm::packHalf1x16(0.0f), glm::packHalf1x16(0.0f), glm::packHalf1x16(0.0f), 0),
      glm::u16vec4(glm::packHalf1x16(10.0f), glm::packHalf1x16(0.0f), glm::packHalf1x16(0.0f), 0),
      glm::u16vec4(glm::packHalf1x16(0.0f), glm::packHalf1x16(10.0f), glm::packHalf1x16(0.0f), 0),
    };
    result.positions = upload<glm::u16vec4>(positions);

    // corners 0 1 2 then 0 2 1, packed four bytes to a word
    const auto triangle_indices = std::array{0x00020100_u32, 0x00000102_u32};
    result.triangle_indices = upload<u32>(triangle_indices);
    const auto vertex_indices = std::array{0_u32, 1_u32, 2_u32};
    result.vertex_indices = upload<u32>(vertex_indices);

    const auto meshlets = std::array{GPU::Meshlet{
      .indirect_vertex_index_offset = 0,
      .local_triangle_index_offset = 0,
      .vertex_count = 3,
      .triangle_count = 2
    }};
    result.meshlets = upload<GPU::Meshlet>(meshlets);

    auto bounds = GPU::MeshletBounds{};
    bounds.aabb_center = {glm::packHalf1x16(0.0f), glm::packHalf1x16(0.0f), glm::packHalf1x16(0.0f)};
    bounds.aabb_extent = {glm::packHalf1x16(1.0f), glm::packHalf1x16(1.0f), glm::packHalf1x16(1.0f)};
    bounds.cone_axis_z = cone_axis_z;
    bounds.cone_cutoff = 0;
    result.bounds = upload<GPU::MeshletBounds>(std::array{bounds});

    auto lod = GPU::MeshLOD{};
    lod.indices_count = 0;
    lod.meshlets = result.meshlets.device_address;
    lod.meshlet_bounds = result.bounds.device_address;
    lod.local_triangle_indices = result.triangle_indices.device_address;
    lod.indirect_vertex_indices = result.vertex_indices.device_address;
    lod.meshlet_count = 1;
    lod.meshlet_bounds_count = 1;
    lod.local_triangle_indices_count = 2;
    lod.indirect_vertex_indices_count = 3;
    result.lod = upload<GPU::MeshLOD>(std::array{lod});

    auto mesh = GPU::Mesh{};
    mesh.vertex_positions = result.positions.device_address;
    mesh.vertex_count = 3;
    mesh.lod_count = 1;
    mesh.lods = result.lod.device_address;
    mesh.bounds = {.aabb_center = glm::vec3(0.0f), .aabb_extent = glm::vec3(1.0f)};
    result.mesh = upload<GPU::Mesh>(std::array{mesh});

    auto materials = std::array<GPU::Material, 2>{};
    materials[0].flags = GPU::MaterialFlag::AlphaOpaque;
    materials[1].flags = GPU::MaterialFlag::AlphaOpaque | GPU::MaterialFlag::DoubleSided;
    result.materials = upload<GPU::Material>(materials);

    auto transform = GPU::TransformWorld{.world = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 10.0f))};
    result.transforms = upload<GPU::TransformWorld>(std::array{transform});

    result.mesh_instances = upload<GPU::MeshInstance>(std::array{GPU::MeshInstance{}});
    result.meshlet_instances = upload<GPU::MeshletInstance>(std::array{GPU::MeshletInstance{}});

    auto visibility = GPU::MeshletInstanceVisibility{};
    visibility.total_visible_meshlet_instances = 1;
    result.visibility = upload<GPU::MeshletInstanceVisibility>(std::array{visibility});
    return result;
  }

  auto set_facing_material(const FacingScene& scene, u32 material_index) -> void {
    auto instance = GPU::MeshInstance{};
    instance.material_index = material_index;
    EXPECT_TRUE(scene.mesh_instances.write<GPU::MeshInstance>(0, std::array{instance}));
    EXPECT_TRUE(render->persistent->flush_buffer(scene.mesh_instances));
  }

  // number of indices cull_triangles emitted for the facing scene
  auto run_cull_triangles(u32 material_index) -> u32 {
    auto scene = build_facing_scene(0);
    set_facing_material(scene, material_index);

    auto visible_buffer = upload<u32>(std::array{0_u32});
    auto indices_buffer = allocate(16 * sizeof(u32));
    auto draw_buffer = allocate(sizeof(VkDrawIndexedIndirectCommand));
    fill<VkDrawIndexedIndirectCommand>(
      draw_buffer,
      1,
      {.indexCount = 0, .instanceCount = 1, .firstIndex = 0, .vertexOffset = 0, .firstInstance = 0}
    );

    auto module = vir::Module{};
    auto meshes = module.import_buffer(scene.mesh, vir::Access::HostWrite);
    auto instances = module.import_buffer(scene.mesh_instances, vir::Access::HostWrite);
    auto meshlet_instances = module.import_buffer(scene.meshlet_instances, vir::Access::HostWrite);
    auto visibility = module.import_buffer(scene.visibility, vir::Access::HostWrite);
    auto transforms = module.import_buffer(scene.transforms, vir::Access::HostWrite);
    auto visible = module.import_buffer(visible_buffer, vir::Access::HostWrite);
    auto indices = module.import_buffer(indices_buffer, vir::Access::HostWrite);
    auto draw = module.import_buffer(draw_buffer, vir::Access::HostWrite);
    auto materials = module.import_buffer(scene.materials, vir::Access::HostWrite);

    std::tie(meshes, instances, meshlet_instances, visibility, transforms, visible, indices, draw, materials) =
      module
        .begin_compute(
          {{meshes, vir::Access::ComputeRead},
           {instances, vir::Access::ComputeRead},
           {meshlet_instances, vir::Access::ComputeRead},
           {visibility, vir::Access::ComputeRead},
           {transforms, vir::Access::ComputeRead},
           {visible, vir::Access::ComputeRead},
           {indices, vir::Access::ComputeRW},
           {draw, vir::Access::ComputeRW},
           {materials, vir::Access::ComputeRead}}
        )
        .with_name("cull triangles facing")
        .bind_compute_pipeline(render->pipeline("cull_triangles"))
        .bind_buffer(0, 0, meshes)
        .bind_buffer(0, 1, instances)
        .bind_buffer(0, 2, meshlet_instances)
        .bind_buffer(0, 3, visibility)
        .bind_buffer(0, 4, transforms)
        .bind_buffer(0, 5, visible)
        .bind_buffer(0, 6, indices)
        .bind_buffer(0, 7, draw)
        .bind_buffer(0, 8, materials)
        .specialize_constant(0, std::to_underlying(GPU::CullFlag::None))
        .push_constants(camera(1))
        .dispatch(1, 1, 1)
        .end_compute();
    module.export_to(draw, vir::Access::HostRead);

    const auto program = render->compile(std::move(module));
    EXPECT_TRUE(program);
    if (!program) {
      return ~0_u32;
    }

    EXPECT_TRUE(render->execute_blocking(*program));
    return read<VkDrawIndexedIndirectCommand>(draw_buffer, 1)[0].indexCount;
  }

  // number of meshlets cull_meshlets_hiz sent on to the triangle cull
  auto run_cull_meshlets_hiz(i8 cone_axis_z, u32 material_index) -> u32 {
    auto scene = build_facing_scene(cone_axis_z);
    set_facing_material(scene, material_index);

    auto visible_buffer = upload<u32>(std::array{0_u32});
    auto mask_buffer = upload<u32>(std::array{~0_u32, ~0_u32, ~0_u32, ~0_u32});
    auto command_buffer = allocate(16);
    fill<VkDispatchIndirectCommand>(command_buffer, 1, {.x = 0, .y = 1, .z = 1});

    auto module = vir::Module{};
    auto hiz = module.transient_image({.extent = {4, 4, 1}, .format = vir::Format::R32Sfloat, .name = "facing hiz"});
    hiz = module.clear(hiz, vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 0.0f));
    auto meshes = module.import_buffer(scene.mesh, vir::Access::HostWrite);
    auto instances = module.import_buffer(scene.mesh_instances, vir::Access::HostWrite);
    auto meshlet_instances = module.import_buffer(scene.meshlet_instances, vir::Access::HostWrite);
    auto transforms = module.import_buffer(scene.transforms, vir::Access::HostWrite);
    auto visibility = module.import_buffer(scene.visibility, vir::Access::HostWrite);
    auto visible = module.import_buffer(visible_buffer, vir::Access::HostWrite);
    auto mask = module.import_buffer(mask_buffer, vir::Access::HostWrite);
    auto command = module.import_buffer(command_buffer, vir::Access::HostWrite);
    auto materials = module.import_buffer(scene.materials, vir::Access::HostWrite);

    const auto flags = GPU::CullFlag::TestFrustum | GPU::CullFlag::TestOcclusion;
    std::tie(hiz, meshes, instances, meshlet_instances, transforms, visibility, visible, mask, command, materials) =
      module
        .begin_compute(
          {{hiz, vir::Access::ComputeSampled},
           {meshes, vir::Access::ComputeRead},
           {instances, vir::Access::ComputeRead},
           {meshlet_instances, vir::Access::ComputeRead},
           {transforms, vir::Access::ComputeRead},
           {visibility, vir::Access::ComputeRW},
           {visible, vir::Access::ComputeRW},
           {mask, vir::Access::ComputeRW},
           {command, vir::Access::ComputeRW},
           {materials, vir::Access::ComputeRead}}
        )
        .with_name("cull meshlets facing")
        .bind_compute_pipeline(render->pipeline("cull_meshlets_hiz"))
        .bind_image(0, 0, hiz)
        .bind_buffer(0, 1, meshes)
        .bind_buffer(0, 2, instances)
        .bind_buffer(0, 3, meshlet_instances)
        .bind_buffer(0, 4, transforms)
        .bind_buffer(0, 5, visibility)
        .bind_buffer(0, 6, visible)
        .bind_buffer(0, 7, mask)
        .bind_buffer(0, 8, command)
        .bind_buffer(0, 9, materials)
        .specialize_constant(0, std::to_underlying(flags))
        .push_constants(camera(1))
        .dispatch(1, 1, 1)
        .end_compute();
    module.export_to(command, vir::Access::HostRead);

    const auto program = render->compile(std::move(module));
    EXPECT_TRUE(program);
    if (!program) {
      return ~0_u32;
    }

    EXPECT_TRUE(render->execute_blocking(*program));
    return read<VkDispatchIndirectCommand>(command_buffer, 1)[0].x;
  }

  auto upload_bytes(const void* data, u64 size) -> vir::Buffer {
    const auto buffer = render->persistent->allocate_buffer(
      {.size = size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .location = vir::MemoryLocation::CpuToGpu}
    );
    EXPECT_TRUE(buffer);
    if (!buffer) {
      return {};
    }

    buffers.push_back(*buffer);
    EXPECT_TRUE(buffer->write<u8>(0, std::span(static_cast<const u8*>(data), size)));
    EXPECT_TRUE(render->persistent->flush_buffer(*buffer));
    return *buffer;
  }

  auto readback_buffer(u64 size) -> vir::Buffer {
    const auto buffer = render->persistent->allocate_buffer(
      {.size = size, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .location = vir::MemoryLocation::GpuToCpu}
    );
    EXPECT_TRUE(buffer);
    if (!buffer) {
      return {};
    }

    buffers.push_back(*buffer);
    return *buffer;
  }

  auto class_of(u32 instance_index) const -> GPU::MaterialClass {
    switch (mesh_instances[instance_index].material_index) {
      case 1 : return GPU::MaterialClass::Blend;
      case 2 : return GPU::MaterialClass::Refractor;
      default: return GPU::MaterialClass::Opaque;
    }
  }

  // every meshlet instance the class contributes, as (mesh instance, meshlet) pairs
  auto expected_list(GPU::MaterialClass material_class, bool is_shadow_pass, bool is_routing) const
    -> std::vector<std::pair<u32, u32>> {
    auto result = std::vector<std::pair<u32, u32>>();
    for (auto index = 0_u32; index < TEST_INSTANCE_COUNT; index++) {
      const auto instance_class = class_of(index);
      const auto is_caster = static_cast<bool>(mesh_instances[index].flags & GPU::MeshInstanceFlag::CastShadows);
      if (is_shadow_pass && (!is_caster || instance_class != GPU::MaterialClass::Opaque)) {
        continue;
      }

      const auto list_class = is_routing ? instance_class : GPU::MaterialClass::Opaque;
      if (list_class != material_class) {
        continue;
      }

      for (auto meshlet = 0_u32; meshlet < TEST_MESHLET_COUNTS[mesh_instances[index].mesh_index]; meshlet++) {
        result.emplace_back(index, meshlet);
      }
    }

    return result;
  }

  static auto sorted_pairs(std::span<const GPU::MeshletInstance> list) -> std::vector<std::pair<u32, u32>> {
    auto result = std::vector<std::pair<u32, u32>>();
    for (const auto& entry : list) {
      result.emplace_back(entry.mesh_instance_index, entry.meshlet_index);
    }

    std::ranges::sort(result);
    return result;
  }

  static auto groups_for(usize total) -> u32 {
    return static_cast<u32>((total + TEST_MESHLETS_PER_GROUP - 1) / TEST_MESHLETS_PER_GROUP);
  }

  vkb::Instance vkb_instance = {};
  vkb::Device vkb_device = {};
  std::atomic<u32> validation_errors = {0};
  std::unique_ptr<TransparencyTestDirectory> directory = nullptr;
  std::unique_ptr<RenderContext> render = nullptr;
  std::vector<vir::Buffer> buffers = {};

  vir::Buffer bounds_buffer = {};
  vir::Buffer lod_buffer = {};
  vir::Buffer meshes_buffer = {};
  vir::Buffer materials_buffer = {};
  vir::Buffer transforms_buffer = {};
  std::vector<GPU::MeshInstance> mesh_instances = {};
  u32 capacity = 0;
};

TEST_F(TransparencyGPUTest, RoutesEachClassIntoItsOwnList) {
  const auto results = run(
    GPU::CullFlag::TestFrustum | GPU::CullFlag::RouteBlend | GPU::CullFlag::RouteRefractors,
    false
  );

  const auto opaque = expected_list(GPU::MaterialClass::Opaque, false, true);
  const auto blend = expected_list(GPU::MaterialClass::Blend, false, true);
  const auto refractor = expected_list(GPU::MaterialClass::Refractor, false, true);
  ASSERT_GT(opaque.size(), 0_sz);
  ASSERT_GT(blend.size(), TEST_MESHLETS_PER_GROUP);
  ASSERT_GT(refractor.size(), TEST_MESHLETS_PER_GROUP);
  ASSERT_EQ(opaque.size() + blend.size() + refractor.size(), static_cast<usize>(capacity));

  EXPECT_EQ(results.visibility.total_visible_meshlet_instances, opaque.size());
  EXPECT_EQ(results.visibility.total_blend_meshlet_instances, blend.size());
  EXPECT_EQ(results.visibility.total_refractor_meshlet_instances, refractor.size());
  EXPECT_EQ(results.commands[0].x, groups_for(opaque.size()));
  EXPECT_EQ(results.commands[1].x, groups_for(blend.size()));
  EXPECT_EQ(results.commands[2].x, groups_for(refractor.size()));

  EXPECT_EQ(sorted_pairs(std::span(results.opaque).first(opaque.size())), opaque);
  EXPECT_EQ(sorted_pairs(std::span(results.transparent).first(blend.size())), blend);
  EXPECT_EQ(sorted_pairs(std::span(results.transparent).last(refractor.size())), refractor);

  for (auto index = 0_u32; index < TEST_INSTANCE_COUNT; index++) {
    EXPECT_EQ(results.mesh_instances[index].lod_index, 0u);
  }
}

TEST_F(TransparencyGPUTest, EverythingIsOpaqueWithoutRouting) {
  const auto results = run(GPU::CullFlag::TestFrustum, false);

  EXPECT_EQ(results.visibility.total_visible_meshlet_instances, capacity);
  EXPECT_EQ(results.visibility.total_blend_meshlet_instances, 0u);
  EXPECT_EQ(results.visibility.total_refractor_meshlet_instances, 0u);
  EXPECT_EQ(results.commands[0].x, groups_for(capacity));
  EXPECT_EQ(results.commands[1].x, 0u);
  EXPECT_EQ(results.commands[2].x, 0u);
  EXPECT_EQ(sorted_pairs(results.opaque), expected_list(GPU::MaterialClass::Opaque, false, false));
}

TEST_F(TransparencyGPUTest, ShadowPassesOnlyCountOpaqueCasters) {
  const auto results = run(
    GPU::CullFlag::TestFrustum | GPU::CullFlag::ShadowCastersOnly | GPU::CullFlag::RouteBlend |
      GPU::CullFlag::RouteRefractors,
    false
  );

  const auto opaque = expected_list(GPU::MaterialClass::Opaque, true, true);
  ASSERT_GT(opaque.size(), 0_sz);
  ASSERT_LT(opaque.size(), static_cast<usize>(capacity));
  EXPECT_EQ(results.visibility.total_visible_meshlet_instances, opaque.size());
  EXPECT_EQ(results.visibility.total_blend_meshlet_instances, 0u);
  EXPECT_EQ(results.visibility.total_refractor_meshlet_instances, 0u);
  EXPECT_EQ(results.commands[1].x, 0u);
  EXPECT_EQ(results.commands[2].x, 0u);
  EXPECT_EQ(sorted_pairs(std::span(results.opaque).first(opaque.size())), opaque);
}

TEST_F(TransparencyGPUTest, TransparentMeshletCullKeepsEveryRoutedMeshletInFrustum) {
  const auto results = run(
    GPU::CullFlag::TestFrustum | GPU::CullFlag::RouteBlend | GPU::CullFlag::RouteRefractors,
    true
  );

  const auto blend = expected_list(GPU::MaterialClass::Blend, false, true);
  const auto refractor = expected_list(GPU::MaterialClass::Refractor, false, true);

  EXPECT_EQ(results.visibility.visible_blend_meshlet_instances, blend.size());
  EXPECT_EQ(results.visibility.visible_refractor_meshlet_instances, refractor.size());
  EXPECT_EQ(results.visibility.early_visible_meshlet_instances, 0u);
  EXPECT_EQ(results.visibility.late_visible_meshlet_instances, 0u);
  EXPECT_EQ(results.triangle_commands[0], blend.size());
  EXPECT_EQ(results.triangle_commands[1], refractor.size());

  auto blend_indices = std::vector<u32>(
    results.visible_indices[0].begin(),
    results.visible_indices[0].begin() + blend.size()
  );
  std::ranges::sort(blend_indices);
  for (auto index = 0_sz; index < blend.size(); index++) {
    EXPECT_EQ(blend_indices[index], index);
  }

  auto refractor_indices = std::vector<u32>(
    results.visible_indices[1].begin(),
    results.visible_indices[1].begin() + refractor.size()
  );
  std::ranges::sort(refractor_indices);
  for (auto index = 0_sz; index < refractor.size(); index++) {
    EXPECT_EQ(refractor_indices[index], capacity - refractor.size() + index);
  }
}

TEST_F(TransparencyGPUTest, TriangleCullDropsBackFacesOfSingleSidedMaterials) { EXPECT_EQ(run_cull_triangles(0), 3u); }

TEST_F(TransparencyGPUTest, TriangleCullKeepsBackFacesOfDoubleSidedMaterials) { EXPECT_EQ(run_cull_triangles(1), 6u); }

TEST_F(TransparencyGPUTest, MeshletCullRejectsBackFacingConeOfSingleSidedMaterials) {
  EXPECT_EQ(run_cull_meshlets_hiz(127, 0), 0u);
  EXPECT_EQ(run_cull_meshlets_hiz(-127, 0), 1u);
}

TEST_F(TransparencyGPUTest, MeshletCullKeepsBackFacingConeOfDoubleSidedMaterials) {
  EXPECT_EQ(run_cull_meshlets_hiz(127, 1), 1u);
  EXPECT_EQ(run_cull_meshlets_hiz(-127, 1), 1u);
}

TEST_F(TransparencyGPUTest, ResolveCompositesLayersIndependentOfOrder) {
  const auto layer_a = WboitLayer{.color = glm::vec3(0.8f, 0.2f, 0.1f), .alpha = 0.3f, .distance = 3.0f};
  const auto layer_b = WboitLayer{.color = glm::vec3(0.1f, 0.4f, 0.9f), .alpha = 0.6f, .distance = 10.0f};
  const auto layer_half = WboitLayer{.color = glm::vec3(0.9f, 0.5f, 0.1f), .alpha = 0.5f, .distance = 4.0f};
  const auto layer_opaque = WboitLayer{.color = glm::vec3(0.2f, 0.7f, 0.3f), .alpha = 1.0f, .distance = 4.0f};
  const auto layer_bright = WboitLayer{.color = glm::vec3(12.0f, 6.0f, 3.0f), .alpha = 0.25f, .distance = 20.0f};
  const auto layers = std::array<std::vector<WboitLayer>, 6>{
    std::vector<WboitLayer>{},
    std::vector<WboitLayer>{layer_half},
    std::vector<WboitLayer>{layer_opaque},
    std::vector<WboitLayer>{layer_a, layer_b},
    std::vector<WboitLayer>{layer_b, layer_a},
    std::vector<WboitLayer>{layer_bright},
  };
  constexpr auto pixel_count = layers.size();
  const auto background = glm::vec4(0.25f, 0.5f, 0.75f, 1.0f);

  auto accumulation_data = std::array<u16, pixel_count * 4>{};
  auto revealage_data = std::array<u8, pixel_count>{};
  auto background_data = std::array<u16, pixel_count * 4>{};
  auto pixels = std::array<WboitPixel, pixel_count>{};
  for (auto index = 0_sz; index < pixel_count; index++) {
    pixels[index] = wboit_accumulate(layers[index]);
    for (auto channel = 0; channel < 4; channel++) {
      accumulation_data[index * 4 + channel] = glm::packHalf1x16(pixels[index].accumulation[channel]);
      background_data[index * 4 + channel] = glm::packHalf1x16(background[channel]);
    }

    revealage_data[index] = static_cast<u8>(std::lround(pixels[index].revealage * 255.0f));
  }

  const auto accumulation_upload = upload_bytes(accumulation_data.data(), sizeof(accumulation_data));
  const auto revealage_upload = upload_bytes(revealage_data.data(), sizeof(revealage_data));
  const auto background_upload = upload_bytes(background_data.data(), sizeof(background_data));
  const auto final_readback = readback_buffer(sizeof(background_data));
  const auto reactive_readback = readback_buffer(pixel_count);

  const auto extent = vir::Extent3D{static_cast<u32>(pixel_count), 1, 1};
  auto module = vir::Module{};
  auto context = TransparencyContext{
    .final_attachment = module.transient_image(
      {.extent = extent, .format = vir::Format::RGBA16Sfloat, .name = "final"}
    ),
    .reactive_mask_attachment = module.transient_image(
      {.extent = extent, .format = vir::Format::R8Unorm, .name = "reactive mask"}
    ),
    .accumulation_attachment = module.transient_image(
      {.extent = extent, .format = vir::Format::RGBA16Sfloat, .name = "wboit accumulation"}
    ),
    .revealage_attachment = module.transient_image(
      {.extent = extent, .format = vir::Format::R8Unorm, .name = "wboit revealage"}
    ),
  };
  context.final_attachment = module.copy_buffer_to_image(
    module.import_buffer(background_upload, vir::Access::HostWrite),
    context.final_attachment
  );
  context.reactive_mask_attachment = module.clear(
    context.reactive_mask_attachment,
    vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 0.0f)
  );
  context.accumulation_attachment = module.copy_buffer_to_image(
    module.import_buffer(accumulation_upload, vir::Access::HostWrite),
    context.accumulation_attachment
  );
  context.revealage_attachment = module.copy_buffer_to_image(
    module.import_buffer(revealage_upload, vir::Access::HostWrite),
    context.revealage_attachment
  );

  RendererInstance::resolve_transparency(module, *render, context);

  const auto final_downloaded = module.copy_image_to_buffer(
    context.final_attachment,
    module.import_buffer(final_readback, vir::Access::None)
  );
  const auto reactive_downloaded = module.copy_image_to_buffer(
    context.reactive_mask_attachment,
    module.import_buffer(reactive_readback, vir::Access::None)
  );
  module.export_to(final_downloaded, vir::Access::HostRead);
  module.export_to(reactive_downloaded, vir::Access::HostRead);

  const auto program = render->compile(std::move(module));
  ASSERT_TRUE(program);
  ASSERT_TRUE(render->execute_blocking(*program));

  const auto final_pixels = read<u16>(final_readback, pixel_count * 4);
  const auto reactive_pixels = read<u8>(reactive_readback, pixel_count);
  auto resolved = std::array<glm::vec4, pixel_count>{};
  for (auto index = 0_sz; index < pixel_count; index++) {
    SCOPED_TRACE(index);
    const auto expected = wboit_composite(pixels[index], background);
    for (auto channel = 0; channel < 4; channel++) {
      SCOPED_TRACE(channel);
      resolved[index][channel] = glm::unpackHalf1x16(final_pixels[index * 4 + channel]);
      EXPECT_NEAR(resolved[index][channel], expected[channel], 1e-3f + 2e-3f * std::abs(expected[channel]));
    }

    const auto expected_reactive = std::min(1.0f - pixels[index].revealage, 0.9f);
    EXPECT_NEAR(static_cast<f32>(reactive_pixels[index]), expected_reactive * 255.0f, 1.0f);
  }

  for (auto channel = 0; channel < 4; channel++) {
    EXPECT_EQ(final_pixels[channel], background_data[channel]);
  }

  EXPECT_EQ(reactive_pixels[0], 0);

  for (auto channel = 0; channel < 4; channel++) {
    // each order rounds revealage to an 8 bit step at a different point, so only a step's worth of drift is allowed
    EXPECT_NEAR(resolved[3][channel], resolved[4][channel], 4e-3f + 4e-3f * std::abs(resolved[3][channel]));
  }

  EXPECT_NEAR(resolved[2].r, layer_opaque.color.r, 2e-3f);
  EXPECT_NEAR(resolved[2].g, layer_opaque.color.g, 2e-3f);
  EXPECT_NEAR(resolved[2].b, layer_opaque.color.b, 2e-3f);
}

TEST_F(TransparencyGPUTest, RefractorsRouteWithoutBlend) {
  const auto results = run(GPU::CullFlag::TestFrustum | GPU::CullFlag::RouteRefractors, true);

  auto opaque = expected_list(GPU::MaterialClass::Opaque, false, true);
  const auto blend = expected_list(GPU::MaterialClass::Blend, false, true);
  const auto refractor = expected_list(GPU::MaterialClass::Refractor, false, true);
  opaque.insert(opaque.end(), blend.begin(), blend.end());
  std::ranges::sort(opaque);
  ASSERT_GT(refractor.size(), TEST_MESHLETS_PER_GROUP);

  EXPECT_EQ(results.visibility.total_visible_meshlet_instances, opaque.size());
  EXPECT_EQ(results.visibility.total_blend_meshlet_instances, 0u);
  EXPECT_EQ(results.visibility.total_refractor_meshlet_instances, refractor.size());
  EXPECT_EQ(results.commands[0].x, groups_for(opaque.size()));
  EXPECT_EQ(results.commands[1].x, 0u);
  EXPECT_EQ(results.commands[2].x, groups_for(refractor.size()));
  EXPECT_EQ(sorted_pairs(std::span(results.opaque).first(opaque.size())), opaque);
  EXPECT_EQ(sorted_pairs(std::span(results.transparent).last(refractor.size())), refractor);

  EXPECT_EQ(results.visibility.visible_blend_meshlet_instances, 0u);
  EXPECT_EQ(results.visibility.visible_refractor_meshlet_instances, refractor.size());
  EXPECT_EQ(results.triangle_commands[0], 0u);
  EXPECT_EQ(results.triangle_commands[1], refractor.size());

  auto refractor_indices = std::vector<u32>(
    results.visible_indices[1].begin(),
    results.visible_indices[1].begin() + refractor.size()
  );
  std::ranges::sort(refractor_indices);
  for (auto index = 0_sz; index < refractor.size(); index++) {
    EXPECT_EQ(refractor_indices[index], capacity - refractor.size() + index);
  }
}
} // namespace ox
