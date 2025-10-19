#include "Render/Renderer.hpp"

#include "Asset/Model.hpp"
#include "Asset/Texture.hpp"
#include "Core/App.hpp"
#include "Core/VFS.hpp"
#include "Render/DebugRenderer.hpp"
#include "Render/RendererInstance.hpp"
#include "Render/Slang/Slang.hpp"
#include "Render/Vulkan/VkContext.hpp"
#include "Scene/SceneGPU.hpp"

namespace ox {
Renderer::Renderer(VkContext* vk_ctx) {
  ZoneScoped;
  this->vk_context = vk_ctx;
}

auto Renderer::new_instance(Scene* scene) -> std::unique_ptr<RendererInstance> {
  ZoneScoped;

  if (!initalized) {
    OX_LOG_ERROR("Renderer must be initialized before creating instances!");
    return nullptr;
  }

  auto instance = std::make_unique<RendererInstance>(scene, *this);
  return instance;
}

auto Renderer::init(this Renderer& self) -> std::expected<void, std::string> {
  if (self.initalized)
    return std::unexpected("Renderer already initialized!");

  self.initalized = true;

  DebugRenderer::init();

  auto& runtime = *self.vk_context->runtime;
  auto& bindless_set = self.vk_context->get_descriptor_set();

  self.vk_context->wait();

  // --- Shaders ---
  auto& vfs = App::get_vfs();
  auto shaders_dir = vfs.resolve_physical_dir(VFS::APP_DIR, "Shaders");

  Slang slang = {};
  slang.create_session(
    {.optimization_level = Slang::OptimizationLevel::Maximal,
     .root_directory = shaders_dir,
     .definitions = {
       {"MAX_DIRECTIONAL_SHADOW_CASCADES", std::to_string(MAX_DIRECTIONAL_SHADOW_CASCADES)},
       {"MESH_MAX_LODS", std::to_string(GPU::Mesh::MAX_LODS)},
       {"CULLING_MESH_COUNT", "64"},
       {"CULLING_MESHLET_COUNT", std::to_string(Model::MAX_MESHLET_INDICES)},
       {"CULLING_TRIANGLE_COUNT", std::to_string(Model::MAX_MESHLET_PRIMITIVES)},
       {"HISTOGRAM_THREADS_X", std::to_string(GPU::HISTOGRAM_THREADS_X)},
       {"HISTOGRAM_THREADS_Y", std::to_string(GPU::HISTOGRAM_THREADS_Y)},
     }}
  );

  slang.create_pipeline(
    runtime,
    "2d_forward",
    {.path = shaders_dir + "/passes/2d_forward.slang", .entry_points = {"vs_main", "fs_main"}},
    &bindless_set
  );

  // --- Sky ---
  slang.create_pipeline(
    runtime,
    "sky_transmittance",
    {.path = shaders_dir + "/passes/sky_transmittance.slang", .entry_points = {"cs_main"}}
  );

  slang.create_pipeline(
    runtime,
    "sky_multiscatter",
    {.path = shaders_dir + "/passes/sky_multiscattering.slang", .entry_points = {"cs_main"}}
  );

  slang.create_pipeline(
    runtime,
    "sky_view",
    {.path = shaders_dir + "/passes/sky_view.slang", .entry_points = {"cs_main"}}
  );

  slang.create_pipeline(
    runtime,
    "sky_aerial_perspective",
    {.path = shaders_dir + "/passes/sky_aerial_perspective.slang", .entry_points = {"cs_main"}}
  );

  // --- VISBUFFER ---
  slang.create_pipeline(
    runtime,
    "vis_cull_meshes",
    {.path = shaders_dir + "/passes/cull_meshes.slang", .entry_points = {"cs_main"}}
  );

  slang.create_pipeline(
    runtime,
    "vis_generate_cull_commands",
    {.path = shaders_dir + "/passes/cull_meshes.slang", .entry_points = {"generate_commands_cs_main"}}
  );

  slang.create_pipeline(
    runtime,
    "vis_cull_meshlets",
    {.path = shaders_dir + "/passes/cull_meshlets.slang", .entry_points = {"cs_main"}}
  );

  slang.create_pipeline(
    runtime,
    "vis_cull_triangles",
    {.path = shaders_dir + "/passes/cull_triangles.slang", .entry_points = {"cs_main"}}
  );

  slang.create_pipeline(
    runtime,
    "visbuffer_encode",
    {.path = shaders_dir + "/passes/visbuffer_encode.slang", .entry_points = {"vs_main", "fs_main"}},
    &bindless_set
  );

  slang.create_pipeline(
    runtime,
    "visbuffer_clear",
    {.path = shaders_dir + "/passes/visbuffer_clear.slang", .entry_points = {"cs_main"}}
  );

  slang.create_pipeline(
    runtime,
    "visbuffer_decode",
    {.path = shaders_dir + "/passes/visbuffer_decode.slang", .entry_points = {"vs_main", "fs_main"}},
    &bindless_set
  );

  // --- SHADOWMAP ---
  slang.create_pipeline(
    runtime,
    "shadowmap_cull_meshes",
    {.path = shaders_dir + "/passes/shadowmap_cull_meshes.slang", .entry_points = {"cs_main"}}
  );

  slang.create_pipeline(
    runtime,
    "shadowmap_generate_cull_commands",
    {.path = shaders_dir + "/passes/shadowmap_cull_meshes.slang", .entry_points = {"generate_commands_cs_main"}}
  );

  slang.create_pipeline(
    runtime,
    "shadowmap_cull_meshlets",
    {.path = shaders_dir + "/passes/shadowmap_cull_meshlets.slang", .entry_points = {"cs_main"}}
  );

  slang.create_pipeline(
    runtime,
    "shadowmap_cull_triangles",
    {.path = shaders_dir + "/passes/shadowmap_cull_triangles.slang", .entry_points = {"cs_main"}}
  );

  slang.create_pipeline(
    runtime,
    "shadowmap_draw",
    {.path = shaders_dir + "/passes/shadowmap_draw.slang", .entry_points = {"vs_main", "fs_main"}}
  );

  slang.create_pipeline(
    runtime,
    "debug",
    {.path = shaders_dir + "/passes/debug.slang", .entry_points = {"vs_main", "fs_main"}}
  );

  // --- PBR ---
  slang.create_pipeline(
    runtime,
    "brdf",
    {.path = shaders_dir + "/passes/brdf.slang", .entry_points = {"vs_main", "fs_main"}}
  );

  //  --- FFX ---
  slang.create_pipeline(runtime, "hiz", {.path = shaders_dir + "/passes/hiz.slang", .entry_points = {"cs_main"}});

  // --- PostProcess ---
  slang.create_pipeline(
    runtime,
    "histogram_generate",
    {.path = shaders_dir + "/passes/histogram_generate.slang", .entry_points = {"cs_main"}}
  );

  slang.create_pipeline(
    runtime,
    "histogram_average",
    {.path = shaders_dir + "/passes/histogram_average.slang", .entry_points = {"cs_main"}}
  );

  slang.create_pipeline(
    runtime,
    "tonemap",
    {.path = shaders_dir + "/passes/tonemap.slang", .entry_points = {"vs_main", "fs_main"}}
  );

  // --- Bloom ---
  slang.create_pipeline(
    runtime,
    "bloom_prefilter",
    {.path = shaders_dir + "/passes/bloom/bloom_prefilter.slang", .entry_points = {"cs_main"}}
  );

  slang.create_pipeline(
    runtime,
    "bloom_downsample",
    {.path = shaders_dir + "/passes/bloom/bloom_downsample.slang", .entry_points = {"cs_main"}}
  );

  slang.create_pipeline(
    runtime,
    "bloom_upsample",
    {.path = shaders_dir + "/passes/bloom/bloom_upsample.slang", .entry_points = {"cs_main"}}
  );

  // --- VBGTAO ---
  slang.create_pipeline(
    runtime,
    "vbgtao_prefilter",
    {.path = shaders_dir + "/passes/gtao/vbgtao_prefilter.slang", .entry_points = {"cs_main"}}
  );

  slang.create_pipeline(
    runtime,
    "vbgtao_main",
    {.path = shaders_dir + "/passes/gtao/vbgtao_main.slang", .entry_points = {"cs_main"}}
  );

  slang.create_pipeline(
    runtime,
    "vbgtao_denoise",
    {.path = shaders_dir + "/passes/gtao/vbgtao_denoise.slang", .entry_points = {"cs_main"}}
  );

  // --- FXAA ---
  slang.create_pipeline(
    runtime,
    "fxaa",
    {.path = shaders_dir + "/passes/fxaa/fxaa.slang", .entry_points = {"vs_main", "fs_main"}}
  );

  slang.create_pipeline(
    runtime,
    "debug_renderer",
    {.path = shaders_dir + "/passes/debug_renderer.slang", .entry_points = {"vs_main", "fs_main"}}
  );

  slang.create_pipeline(
    runtime,
    "contact_shadows",
    {.path = shaders_dir + "/passes/contact_shadows.slang", .entry_points = {"cs_main"}}
  );

  self.sky_transmittance_lut_view = Texture("sky_transmittance_lut");
  self.sky_transmittance_lut_view.create(
    {},
    {.preset = vuk::ImageAttachment::Preset::eSTT2DUnmipped,
     .format = vuk::Format::eR16G16B16A16Sfloat,
     .extent = vuk::Extent3D{.width = 256u, .height = 64u, .depth = 1u}}
  );

  self.sky_multiscatter_lut_view = Texture("sky_multiscatter_lut");
  self.sky_multiscatter_lut_view.create(
    {},
    {.preset = vuk::ImageAttachment::Preset::eSTT2DUnmipped,
     .format = vuk::Format::eR16G16B16A16Sfloat,
     .extent = vuk::Extent3D{.width = 32u, .height = 32u, .depth = 1u}}
  );

  constexpr auto HILBERT_NOISE_LUT_WIDTH = 64_u32;
  auto hilbert_index = [](u32 pos_x, u32 pos_y) -> u16 {
    auto index = 0_u32;
    for (auto cur_level = HILBERT_NOISE_LUT_WIDTH / 2; cur_level > 0_u32; cur_level /= 2_u32) {
      auto region_x = (pos_x & cur_level) > 0_u32;
      auto region_y = (pos_y & cur_level) > 0_u32;
      index += cur_level * cur_level * ((3_u32 * region_x) ^ region_y);
      if (region_y == 0_u32) {
        if (region_x == 1_u32) {
          pos_x = (HILBERT_NOISE_LUT_WIDTH - 1_u32) - pos_x;
          pos_y = (HILBERT_NOISE_LUT_WIDTH - 1_u32) - pos_y;
        }

        auto temp_pos_x = pos_x;
        pos_x = pos_y;
        pos_y = temp_pos_x;
      }
    }

    return index;
  };

  u16 hilbert_noise[HILBERT_NOISE_LUT_WIDTH * HILBERT_NOISE_LUT_WIDTH] = {};
  for (auto y = 0_u32; y < HILBERT_NOISE_LUT_WIDTH; y++) {
    for (auto x = 0_u32; x < HILBERT_NOISE_LUT_WIDTH; x++) {
      hilbert_noise[y * HILBERT_NOISE_LUT_WIDTH + x] = hilbert_index(x, y);
    }
  }

  self.hilbert_noise_lut = Texture("hilbert_noise_lut");
  self.hilbert_noise_lut.create(
    {},
    {.preset = vuk::ImageAttachment::Preset::eSTT2DUnmipped,
     .format = vuk::Format::eR16Uint,
     .loaded_data = hilbert_noise,
     .extent = vuk::Extent3D{.width = HILBERT_NOISE_LUT_WIDTH, .height = HILBERT_NOISE_LUT_WIDTH, .depth = 1u}}
  );

  auto temp_atmos_info = GPU::Atmosphere{};
  temp_atmos_info.transmittance_lut_size = self.sky_transmittance_lut_view.get_extent();
  temp_atmos_info.multiscattering_lut_size = self.sky_multiscatter_lut_view.get_extent();
  auto temp_atmos_buffer = self.vk_context->scratch_buffer(temp_atmos_info);

  auto transmittance_lut_attachment = self.sky_transmittance_lut_view.discard("sky_transmittance_lut");

  auto transmittance_lut_pass = vuk::make_pass(
    "transmittance_lut_pass",
    [](vuk::CommandBuffer& cmd_list, VUK_IA(vuk::eComputeRW) dst, VUK_BA(vuk::eComputeRead) atmos) {
      cmd_list.bind_compute_pipeline("sky_transmittance")
        .bind_image(0, 0, dst)
        .bind_buffer(0, 1, atmos)
        .dispatch_invocations_per_pixel(dst);

      return std::make_tuple(dst, atmos);
    }
  );

  std::tie(transmittance_lut_attachment, temp_atmos_buffer) = transmittance_lut_pass(
    std::move(transmittance_lut_attachment),
    std::move(temp_atmos_buffer)
  );

  auto multiscatter_lut_attachment = self.sky_multiscatter_lut_view.discard("sky_multiscatter_lut");
  auto sky_multiscatter_lut_pass = vuk::make_pass(
    "sky_multiscatter_lut_pass",
    [](
      vuk::CommandBuffer& cmd_list,
      VUK_IA(vuk::eComputeSampled) sky_transmittance_lut,
      VUK_IA(vuk::eComputeRW) sky_multiscatter_lut,
      VUK_BA(vuk::eComputeRead) atmos
    ) {
      cmd_list.bind_compute_pipeline("sky_multiscatter")
        .bind_sampler(0, 0, {.magFilter = vuk::Filter::eLinear, .minFilter = vuk::Filter::eLinear})
        .bind_image(0, 1, sky_transmittance_lut)
        .bind_image(0, 2, sky_multiscatter_lut)
        .bind_buffer(0, 3, atmos)
        .dispatch_invocations_per_pixel(sky_multiscatter_lut);

      return std::make_tuple(sky_transmittance_lut, sky_multiscatter_lut, atmos);
    }
  );

  std::tie(transmittance_lut_attachment, multiscatter_lut_attachment, temp_atmos_buffer) = sky_multiscatter_lut_pass(
    std::move(transmittance_lut_attachment),
    std::move(multiscatter_lut_attachment),
    std::move(temp_atmos_buffer)
  );

  transmittance_lut_attachment = transmittance_lut_attachment.as_released(
    vuk::eComputeSampled,
    vuk::DomainFlagBits::eGraphicsQueue
  );
  multiscatter_lut_attachment = multiscatter_lut_attachment.as_released(
    vuk::eComputeSampled,
    vuk::DomainFlagBits::eGraphicsQueue
  );

  self.vk_context->wait_on(std::move(transmittance_lut_attachment));
  self.vk_context->wait_on(std::move(multiscatter_lut_attachment));

  struct Vertex {
    alignas(4) glm::vec3 position = {};
    alignas(4) glm::vec2 uv = {};
  };
  std::vector<Vertex> vertices(4);
  vertices[0].position = glm::vec3{-1.f, -1.f, 0.f};
  vertices[0].uv = glm::vec2{0.f, 0.f};

  vertices[1].position = glm::vec3{1.f, -1.f, 0.f};
  vertices[1].uv = glm::vec2{1.f, 0.f};

  vertices[2].position = glm::vec3{1.f, 1.f, 0.f};
  vertices[2].uv = glm::vec2{1.f, 1.f};

  vertices[3].position = glm::vec3{-1.f, 1.f, 0.f};
  vertices[3].uv = glm::vec2{0.f, 1.f};

  auto indices = std::vector<uint32_t>{0, 1, 2, 2, 3, 0};

  self.quad_vertex_buffer = self.vk_context->resize_buffer(
    std::move(self.quad_vertex_buffer),
    vuk::MemoryUsage::eGPUonly,
    ox::size_bytes(vertices)
  );
  auto upload_quad_vertex_buffer = self.vk_context->upload_staging(std::span(vertices), *self.quad_vertex_buffer);

  self.quad_index_buffer = self.vk_context->resize_buffer(
    std::move(self.quad_index_buffer),
    vuk::MemoryUsage::eGPUonly,
    ox::size_bytes(indices)
  );
  auto upload_quad_index_buffer = self.vk_context->upload_staging(std::span(indices), *self.quad_index_buffer);

  self.vk_context->wait_on(std::move(upload_quad_vertex_buffer));
  self.vk_context->wait_on(std::move(upload_quad_index_buffer));

  return {};
}

auto Renderer::deinit(this Renderer& self) -> std::expected<void, std::string> {
  ZoneScoped;

  DebugRenderer::release();

  return {};
}
} // namespace ox
