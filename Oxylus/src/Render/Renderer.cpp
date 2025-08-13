#include "Render/Renderer.hpp"

#include "Asset/Mesh.hpp"
#include "Asset/Texture.hpp"
#include "Core/App.hpp"
#include "Core/VFS.hpp"
#include "Render/RendererInstance.hpp"
#include "Render/Slang/Slang.hpp"
#include "Render/Utils/VukCommon.hpp"
#include "Render/Vulkan/VkContext.hpp"
#include "Scene/SceneGPU.hpp"

namespace ox {
static constexpr auto sampler_min_clamp_reduction_mode = VkSamplerReductionModeCreateInfo{
    .sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO,
    .pNext = nullptr,
    .reductionMode = VK_SAMPLER_REDUCTION_MODE_MIN,
};

Renderer::Renderer(VkContext* vk_context) {
  ZoneScoped;
  this->vk_context = vk_context;
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

auto Renderer::init() -> std::expected<void, std::string> {
  if (initalized)
    return std::unexpected("Renderer already initialized!");

  initalized = true;

  auto& runtime = *vk_context->runtime;
  auto& allocator = *vk_context->superframe_allocator;

  vk_context->wait();

  auto dslci_01 = vuk::descriptor_set_layout_create_info(
      {
          ds_layout_binding(BindlessID::Samplers, vuk::DescriptorType::eSampler, 6),        // Samplers
          ds_layout_binding(BindlessID::SampledImages, vuk::DescriptorType::eSampledImage), // SampledImages
      },
      1);

  // --- Shaders ---
  auto* vfs = App::get_system<VFS>(EngineSystems::VFS);
  auto shaders_dir = vfs->resolve_physical_dir(VFS::APP_DIR, "Shaders");

  Slang slang = {};
  slang.create_session({.root_directory = shaders_dir,
                        .definitions = {
                            {"CULLING_MESHLET_COUNT", std::to_string(Mesh::MAX_MESHLET_INDICES)},
                            {"CULLING_TRIANGLE_COUNT", std::to_string(Mesh::MAX_MESHLET_PRIMITIVES)},
                            {"HISTOGRAM_THREADS_X", std::to_string(GPU::HISTOGRAM_THREADS_X)},
                            {"HISTOGRAM_THREADS_Y", std::to_string(GPU::HISTOGRAM_THREADS_Y)},
                        }});

  slang.create_pipeline(runtime,
                        "2d_forward_pipeline",
                        dslci_01,
                        {.path = shaders_dir + "/passes/2d_forward.slang", .entry_points = {"vs_main", "ps_main"}});

  // --- Sky ---
  slang.create_pipeline(runtime,
                        "sky_transmittance_pipeline",
                        {},
                        {.path = shaders_dir + "/passes/sky_transmittance.slang", .entry_points = {"cs_main"}});

  slang.create_pipeline(runtime,
                        "sky_multiscatter_lut_pipeline",
                        {},
                        {.path = shaders_dir + "/passes/sky_multiscattering.slang", .entry_points = {"cs_main"}});

  slang.create_pipeline(runtime,
                        "sky_view_pipeline",
                        dslci_01,
                        {.path = shaders_dir + "/passes/sky_view.slang", .entry_points = {"cs_main"}});

  slang.create_pipeline(runtime,
                        "sky_aerial_perspective_pipeline",
                        dslci_01,
                        {.path = shaders_dir + "/passes/sky_aerial_perspective.slang", .entry_points = {"cs_main"}});

  slang.create_pipeline(runtime,
                        "sky_final_pipeline",
                        dslci_01,
                        {.path = shaders_dir + "/passes/sky_final.slang", .entry_points = {"vs_main", "fs_main"}});

  // --- VISBUFFER ---
  slang.create_pipeline(
      runtime, "cull_meshlets", {}, {.path = shaders_dir + "/passes/cull_meshlets.slang", .entry_points = {"cs_main"}});

  slang.create_pipeline(runtime,
                        "cull_triangles",
                        {},
                        {.path = shaders_dir + "/passes/cull_triangles.slang", .entry_points = {"cs_main"}});

  slang.create_pipeline(
      runtime,
      "visbuffer_encode",
      dslci_01,
      {.path = shaders_dir + "/passes/visbuffer_encode.slang", .entry_points = {"vs_main", "fs_main"}});

  slang.create_pipeline(runtime,
                        "visbuffer_clear",
                        {},
                        {.path = shaders_dir + "/passes/visbuffer_clear.slang", .entry_points = {"cs_main"}});

  slang.create_pipeline(
      runtime,
      "visbuffer_decode",
      dslci_01,
      {.path = shaders_dir + "/passes/visbuffer_decode.slang", .entry_points = {"vs_main", "fs_main"}});

  slang.create_pipeline(
      runtime, "debug", {}, {.path = shaders_dir + "/passes/debug.slang", .entry_points = {"vs_main", "fs_main"}});

  // --- PBR ---
  slang.create_pipeline(
      runtime, "brdf", dslci_01, {.path = shaders_dir + "/passes/brdf.slang", .entry_points = {"vs_main", "fs_main"}});

  //  --- FFX ---
  slang.create_pipeline(
      runtime, "hiz_pipeline", {}, {.path = shaders_dir + "/passes/hiz.slang", .entry_points = {"cs_main"}});

  // --- PostProcess ---
  slang.create_pipeline(runtime,
                        "histogram_generate_pipeline",
                        {},
                        {.path = shaders_dir + "/passes/histogram_generate.slang", .entry_points = {"cs_main"}});

  slang.create_pipeline(runtime,
                        "histogram_average_pipeline",
                        {},
                        {.path = shaders_dir + "/passes/histogram_average.slang", .entry_points = {"cs_main"}});

  slang.create_pipeline(runtime,
                        "tonemap_pipeline",
                        {},
                        {.path = shaders_dir + "/passes/tonemap.slang", .entry_points = {"vs_main", "fs_main"}});

  slang.create_pipeline(runtime,
                        "bloom_prefilter_pipeline",
                        {},
                        {.path = shaders_dir + "/passes/bloom/bloom_prefilter.slang", .entry_points = {"cs_main"}});

  slang.create_pipeline(runtime,
                        "bloom_downsample_pipeline",
                        {},
                        {.path = shaders_dir + "/passes/bloom/bloom_downsample.slang", .entry_points = {"cs_main"}});

  slang.create_pipeline(runtime,
                        "bloom_upsample_pipeline",
                        {},
                        {.path = shaders_dir + "/passes/bloom/bloom_upsample.slang", .entry_points = {"cs_main"}});

  slang.create_pipeline(runtime,
                        "fxaa_pipeline",
                        {},
                        {.path = shaders_dir + "/passes/fxaa/fxaa.slang", .entry_points = {"vs_main", "fs_main"}});

  // --- DescriptorSets ---
  this->descriptor_set_01 = runtime.create_persistent_descriptorset(allocator, dslci_01, 1);

  // --- Samplers ---
  static constexpr auto hiz_sampler_ci = vuk::SamplerCreateInfo{
      .pNext = &sampler_min_clamp_reduction_mode,
      .magFilter = vuk::Filter::eLinear,
      .minFilter = vuk::Filter::eLinear,
      .mipmapMode = vuk::SamplerMipmapMode::eNearest,
      .addressModeU = vuk::SamplerAddressMode::eClampToEdge,
      .addressModeV = vuk::SamplerAddressMode::eClampToEdge,
  };

  const vuk::Sampler linear_sampler_clamped = runtime.acquire_sampler(vuk::LinearSamplerClamped,
                                                                      runtime.get_frame_count());
  const vuk::Sampler linear_sampler_repeated = runtime.acquire_sampler(vuk::LinearSamplerRepeated,
                                                                       runtime.get_frame_count());
  const vuk::Sampler linear_sampler_repeated_anisotropy = runtime.acquire_sampler(vuk::LinearSamplerRepeatedAnisotropy,
                                                                                  runtime.get_frame_count());
  const vuk::Sampler nearest_sampler_clamped = runtime.acquire_sampler(vuk::NearestSamplerClamped,
                                                                       runtime.get_frame_count());
  const vuk::Sampler nearest_sampler_repeated = runtime.acquire_sampler(vuk::NearestSamplerRepeated,
                                                                        runtime.get_frame_count());
  const vuk::Sampler hiz_sampler = runtime.acquire_sampler(hiz_sampler_ci, runtime.get_frame_count());
  this->descriptor_set_01->update_sampler(BindlessID::Samplers, 0, linear_sampler_repeated);
  this->descriptor_set_01->update_sampler(BindlessID::Samplers, 1, linear_sampler_clamped);

  this->descriptor_set_01->update_sampler(BindlessID::Samplers, 2, nearest_sampler_repeated);
  this->descriptor_set_01->update_sampler(BindlessID::Samplers, 3, nearest_sampler_clamped);

  this->descriptor_set_01->update_sampler(BindlessID::Samplers, 4, linear_sampler_repeated_anisotropy);

  this->descriptor_set_01->update_sampler(BindlessID::Samplers, 5, hiz_sampler);

  sky_transmittance_lut_view = Texture("sky_transmittance_lut");
  sky_transmittance_lut_view.create({},
                                    {.preset = vuk::ImageAttachment::Preset::eSTT2DUnmipped,
                                     .format = vuk::Format::eR16G16B16A16Sfloat,
                                     .extent = vuk::Extent3D{.width = 256u, .height = 64u, .depth = 1u}});

  sky_multiscatter_lut_view = Texture("sky_multiscatter_lut");
  sky_multiscatter_lut_view.create({},
                                   {.preset = vuk::ImageAttachment::Preset::eSTT2DUnmipped,
                                    .format = vuk::Format::eR16G16B16A16Sfloat,
                                    .extent = vuk::Extent3D{.width = 32u, .height = 32u, .depth = 1u}});

  auto temp_atmos_info = GPU::Atmosphere{};
  temp_atmos_info.transmittance_lut_size = sky_transmittance_lut_view.get_extent();
  temp_atmos_info.multiscattering_lut_size = sky_multiscatter_lut_view.get_extent();
  auto temp_atmos_buffer = vk_context->scratch_buffer(std::span(&temp_atmos_info, 1));

  auto transmittance_lut_attachment = sky_transmittance_lut_view.discard("sky_transmittance_lut");

  std::tie(transmittance_lut_attachment, temp_atmos_buffer) = vuk::make_pass(
      "transmittance_lut_pass",
      [](vuk::CommandBuffer& cmd_list, VUK_IA(vuk::eComputeRW) dst, VUK_BA(vuk::eComputeRead) atmos) {
        cmd_list.bind_compute_pipeline("sky_transmittance_pipeline")
            .bind_image(0, 0, dst)
            .bind_buffer(0, 1, atmos)
            .dispatch_invocations_per_pixel(dst);

        return std::make_tuple(dst, atmos);
      })(std::move(transmittance_lut_attachment), std::move(temp_atmos_buffer));

  auto multiscatter_lut_attachment = sky_multiscatter_lut_view.discard("sky_multiscatter_lut");

  std::tie(transmittance_lut_attachment, multiscatter_lut_attachment, temp_atmos_buffer) = vuk::make_pass(
      "sky_multiscatter_lut_pass",
      [](vuk::CommandBuffer& cmd_list,
         VUK_IA(vuk::eComputeSampled) sky_transmittance_lut,
         VUK_IA(vuk::eComputeRW) sky_multiscatter_lut,
         VUK_BA(vuk::eComputeRead) atmos) {
        cmd_list.bind_compute_pipeline("sky_multiscatter_lut_pipeline")
            .bind_sampler(0, 0, {.magFilter = vuk::Filter::eLinear, .minFilter = vuk::Filter::eLinear})
            .bind_image(0, 1, sky_transmittance_lut)
            .bind_image(0, 2, sky_multiscatter_lut)
            .bind_buffer(0, 3, atmos)
            .dispatch_invocations_per_pixel(sky_multiscatter_lut);

        return std::make_tuple(sky_transmittance_lut, sky_multiscatter_lut, atmos);
      })(std::move(transmittance_lut_attachment), std::move(multiscatter_lut_attachment), std::move(temp_atmos_buffer));

  transmittance_lut_attachment = transmittance_lut_attachment.as_released(vuk::eComputeSampled,
                                                                          vuk::DomainFlagBits::eGraphicsQueue);
  multiscatter_lut_attachment = multiscatter_lut_attachment.as_released(vuk::eComputeSampled,
                                                                        vuk::DomainFlagBits::eGraphicsQueue);

  vk_context->wait_on(std::move(transmittance_lut_attachment));
  vk_context->wait_on(std::move(multiscatter_lut_attachment));

  if (this->exposure_buffer) {
    this->exposure_buffer.reset();
  }
  this->exposure_buffer = vk_context->allocate_buffer_super(vuk::MemoryUsage::eGPUonly,
                                                            sizeof(GPU::HistogramLuminance));

  return {};
}

auto Renderer::deinit() -> std::expected<void, std::string> { return {}; }
} // namespace ox
