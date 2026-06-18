#include "Render/Renderer.hpp"

#include <vuk/runtime/CommandBuffer.hpp>

#include "Asset/Texture.hpp"
#include "Core/App.hpp"
#include "Core/VFS.hpp"
#include "Render/RendererInstance.hpp"
#include "Render/RenderContext.hpp"
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

  self.render_context = &App::get_rendercontext();

  auto& bindless_set = self.render_context->get_descriptor_set();

  self.render_context->wait();

  // --- Shaders ---
  auto& vfs = App::get_vfs();
  auto shaders_dir = vfs.resolve_physical_dir(VFS::APP_DIR, "Shaders");
  auto shader_file = AssetFile::unpack(shaders_dir / "engine.oxpack");
  if (!shader_file.has_value()) {
    return std::unexpected("Cannot initialize renderer shaders!");
  }

  for (const auto &entry : shader_file->entries) {
    const auto *pipeline_data = std::get_if<ShaderPipelineData>(&entry.data);
    if (!pipeline_data) {
      continue;
    }

    self.render_context->create_pipeline(*pipeline_data);
  }

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
  auto temp_atmos_buffer = self.render_context->scratch_buffer(temp_atmos_info);

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

  self.render_context->wait_on(std::move(transmittance_lut_attachment));
  self.render_context->wait_on(std::move(multiscatter_lut_attachment));

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

  self.quad_vertex_buffer = self.render_context->resize_buffer(
    std::move(self.quad_vertex_buffer),
    vuk::MemoryUsage::eGPUonly,
    ox::size_bytes(vertices)
  );
  auto upload_quad_vertex_buffer = self.render_context->upload_staging(std::span(vertices), *self.quad_vertex_buffer);

  self.quad_index_buffer = self.render_context->resize_buffer(
    std::move(self.quad_index_buffer),
    vuk::MemoryUsage::eGPUonly,
    ox::size_bytes(indices)
  );
  auto upload_quad_index_buffer = self.render_context->upload_staging(std::span(indices), *self.quad_index_buffer);

  self.render_context->wait_on(std::move(upload_quad_vertex_buffer));
  self.render_context->wait_on(std::move(upload_quad_index_buffer));

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
