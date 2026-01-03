#include "Render/Renderer.hpp"

#include <vuk/runtime/CommandBuffer.hpp>

#include "Asset/Model.hpp"
#include "Asset/Texture.hpp"
#include "Core/App.hpp"
#include "Core/VFS.hpp"
#include "Render/RendererInstance.hpp"
#include "Render/Vulkan/VkContext.hpp"
#include "Scene/SceneGPU.hpp"

namespace ox {
auto Renderer::new_instance(Scene& scene) -> std::unique_ptr<RendererInstance> {
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

  self.vk_context = &App::get_vkcontext();

  auto& bindless_set = self.vk_context->get_descriptor_set();

  self.vk_context->wait();

  // --- Shaders ---
  auto& vfs = App::get_vfs();
  auto shaders_dir = vfs.resolve_physical_dir(VFS::APP_DIR, "Shaders");

  self.vk_context->create_pipelines(
    SlangSessionInfo{
      .definitions =
        {
          {"MAX_DIRECTIONAL_SHADOW_CASCADES", std::to_string(MAX_DIRECTIONAL_SHADOW_CASCADES)},
          {"MESH_MAX_LODS", std::to_string(GPU::Mesh::MAX_LODS)},
          {"CULLING_MESH_COUNT", "64"},
          {"CULLING_MESHLET_COUNT", std::to_string(Model::MAX_MESHLET_INDICES)},
          {"CULLING_TRIANGLE_COUNT", std::to_string(Model::MAX_MESHLET_PRIMITIVES)},
          {"HISTOGRAM_THREADS_X", std::to_string(GPU::HISTOGRAM_THREADS_X)},
          {"HISTOGRAM_THREADS_Y", std::to_string(GPU::HISTOGRAM_THREADS_Y)},
        },
      .root_directory = shaders_dir,
    },
    {
      {.path = "passes/2d_forward.slang",
       .module_name = "2d_forward",
       .entry_points = {"vs_main", "fs_main"},
       .persistent_set = &bindless_set},
      {.path = "passes/2d_forward_vis.slang",
       .module_name = "2d_forward_vis",
       .entry_points = {"vs_main", "fs_main"},
       .persistent_set = &bindless_set},

      // --- Sky ---
      {.path = "passes/sky_transmittance.slang", .module_name = "sky_transmittance", .entry_points = {"cs_main"}},
      {.path = "passes/sky_multiscattering.slang", .module_name = "sky_multiscatter", .entry_points = {"cs_main"}},
      {.path = "passes/sky_view.slang", .module_name = "sky_view", .entry_points = {"cs_main"}},
      {.path = "passes/sky_aerial_perspective.slang",
       .module_name = "sky_aerial_perspective",
       .entry_points = {"cs_main"}},

      // --- VISBUFFER ---
      {.path = "passes/cull_meshes.slang", .module_name = "vis_cull_meshes", .entry_points = {"cs_main"}},
      {.path = "passes/cull_meshes.slang",
       .module_name = "vis_generate_cull_commands",
       .entry_points = {"generate_commands_cs_main"}},
      {.path = "passes/cull_meshlets.slang", .module_name = "vis_cull_meshlets", .entry_points = {"cs_main"}},
      {.path = "passes/cull_triangles.slang", .module_name = "vis_cull_triangles", .entry_points = {"cs_main"}},
      {.path = "passes/visbuffer_encode.slang",
       .module_name = "visbuffer_encode",
       .entry_points = {"vs_main", "fs_main"},
       .persistent_set = &bindless_set},
      {.path = "passes/visbuffer_clear.slang", .module_name = "visbuffer_clear", .entry_points = {"cs_main"}},
      {.path = "passes/visbuffer_decode.slang",
       .module_name = "visbuffer_decode",
       .entry_points = {"vs_main", "fs_main"},
       .persistent_set = &bindless_set},

      // --- SHADOWMAP ---
      {.path = "passes/shadowmap_cull_meshes.slang",
       .module_name = "shadowmap_cull_meshes",
       .entry_points = {"cs_main"}},
      {.path = "passes/shadowmap_cull_meshes.slang",
       .module_name = "shadowmap_generate_cull_commands",
       .entry_points = {"generate_commands_cs_main"}},
      {.path = "passes/shadowmap_cull_meshlets.slang",
       .module_name = "shadowmap_cull_meshlets",
       .entry_points = {"cs_main"}},
      {.path = "passes/shadowmap_cull_triangles.slang",
       .module_name = "shadowmap_cull_triangles",
       .entry_points = {"cs_main"}},
      {.path = "passes/shadowmap_draw.slang", .module_name = "shadowmap_draw", .entry_points = {"vs_main", "fs_main"}},
      {.path = "passes/debug_view.slang", .module_name = "debug_view", .entry_points = {"vs_main", "fs_main"}},

      // --- PBR ---
      {.path = "passes/pbr_apply.slang", .module_name = "pbr_apply", .entry_points = {"vs_main", "fs_main"}},

      //  --- FFX ---
      {.path = "passes/hiz.slang", .module_name = "hiz", .entry_points = {"cs_main"}},

      // --- PostProcess ---
      {.path = "passes/histogram_generate.slang", .module_name = "histogram_generate", .entry_points = {"cs_main"}},
      {.path = "passes/histogram_average.slang", .module_name = "histogram_average", .entry_points = {"cs_main"}},
      {.path = "passes/tonemap.slang", .module_name = "tonemap", .entry_points = {"vs_main", "fs_main"}},

      // --- Bloom ---
      {.path = "passes/bloom/bloom_prefilter.slang", .module_name = "bloom_prefilter", .entry_points = {"cs_main"}},
      {.path = "passes/bloom/bloom_downsample.slang", .module_name = "bloom_downsample", .entry_points = {"cs_main"}},
      {.path = "passes/bloom/bloom_upsample.slang", .module_name = "bloom_upsample", .entry_points = {"cs_main"}},

      // --- VBGTAO ---
      {.path = "passes/gtao/vbgtao_prefilter.slang", .module_name = "vbgtao_prefilter", .entry_points = {"cs_main"}},
      {.path = "passes/gtao/vbgtao_main.slang", .module_name = "vbgtao_main", .entry_points = {"cs_main"}},
      {.path = "passes/gtao/vbgtao_denoise.slang", .module_name = "vbgtao_denoise", .entry_points = {"cs_main"}},

      // --- FXAA ---
      {.path = "passes/fxaa/fxaa.slang", .module_name = "fxaa", .entry_points = {"vs_main", "fs_main"}},

      {.path = "passes/debug_mesh.slang", .module_name = "debug_mesh", .entry_points = {"vs_main", "fs_main"}},
      {.path = "passes/contact_shadows.slang", .module_name = "contact_shadows", .entry_points = {"cs_main"}},
    }
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

    return static_cast<u16>(index);
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
      cmd_list //
        .bind_compute_pipeline("sky_transmittance")
        .bind_image(0, 0, dst)
        .bind_buffer(0, 1, atmos)
        .dispatch_invocations_per_pixel(dst);

      return std::make_tuple(dst, atmos);
    }
  );

  std::tie(transmittance_lut_attachment, temp_atmos_buffer) = transmittance_lut_pass(
    transmittance_lut_attachment,
    temp_atmos_buffer
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
    transmittance_lut_attachment,
    multiscatter_lut_attachment,
    temp_atmos_buffer
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

  return {};
}

auto Renderer::new_frame(this Renderer& self) -> void {
  self.acquired_sky_transmittance_lut_view = self.sky_transmittance_lut_view.acquire(
    "sky_transmittance_lut",
    vuk::Access::eComputeSampled
  );
  self.acquired_sky_multiscatter_lut_view = self.sky_multiscatter_lut_view.acquire(
    "sky_multiscatter_lut",
    vuk::Access::eComputeSampled
  );
  self.acquired_hilbert_noise_lut = self.hilbert_noise_lut.acquire("hilbert noise", vuk::eComputeSampled);
}
} // namespace ox
