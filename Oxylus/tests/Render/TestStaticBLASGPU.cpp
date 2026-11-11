#include <VkBootstrap.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <glm/gtc/packing.hpp>
#include <gtest/gtest.h>
#include <memory>
#include <span>
#include <vector>
#include <vir/Allocator.hpp>
#include <vir/Context.hpp>

#ifdef OX_PLATFORM_LINUX
  #include <dlfcn.h>
  #include <link.h>
#endif

#include "Asset/AssetManager.hpp"
#include "Asset/Model.hpp"
#include "Core/Base.hpp"
#include "Render/AccelerationStructure.hpp"
#include "Render/RenderContext.hpp"

namespace ox {
static auto retain_static_blas_driver_modules() -> void {
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

TEST(StaticBLASBudget, AdmitsUpToTheBudgetAndAlwaysTheFirstBuild) {
  auto uncapped = StaticBLASBudget{.triangle_budget = 0, .spent = 1'000'000};
  EXPECT_TRUE(uncapped.admits(1'000'000));

  auto budget = StaticBLASBudget{.triangle_budget = 100};
  EXPECT_TRUE(budget.admits(5'000));
  budget.spent = 60;
  EXPECT_TRUE(budget.admits(40));
  EXPECT_FALSE(budget.admits(41));
}

// a mesh buffer laid out the way the loader leaves one: half4 positions first, then LOD0's indices. Host
// visible, so the test can fill it without an upload
struct TestMesh {
  BufferID buffer = BufferID::Invalid;
  GPU::Mesh gpu_mesh = {};
  Model::IndexRange lod0 = {};
};

class StaticBLASGPUTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto instance = vkb::InstanceBuilder{}
                      .set_headless()
                      .require_api_version(1, 3, 0)
                      .enable_extension(VK_KHR_SURFACE_EXTENSION_NAME)
                      .build();
    if (!instance) {
      GTEST_SKIP() << "headless Vulkan instance unavailable: " << instance.error().message();
    }
    vkb_instance = *instance;

    auto features12 = VkPhysicalDeviceVulkan12Features{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.timelineSemaphore = VK_TRUE;
    features12.bufferDeviceAddress = VK_TRUE;
    auto features13 = VkPhysicalDeviceVulkan13Features{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.synchronization2 = VK_TRUE;
    auto acceleration_structure = VkPhysicalDeviceAccelerationStructureFeaturesKHR{};
    acceleration_structure.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    acceleration_structure.accelerationStructure = VK_TRUE;
    const auto physical = vkb::PhysicalDeviceSelector(vkb_instance)
                            .require_present(false)
                            .set_minimum_version(1, 3)
                            .add_required_extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME)
                            .add_required_extension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)
                            .add_required_extension(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME)
                            .set_required_features_12(features12)
                            .set_required_features_13(features13)
                            .add_required_extension_features(acceleration_structure)
                            .select();
    if (!physical) {
      GTEST_SKIP() << "no Vulkan device builds acceleration structures: " << physical.error().message();
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

    render = std::make_unique<RenderContext>();
    ASSERT_TRUE(render->initialize_graph(std::move(*context)));
    // set by device selection in `create_context`, which a headless device never goes through
    render->features |= RenderContext::Feature::RayTracing;
    ASSERT_TRUE(render->use_ray_tracing());
  }

  void TearDown() override {
    for (auto& model : models) {
      if (render) {
        model->destroy_gpu_resources(*render);
      }
    }

    models.clear();
    render.reset();
    if (vkb_device.device != VK_NULL_HANDLE) {
      retain_static_blas_driver_modules();
      vkb::destroy_device(vkb_device);
    }

    if (vkb_instance.instance != VK_NULL_HANDLE) {
      vkb::destroy_instance(vkb_instance);
    }
  }

  auto create_mesh(u32 triangle_count) -> TestMesh {
    // a fan around the origin, so every triangle is distinct and non-degenerate
    const auto vertex_count = triangle_count + 2;
    auto positions = std::vector<u16>();
    positions.reserve(vertex_count * 4);
    for (auto vertex = 0_u32; vertex < vertex_count; vertex++) {
      const auto angle = vertex == 0 ? 0.0f : static_cast<f32>(vertex - 1) * 0.1f;
      const auto radius = vertex == 0 ? 0.0f : 1.0f;
      positions.push_back(glm::packHalf1x16(radius * std::cos(angle)));
      positions.push_back(glm::packHalf1x16(radius * std::sin(angle)));
      positions.push_back(glm::packHalf1x16(0.0f));
      positions.push_back(glm::packHalf1x16(1.0f));
    }

    auto indices = std::vector<u32>();
    indices.reserve(triangle_count * 3);
    for (auto triangle = 0_u32; triangle < triangle_count; triangle++) {
      indices.insert(indices.end(), {0, triangle + 1, triangle + 2});
    }

    const auto positions_size = positions.size() * sizeof(u16);
    const auto indices_offset = (positions_size + 15) & ~15_u64;
    const auto size = indices_offset + indices.size() * sizeof(u32);
    const auto buffer_id = render->allocate_buffer({
      .size = size,
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
      .location = vir::MemoryLocation::CpuToGpu,
      .alignment = 16,
      .name = "test mesh",
    });
    EXPECT_TRUE(buffer_id);
    if (!buffer_id) {
      return {};
    }

    const auto buffer = render->buffer(*buffer_id);
    EXPECT_TRUE(buffer.write<u16>(0, positions));
    EXPECT_TRUE(buffer.write<u32>(indices_offset, indices));
    EXPECT_TRUE(render->persistent->flush_buffer(buffer));

    auto mesh = TestMesh{.buffer = *buffer_id};
    mesh.gpu_mesh.vertex_count = vertex_count;
    mesh.gpu_mesh.vertex_positions = buffer.device_address;
    mesh.lod0 = {.device_address = buffer.device_address + indices_offset, .count = triangle_count * 3};
    return mesh;
  }

  // what `load_model` leaves behind once every mesh is in: a buffer per mesh and no structure for any
  auto create_model(std::span<const u32> triangle_counts) -> Model& {
    auto& model = *models.emplace_back(std::make_unique<Model>());
    const auto mesh_count = triangle_counts.size();
    model.gpu_meshes.resize(mesh_count);
    model.lod0_index_ranges.resize(mesh_count);
    model.gpu_mesh_buffers.assign(mesh_count, BufferID::Invalid);
    model.mesh_blases.assign(mesh_count, AccelerationStructureID::Invalid);
    model.mesh_ready = std::vector<std::atomic_flag>(mesh_count);
    for (auto mesh_index = 0_sz; mesh_index < mesh_count; mesh_index++) {
      const auto mesh = create_mesh(triangle_counts[mesh_index]);
      model.gpu_mesh_buffers[mesh_index] = mesh.buffer;
      model.gpu_meshes[mesh_index] = mesh.gpu_mesh;
      model.lod0_index_ranges[mesh_index] = mesh.lod0;
      model.mesh_ready[mesh_index].test_and_set(std::memory_order_release);
    }

    return model;
  }

  // one frame's worth of the scene's address pass over every mesh of `model`
  auto resolve_frame(Model& model, StaticBLASBudget* budget) -> std::vector<u64> {
    auto addresses = std::vector<u64>();
    for (auto mesh_index = 0_sz; mesh_index < model.mesh_blases.size(); mesh_index++) {
      addresses.push_back(AssetManager::resolve_mesh_blas(*render, model, mesh_index, budget));
    }

    return addresses;
  }

  static auto built_count(const Model& model) -> usize {
    return static_cast<usize>(std::ranges::count_if(model.mesh_blases, [](const AccelerationStructureID blas) {
      return blas != AccelerationStructureID::Invalid;
    }));
  }

  vkb::Instance vkb_instance = {};
  vkb::Device vkb_device = {};
  std::unique_ptr<RenderContext> render = nullptr;
  std::vector<std::unique_ptr<Model>> models = {};
};

TEST_F(StaticBLASGPUTest, BuildsNothingWithoutABudget) {
  const auto triangle_counts = std::array{4_u32, 8_u32};
  auto& model = create_model(triangle_counts);

  for (auto frame = 0; frame < 3; frame++) {
    for (const auto address : resolve_frame(model, nullptr)) {
      EXPECT_EQ(address, 0_u64);
    }
  }

  EXPECT_EQ(built_count(model), 0_sz);
}

TEST_F(StaticBLASGPUTest, BuildsOnceAndKeepsTheStructure) {
  const auto triangle_counts = std::array{4_u32, 8_u32};
  auto& model = create_model(triangle_counts);

  auto budget = StaticBLASBudget{};
  const auto first = resolve_frame(model, &budget);
  ASSERT_EQ(built_count(model), 2_sz);
  EXPECT_EQ(budget.spent, 12_u64);
  for (auto mesh_index = 0_sz; mesh_index < first.size(); mesh_index++) {
    EXPECT_NE(first[mesh_index], 0_u64);
    EXPECT_EQ(first[mesh_index], render->acceleration_structure(model.mesh_blases[mesh_index]).device_address);
  }

  const auto built = model.mesh_blases;
  auto next_budget = StaticBLASBudget{};
  EXPECT_EQ(resolve_frame(model, &next_budget), first);
  EXPECT_EQ(next_budget.spent, 0_u64);
  EXPECT_EQ(model.mesh_blases, built);

  // a consumer that goes away leaves what was built in place
  EXPECT_EQ(resolve_frame(model, nullptr), first);
}

TEST_F(StaticBLASGPUTest, DefersBuildsPastTheBudgetToLaterFrames) {
  const auto triangle_counts = std::array{6_u32, 6_u32, 6_u32, 6_u32, 6_u32};
  auto& model = create_model(triangle_counts);

  auto built_per_frame = std::vector<usize>();
  for (auto frame = 0; frame < 5 && built_count(model) < triangle_counts.size(); frame++) {
    auto budget = StaticBLASBudget{.triangle_budget = 12};
    const auto addresses = resolve_frame(model, &budget);
    EXPECT_LE(budget.spent, 12_u64);
    built_per_frame.push_back(built_count(model));
    for (auto mesh_index = 0_sz; mesh_index < addresses.size(); mesh_index++) {
      const auto is_built = model.mesh_blases[mesh_index] != AccelerationStructureID::Invalid;
      EXPECT_EQ(addresses[mesh_index] != 0, is_built) << "mesh " << mesh_index;
    }
  }

  EXPECT_EQ(built_per_frame, (std::vector<usize>{2, 4, 5}));
}

TEST_F(StaticBLASGPUTest, AMeshLargerThanTheBudgetStillGetsBuilt) {
  const auto triangle_counts = std::array{4_u32, 64_u32};
  auto& model = create_model(triangle_counts);

  auto first = StaticBLASBudget{.triangle_budget = 16};
  resolve_frame(model, &first);
  EXPECT_NE(model.mesh_blases[0], AccelerationStructureID::Invalid);
  EXPECT_EQ(model.mesh_blases[1], AccelerationStructureID::Invalid);

  auto second = StaticBLASBudget{.triangle_budget = 16};
  resolve_frame(model, &second);
  EXPECT_NE(model.mesh_blases[1], AccelerationStructureID::Invalid);
  EXPECT_EQ(second.spent, 64_u64);
}

TEST_F(StaticBLASGPUTest, SkipsMeshesThatAreNotReadyOrHaveNoGPUSide) {
  const auto triangle_counts = std::array{4_u32};
  auto& model = create_model(triangle_counts);
  model.mesh_ready[0].clear(std::memory_order_release);

  auto budget = StaticBLASBudget{};
  EXPECT_EQ(resolve_frame(model, &budget), (std::vector<u64>{0}));
  EXPECT_EQ(budget.spent, 0_u64);
  EXPECT_EQ(model.mesh_blases[0], AccelerationStructureID::Invalid);

  model.mesh_ready[0].test_and_set(std::memory_order_release);
  EXPECT_NE(resolve_frame(model, &budget)[0], 0_u64);

  // published models can mark a mesh ready with nothing on the GPU behind it
  auto published = Model{};
  published.mesh_blases.assign(1, AccelerationStructureID::Invalid);
  published.mesh_ready = std::vector<std::atomic_flag>(1);
  published.mesh_ready[0].test_and_set(std::memory_order_release);
  auto published_budget = StaticBLASBudget{};
  EXPECT_EQ(AssetManager::resolve_mesh_blas(*render, published, 0, &published_budget), 0_u64);
  EXPECT_EQ(AssetManager::resolve_mesh_blas(*render, published, 3, &published_budget), 0_u64);
  EXPECT_EQ(published_budget.spent, 0_u64);
}
} // namespace ox
