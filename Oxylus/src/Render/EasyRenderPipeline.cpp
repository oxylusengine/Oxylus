#include "Render/EasyRenderPipeline.hpp"

#include <vuk/ImageAttachment.hpp>
#include <vuk/RenderGraph.hpp>
#include <vuk/Types.hpp>
#include <vuk/Value.hpp>
#include <vuk/runtime/vk/Allocator.hpp>
#include <vuk/runtime/vk/Image.hpp>
#include <vuk/vsl/Core.hpp>

#include "Asset/AssetManager.hpp"
#include "Asset/Texture.hpp"
#include "Core/App.hpp"
#include "Core/VFS.hpp"
#include "Render/Camera.hpp"
#include "Render/DebugRenderer.hpp"
#include "Render/RendererConfig.hpp"
#include "Render/Slang/Slang.hpp"
#include "Render/Utils/VukCommon.hpp"
#include "Render/Vulkan/VkContext.hpp"
#include "Scene/ECSModule/Core.hpp"
#include "Scene/SceneGPU.hpp"
#include "Utils/Profiler.hpp"

namespace ox {
auto EasyRenderPipeline::init(VkContext& vk_context) -> void {
  if (initalized)
    return;

  initalized = true;

  auto& runtime = *vk_context.runtime;
  auto& allocator = *vk_context.superframe_allocator;

  vk_context.wait();

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

  //  ── FFX ─────────────────────────────────────────────────────────────
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
  auto hiz_sampler_ci = vuk::SamplerCreateInfo{
      .magFilter = vuk::Filter::eNearest,
      .minFilter = vuk::Filter::eNearest,
      .mipmapMode = vuk::SamplerMipmapMode::eNearest,
      .addressModeU = vuk::SamplerAddressMode::eClampToEdge,
      .addressModeV = vuk::SamplerAddressMode::eClampToEdge,
      .addressModeW = vuk::SamplerAddressMode::eClampToEdge,
      .minLod = 0.0f,
      .maxLod = 16.0f,
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
  auto temp_atmos_buffer = vk_context.scratch_buffer(std::span(&temp_atmos_info, 1));

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

  vk_context.wait_on(std::move(transmittance_lut_attachment));
  vk_context.wait_on(std::move(multiscatter_lut_attachment));

  if (this->exposure_buffer) {
    this->exposure_buffer.reset();
  }
  this->exposure_buffer = vk_context.allocate_buffer_super(vuk::MemoryUsage::eGPUonly, sizeof(GPU::HistogramLuminance));
}

auto EasyRenderPipeline::deinit() -> void {}

auto EasyRenderPipeline::on_render(VkContext& vk_context, const RenderInfo& render_info)
    -> vuk::Value<vuk::ImageAttachment> {
  ZoneScoped;

  auto* asset_man = App::get_asset_manager();

  bool rebuild_transforms = false;
  auto buffer_size = this->transforms_buffer ? this->transforms_buffer->size : 0;
  if (ox::size_bytes(this->transforms) > buffer_size) {
    if (this->transforms_buffer->buffer != VK_NULL_HANDLE) {
      // Device wait here is important, do not remove it. Why?
      // We are using ONE transform buffer for all frames, if
      // this buffer gets destroyed in the current frame, previous
      // rendering frame buffer will get corrupted and crash the GPU.
      vk_context.wait();
      this->transforms_buffer.reset();
    }

    this->transforms_buffer = vk_context.allocate_buffer_super(vuk::MemoryUsage::eGPUonly,
                                                               ox::size_bytes(this->transforms));

    rebuild_transforms = true;
  }

  auto transforms_buffer_value = vuk::Value<vuk::Buffer>{};
  if (this->transforms_buffer->buffer != VK_NULL_HANDLE) {
    transforms_buffer_value = vuk::acquire_buf("transforms_buffer", *this->transforms_buffer, vuk::Access::eMemoryRead);
  }

  if (rebuild_transforms) {
    transforms_buffer_value = vk_context.upload_staging(this->transforms, std::move(transforms_buffer_value));
  } else if (!this->dirty_transforms.empty()) {
    auto transform_count = this->dirty_transforms.size();
    auto new_transforms_size_bytes = transform_count * sizeof(GPU::Transforms);
    auto upload_buffer = vk_context.alloc_transient_buffer(vuk::MemoryUsage::eCPUonly, new_transforms_size_bytes);
    auto* dst_transform_ptr = reinterpret_cast<GPU::Transforms*>(upload_buffer->mapped_ptr);
    auto upload_offsets = std::vector<u64>(transform_count);

    for (const auto& [dirty_transform_id, offset] : std::views::zip(this->dirty_transforms, upload_offsets)) {
      auto index = SlotMap_decode_id(dirty_transform_id).index;
      const auto& transform = this->transforms[index];
      std::memcpy(dst_transform_ptr, &transform, sizeof(GPU::Transforms));
      offset = index * sizeof(GPU::Transforms);
      dst_transform_ptr++;
    }

    transforms_buffer_value = vuk::make_pass(
        "update scene transforms",
        [upload_off = std::move(upload_offsets)](vuk::CommandBuffer& cmd_list,
                                                 VUK_BA(vuk::Access::eTransferRead) src_buffer,
                                                 VUK_BA(vuk::Access::eTransferWrite) dst_buffer) {
          for (usize i = 0; i < upload_off.size(); i++) {
            auto offset = upload_off[i];
            auto src_subrange = src_buffer->subrange(i * sizeof(GPU::Transforms), sizeof(GPU::Transforms));
            auto dst_subrange = dst_buffer->subrange(offset, sizeof(GPU::Transforms));
            cmd_list.copy_buffer(src_subrange, dst_subrange);
          }

          return dst_buffer;
        })(std::move(upload_buffer), std::move(transforms_buffer_value));
  }

  camera_data.resolution = {render_info.extent.width, render_info.extent.height};
  auto camera_buffer = vk_context.scratch_buffer(std::span(&camera_data, 1));

  auto material_buffer = asset_man->get_materials_buffer(
      vk_context, *this->descriptor_set_01, BindlessID::SampledImages);
  this->descriptor_set_01->commit(*vk_context.runtime);

  render_queue_2d.update();
  render_queue_2d.sort();
  auto vertex_buffer_2d = vk_context.scratch_buffer(std::span(render_queue_2d.sprite_data));

  const vuk::Extent3D sky_view_lut_extent = {.width = 312, .height = 192, .depth = 1};
  const vuk::Extent3D sky_aerial_perspective_lut_extent = {.width = 32, .height = 32, .depth = 32};

  auto atmosphere_buffer = vuk::Value<vuk::Buffer>{};
  if (this->atmosphere.has_value()) {
    this->atmosphere->sky_view_lut_size = sky_view_lut_extent;
    this->atmosphere->aerial_perspective_lut_size = sky_aerial_perspective_lut_extent;
    this->atmosphere->transmittance_lut_size = sky_transmittance_lut_view.get_extent();
    this->atmosphere->multiscattering_lut_size = sky_multiscatter_lut_view.get_extent();
    atmosphere_buffer = vk_context.scratch_buffer(this->atmosphere);
  }
  auto sun_buffer = vuk::Value<vuk::Buffer>{};
  if (this->sun.has_value()) {
    sun_buffer = vk_context.scratch_buffer(this->sun);
  }

  const auto final_attachment_ia = vuk::ImageAttachment{
      .usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
      .extent = render_info.extent,
      .format = vuk::Format::eB10G11R11UfloatPack32,
      .sample_count = vuk::Samples::e1,
      .level_count = 1,
      .layer_count = 1,
  };
  auto final_attachment = vuk::clear_image(vuk::declare_ia("final_attachment", final_attachment_ia), vuk::Black<float>);

  auto result_attachment = vuk::declare_ia(
      "result",
      {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
       .format = render_info.format,
       .sample_count = vuk::Samples::e1});
  result_attachment.same_shape_as(final_attachment);
  result_attachment = vuk::clear_image(std::move(result_attachment), vuk::Black<f32>);

  const auto depth_ia = vuk::ImageAttachment{
      .extent = render_info.extent,
      .format = vuk::Format::eD32Sfloat,
      .sample_count = vuk::SampleCountFlagBits::e1,
      .level_count = 1,
      .layer_count = 1,
  };
  auto depth_attachment = vuk::clear_image(vuk::declare_ia("depth_image", depth_ia), vuk::DepthOne);

  auto sky_transmittance_lut_attachment = sky_transmittance_lut_view.acquire("sky_transmittance_lut",
                                                                             vuk::Access::eComputeSampled);
  auto sky_multiscatter_lut_attachment = sky_multiscatter_lut_view.acquire("sky_multiscatter_lut",
                                                                           vuk::Access::eComputeSampled);

  const auto debug_view = static_cast<GPU::DebugView>(RendererCVar::cvar_debug_view.get());
  const f32 debug_heatmap_scale = 5.0;
  const auto debugging = debug_view != GPU::DebugView::None;

  // --- 3D Pass ---
  if (!this->gpu_meshes.empty() && !this->gpu_meshlet_instances.empty()) {
    const auto cull_flags = static_cast<GPU::CullFlags>(RendererCVar::cvar_cull_flags.get()); // FIXME: Unsafe!

    buffer_size = this->meshes_buffer ? this->meshes_buffer->size : 0;
    if (ox::size_bytes(this->gpu_meshes) > buffer_size) {
      if (this->meshes_buffer->buffer != VK_NULL_HANDLE) {
        vk_context.wait();
        this->meshes_buffer.reset();
      }

      this->meshes_buffer = vk_context.allocate_buffer_super(vuk::MemoryUsage::eGPUonly,
                                                             ox::size_bytes(this->gpu_meshes));
    }

    buffer_size = this->meshlet_instances_buffer ? this->meshlet_instances_buffer->size : 0;
    if (ox::size_bytes(this->gpu_meshlet_instances) > buffer_size) {
      if (this->meshlet_instances_buffer->buffer != VK_NULL_HANDLE) {
        vk_context.wait();
        this->meshlet_instances_buffer.reset();
      }

      this->meshlet_instances_buffer = vk_context.allocate_buffer_super(vuk::MemoryUsage::eGPUonly,
                                                                        ox::size_bytes(this->gpu_meshlet_instances));
    }

    vuk::Value<vuk::Buffer> meshes_buffer_value;
    vuk::Value<vuk::Buffer> meshlet_instances_buffer_value;
    if (this->meshes_dirty) {
      meshes_buffer_value = vk_context.upload_staging(std::span(this->gpu_meshes), *this->meshes_buffer);
      meshlet_instances_buffer_value = vk_context.upload_staging(std::span(this->gpu_meshlet_instances),
                                                                 *this->meshlet_instances_buffer);
      this->meshes_dirty = false;
    } else {
      meshes_buffer_value = vuk::acquire_buf("meshes_buffer", *this->meshes_buffer, vuk::Access::eNone);
      meshlet_instances_buffer_value = vuk::acquire_buf(
          "meshlet_instances_buffer", *this->meshlet_instances_buffer, vuk::Access::eNone);
    }

    const auto hiz_extent = vuk::Extent3D{
        .width = (depth_ia.extent.width + 63_u32) & ~63_u32,
        .height = (depth_ia.extent.height + 63_u32) & ~63_u32,
        .depth = 1,
    };

    if (this->hiz_view.get_extent() != hiz_extent) {
      if (this->hiz_view) {
        this->hiz_view.destroy();
      }

      this->hiz_view.create({}, {.preset = Preset::eSTT2D, .format = vuk::Format::eR32Sfloat, .extent = hiz_extent});
      this->hiz_view.set_name("hiz");
    }
    auto hiz_attachment = this->hiz_view.acquire("hiz", vuk::eNone);

    const auto meshlet_instance_count = static_cast<u32>(this->gpu_meshlet_instances.size());

    auto cull_triangles_cmd_buffer = vk_context.scratch_buffer<vuk::DispatchIndirectCommand>({.x = 0, .y = 1, .z = 1});
    auto visible_meshlet_instances_indices_buffer = vk_context.alloc_transient_buffer(
        vuk::MemoryUsage::eGPUonly, meshlet_instance_count * sizeof(u32));

    std::tie(cull_triangles_cmd_buffer,
             visible_meshlet_instances_indices_buffer,
             meshlet_instances_buffer_value,
             transforms_buffer_value,
             meshes_buffer_value,
             camera_buffer,
             hiz_attachment) = vuk::make_pass( //
        "vis cull meshlets",
        [meshlet_instance_count, cull_flags](vuk::CommandBuffer& cmd_list,
                                             VUK_BA(vuk::eComputeWrite) cull_triangles_cmd,
                                             VUK_BA(vuk::eComputeWrite) visible_meshlet_instances_indices,
                                             VUK_BA(vuk::eComputeRead) meshlet_instances,
                                             VUK_BA(vuk::eComputeRead) transforms_,
                                             VUK_BA(vuk::eComputeRead) meshes,
                                             VUK_BA(vuk::eComputeRead) camera,
                                             VUK_IA(vuk::eComputeRead) hiz) {
          static constexpr auto sampler_min_clamp_reduction_mode = VkSamplerReductionModeCreateInfo{
              .sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO,
              .pNext = nullptr,
              .reductionMode = VK_SAMPLER_REDUCTION_MODE_MIN,
          };
          auto sampler_info = vuk::SamplerCreateInfo{
              .pNext = &sampler_min_clamp_reduction_mode,
              .magFilter = vuk::Filter::eLinear,
              .minFilter = vuk::Filter::eLinear,
              .mipmapMode = vuk::SamplerMipmapMode::eNearest,
              .addressModeU = vuk::SamplerAddressMode::eClampToEdge,
              .addressModeV = vuk::SamplerAddressMode::eClampToEdge,
          };

          cmd_list //
              .bind_compute_pipeline("cull_meshlets")
              .bind_buffer(0, 0, cull_triangles_cmd)
              .bind_buffer(0, 1, camera)
              .bind_buffer(0, 2, visible_meshlet_instances_indices)
              .bind_buffer(0, 3, meshlet_instances)
              .bind_buffer(0, 4, meshes)
              .bind_buffer(0, 5, transforms_)
              .bind_image(0, 6, hiz)
              .bind_sampler(0, 7, sampler_info)
              .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(meshlet_instance_count, cull_flags))
              .dispatch((meshlet_instance_count + Mesh::MAX_MESHLET_INDICES - 1) / Mesh::MAX_MESHLET_INDICES);
          return std::make_tuple(cull_triangles_cmd,
                                 visible_meshlet_instances_indices,
                                 meshlet_instances,
                                 transforms_,
                                 meshes,
                                 camera,
                                 hiz);
        })(std::move(cull_triangles_cmd_buffer),
           std::move(visible_meshlet_instances_indices_buffer),
           std::move(meshlet_instances_buffer_value),
           std::move(transforms_buffer_value),
           std::move(meshes_buffer_value),
           std::move(camera_buffer),
           std::move(hiz_attachment));

    auto draw_command_buffer = vk_context.scratch_buffer<vuk::DrawIndexedIndirectCommand>({.instanceCount = 1});
    auto reordered_indices_buffer = vk_context.alloc_transient_buffer(
        vuk::MemoryUsage::eGPUonly, meshlet_instance_count * Mesh::MAX_MESHLET_PRIMITIVES * 3 * sizeof(u32));

    std::tie(hiz_attachment,
             draw_command_buffer,
             visible_meshlet_instances_indices_buffer,
             reordered_indices_buffer,
             meshlet_instances_buffer_value,
             transforms_buffer_value,
             meshes_buffer_value,
             camera_buffer) = vuk::make_pass( //
        "vis cull triangles",
        [cull_flags](vuk::CommandBuffer& cmd_list,
                     VUK_IA(vuk::eComputeRead) hiz,
                     VUK_BA(vuk::eIndirectRead) cull_triangles_cmd,
                     VUK_BA(vuk::eComputeWrite) draw_indexed_cmd,
                     VUK_BA(vuk::eComputeRead) visible_meshlet_instances_indices,
                     VUK_BA(vuk::eComputeWrite) reordered_indices,
                     VUK_BA(vuk::eComputeRead) meshlet_instances,
                     VUK_BA(vuk::eComputeRead) transforms_,
                     VUK_BA(vuk::eComputeRead) meshes,
                     VUK_BA(vuk::eComputeRead) camera) {
          cmd_list //
              .bind_compute_pipeline("cull_triangles")
              .bind_buffer(0, 0, draw_indexed_cmd)
              .bind_buffer(0, 1, camera)
              .bind_buffer(0, 2, visible_meshlet_instances_indices)
              .bind_buffer(0, 3, reordered_indices)
              .bind_buffer(0, 4, meshlet_instances)
              .bind_buffer(0, 5, meshes)
              .bind_buffer(0, 6, transforms_)
              .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, cull_flags)
              .dispatch_indirect(cull_triangles_cmd);
          return std::make_tuple(hiz,
                                 draw_indexed_cmd,
                                 visible_meshlet_instances_indices,
                                 reordered_indices,
                                 meshlet_instances,
                                 transforms_,
                                 meshes,
                                 camera);
        })(std::move(hiz_attachment),
           std::move(cull_triangles_cmd_buffer),
           std::move(draw_command_buffer),
           std::move(visible_meshlet_instances_indices_buffer),
           std::move(reordered_indices_buffer),
           std::move(meshlet_instances_buffer_value),
           std::move(transforms_buffer_value),
           std::move(meshes_buffer_value),
           std::move(camera_buffer));

    auto visbuffer_data_attachment = vuk::declare_ia(
        "visbuffer data",
        {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
         .format = vuk::Format::eR32Uint,
         .sample_count = vuk::Samples::e1});
    visbuffer_data_attachment.same_shape_as(final_attachment);

    auto overdraw_attachment = vuk::declare_ia(
        "overdraw",
        {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
         .format = vuk::Format::eR32Uint,
         .sample_count = vuk::Samples::e1});
    overdraw_attachment.same_shape_as(final_attachment);

    std::tie(visbuffer_data_attachment, overdraw_attachment) = vuk::make_pass(
        "vis clear",
        []( //
            vuk::CommandBuffer& cmd_list,
            VUK_IA(vuk::eComputeWrite) visbuffer_data,
            VUK_IA(vuk::eComputeWrite) overdraw) {
          cmd_list //
              .bind_compute_pipeline("visbuffer_clear")
              .bind_image(0, 0, visbuffer_data)
              .bind_image(0, 1, overdraw)
              .push_constants(vuk::ShaderStageFlagBits::eCompute,
                              0,
                              PushConstants(glm::uvec2(visbuffer_data->extent.width, visbuffer_data->extent.height)))
              .dispatch_invocations_per_pixel(visbuffer_data);

          return std::make_tuple(visbuffer_data, overdraw);
        })(std::move(visbuffer_data_attachment), std::move(overdraw_attachment));

    std::tie(visbuffer_data_attachment,
             depth_attachment,
             camera_buffer,
             visible_meshlet_instances_indices_buffer,
             meshlet_instances_buffer_value,
             transforms_buffer_value,
             meshes_buffer_value,
             material_buffer,
             overdraw_attachment) = vuk::make_pass(   //
        "vis encode",
        [&descriptor_set = *this->descriptor_set_01]( //
            vuk::CommandBuffer& cmd_list,
            VUK_BA(vuk::eIndirectRead) triangle_indirect,
            VUK_BA(vuk::eIndexRead) index_buffer,
            VUK_IA(vuk::eColorWrite) visbuffer,
            VUK_IA(vuk::eDepthStencilRW) depth,
            VUK_BA(vuk::eVertexRead) camera,
            VUK_BA(vuk::eVertexRead) visible_meshlet_instances_indices,
            VUK_BA(vuk::eVertexRead) meshlet_instances,
            VUK_BA(vuk::eVertexRead) transforms_,
            VUK_BA(vuk::eVertexRead) meshes,
            VUK_BA(vuk::eFragmentRead) materials,
            VUK_IA(vuk::eFragmentRW) overdraw) {
          cmd_list //
              .bind_graphics_pipeline("visbuffer_encode")
              .set_rasterization({.cullMode = vuk::CullModeFlagBits::eBack})
              .set_depth_stencil(
                  {.depthTestEnable = true, .depthWriteEnable = true, .depthCompareOp = vuk::CompareOp::eLessOrEqual})
              .set_color_blend(visbuffer, vuk::BlendPreset::eOff)
              .set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
              .set_viewport(0, vuk::Rect2D::framebuffer())
              .set_scissor(0, vuk::Rect2D::framebuffer())
              .bind_persistent(1, descriptor_set)
              .bind_image(0, 0, overdraw)
              .bind_buffer(0, 1, camera)
              .bind_buffer(0, 2, visible_meshlet_instances_indices)
              .bind_buffer(0, 3, meshlet_instances)
              .bind_buffer(0, 4, meshes)
              .bind_buffer(0, 5, transforms_)
              .bind_buffer(0, 6, materials)
              .bind_index_buffer(index_buffer, vuk::IndexType::eUint32)
              .draw_indexed_indirect(1, triangle_indirect);
          return std::make_tuple(visbuffer,
                                 depth,
                                 camera,
                                 visible_meshlet_instances_indices,
                                 meshlet_instances,
                                 transforms_,
                                 meshes,
                                 materials,
                                 overdraw);
        })(std::move(draw_command_buffer),
           std::move(reordered_indices_buffer),
           std::move(visbuffer_data_attachment),
           std::move(depth_attachment),
           std::move(camera_buffer),
           std::move(visible_meshlet_instances_indices_buffer),
           std::move(meshlet_instances_buffer_value),
           std::move(transforms_buffer_value),
           std::move(meshes_buffer_value),
           std::move(material_buffer),
           std::move(overdraw_attachment));

    std::tie(depth_attachment, hiz_attachment) = vuk::make_pass(
        "hiz generate",
        [](vuk::CommandBuffer& cmd_list, VUK_IA(vuk::eComputeSampled) src, VUK_IA(vuk::eComputeRW) dst) {
          auto extent = dst->extent;
          auto mip_count = dst->level_count;
          OX_CHECK_LT(mip_count, 13u);

          auto dispatch_x = (extent.width + 63) >> 6;
          auto dispatch_y = (extent.height + 63) >> 6;

          static constexpr auto sampler_min_clamp_reduction_mode = VkSamplerReductionModeCreateInfo{
              .sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO,
              .pNext = nullptr,
              .reductionMode = VK_SAMPLER_REDUCTION_MODE_MIN,
          };
          auto sampler_info = vuk::SamplerCreateInfo{
              .pNext = &sampler_min_clamp_reduction_mode,
              .minFilter = vuk::Filter::eLinear,
              .mipmapMode = vuk::SamplerMipmapMode::eNearest,
              .addressModeU = vuk::SamplerAddressMode::eClampToEdge,
              .addressModeV = vuk::SamplerAddressMode::eClampToEdge,
          };

          cmd_list //
              .bind_compute_pipeline("hiz_pipeline")
              .push_constants(vuk::ShaderStageFlagBits::eCompute,
                              0,
                              PushConstants(mip_count, dispatch_x * dispatch_y, glm::mat2(1.0f)));

          *cmd_list.scratch_buffer<u32>(0, 0) = 0;
          cmd_list.bind_sampler(0, 1, sampler_info);
          cmd_list.bind_image(0, 2, src);

          for (u32 i = 0; i < 13; i++) {
            cmd_list.bind_image(0, i + 3, dst->mip(ox::min(i, mip_count - 1_u32)));
          }

          cmd_list.dispatch(dispatch_x, dispatch_y);

          return std::make_tuple(src, dst);
        })(std::move(depth_attachment), std::move(hiz_attachment));

    auto albedo_attachment = vuk::declare_ia(
        "albedo",
        {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
         .format = vuk::Format::eR8G8B8A8Srgb,
         .sample_count = vuk::Samples::e1});
    albedo_attachment.same_shape_as(visbuffer_data_attachment);
    albedo_attachment = vuk::clear_image(std::move(albedo_attachment), vuk::Black<f32>);

    auto normal_attachment = vuk::declare_ia(
        "normal",
        {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
         .format = vuk::Format::eR16G16B16A16Sfloat,
         .sample_count = vuk::Samples::e1});
    normal_attachment.same_shape_as(visbuffer_data_attachment);
    normal_attachment = vuk::clear_image(std::move(normal_attachment), vuk::Black<f32>);

    auto emissive_attachment = vuk::declare_ia(
        "emissive",
        {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
         .format = vuk::Format::eB10G11R11UfloatPack32,
         .sample_count = vuk::Samples::e1});
    emissive_attachment.same_shape_as(visbuffer_data_attachment);
    emissive_attachment = vuk::clear_image(std::move(emissive_attachment), vuk::Black<f32>);

    auto metallic_roughness_occlusion_attachment = vuk::declare_ia(
        "metallic roughness occlusion",
        {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
         .format = vuk::Format::eR8G8B8A8Unorm,
         .sample_count = vuk::Samples::e1});
    metallic_roughness_occlusion_attachment.same_shape_as(visbuffer_data_attachment);
    metallic_roughness_occlusion_attachment = vuk::clear_image(std::move(metallic_roughness_occlusion_attachment),
                                                               vuk::Black<f32>);

    std::tie(albedo_attachment,
             normal_attachment,
             emissive_attachment,
             metallic_roughness_occlusion_attachment,
             camera_buffer,
             visible_meshlet_instances_indices_buffer,
             meshlet_instances_buffer_value,
             transforms_buffer_value,
             material_buffer,
             visbuffer_data_attachment) = vuk::make_pass( //
        "vis decode",
        [&descriptor_set = *this->descriptor_set_01](vuk::CommandBuffer& cmd_list,
                                                     VUK_IA(vuk::eColorWrite) albedo,
                                                     VUK_IA(vuk::eColorWrite) normal,
                                                     VUK_IA(vuk::eColorWrite) emissive,
                                                     VUK_IA(vuk::eColorWrite) metallic_roughness_occlusion,
                                                     VUK_BA(vuk::eFragmentRead) camera,
                                                     VUK_BA(vuk::eFragmentRead) visible_meshlet_instances_indices,
                                                     VUK_BA(vuk::eFragmentRead) meshlet_instances,
                                                     VUK_BA(vuk::eFragmentRead) meshes,
                                                     VUK_BA(vuk::eFragmentRead) transforms_,
                                                     VUK_BA(vuk::eFragmentRead) materials,
                                                     VUK_IA(vuk::eFragmentSampled) visbuffer) {
          cmd_list //
              .bind_graphics_pipeline("visbuffer_decode")
              .set_rasterization({.cullMode = vuk::CullModeFlagBits::eNone})
              .set_depth_stencil({})
              .set_color_blend(albedo, vuk::BlendPreset::eOff)
              .set_color_blend(normal, vuk::BlendPreset::eOff)
              .set_color_blend(emissive, vuk::BlendPreset::eOff)
              .set_color_blend(metallic_roughness_occlusion, vuk::BlendPreset::eOff)
              .set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
              .set_viewport(0, vuk::Rect2D::framebuffer())
              .set_scissor(0, vuk::Rect2D::framebuffer())
              .bind_image(0, 0, visbuffer)
              .bind_buffer(0, 1, camera)
              .bind_buffer(0, 2, visible_meshlet_instances_indices)
              .bind_buffer(0, 3, meshlet_instances)
              .bind_buffer(0, 4, meshes)
              .bind_buffer(0, 5, transforms_)
              .bind_buffer(0, 6, materials)
              .bind_persistent(1, descriptor_set)
              .draw(3, 1, 0, 1);

          return std::make_tuple(albedo,
                                 normal,
                                 emissive,
                                 metallic_roughness_occlusion,
                                 camera,
                                 visible_meshlet_instances_indices,
                                 meshlet_instances,
                                 transforms_,
                                 materials,
                                 visbuffer);
        })(std::move(albedo_attachment),
           std::move(normal_attachment),
           std::move(emissive_attachment),
           std::move(metallic_roughness_occlusion_attachment),
           std::move(camera_buffer),
           std::move(visible_meshlet_instances_indices_buffer),
           std::move(meshlet_instances_buffer_value),
           std::move(meshes_buffer_value),
           std::move(transforms_buffer_value),
           std::move(material_buffer),
           std::move(visbuffer_data_attachment));

    if (!debugging && atmosphere.has_value() && sun.has_value()) {
      // --- BRDF ---
      auto brdf_pass = vuk::make_pass(
          "brdf",
          []( //
              vuk::CommandBuffer& cmd_list,
              VUK_IA(vuk::eColorWrite) dst,
              VUK_BA(vuk::eFragmentRead) atmosphere_,
              VUK_BA(vuk::eFragmentRead) sun_,
              VUK_BA(vuk::eFragmentRead) camera,
              VUK_IA(vuk::eFragmentSampled) sky_transmittance_lut,
              VUK_IA(vuk::eFragmentSampled) sky_multiscatter_lut,
              VUK_IA(vuk::eFragmentSampled) depth,
              VUK_IA(vuk::eFragmentSampled) albedo,
              VUK_IA(vuk::eFragmentSampled) normal,
              VUK_IA(vuk::eFragmentSampled) emissive,
              VUK_IA(vuk::eFragmentSampled) metallic_roughness_occlusion) {
            auto linear_clamp_sampler = vuk::SamplerCreateInfo{
                .magFilter = vuk::Filter::eLinear,
                .minFilter = vuk::Filter::eLinear,
                .addressModeU = vuk::SamplerAddressMode::eClampToEdge,
                .addressModeV = vuk::SamplerAddressMode::eClampToEdge,
                .addressModeW = vuk::SamplerAddressMode::eClampToEdge,
            };

            auto linear_repeat_sampler = vuk::SamplerCreateInfo{
                .magFilter = vuk::Filter::eLinear,
                .minFilter = vuk::Filter::eLinear,
                .addressModeU = vuk::SamplerAddressMode::eRepeat,
                .addressModeV = vuk::SamplerAddressMode::eRepeat,
            };

            cmd_list //
                .bind_graphics_pipeline("brdf")
                .set_rasterization({})
                .set_color_blend(dst, vuk::BlendPreset::eOff)
                .set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
                .set_viewport(0, vuk::Rect2D::framebuffer())
                .set_scissor(0, vuk::Rect2D::framebuffer())
                .bind_sampler(0, 0, linear_clamp_sampler)
                .bind_sampler(0, 1, linear_repeat_sampler)
                .bind_image(0, 2, sky_transmittance_lut)
                .bind_image(0, 3, sky_multiscatter_lut)
                .bind_image(0, 4, depth)
                .bind_image(0, 5, albedo)
                .bind_image(0, 6, normal)
                .bind_image(0, 7, emissive)
                .bind_image(0, 8, metallic_roughness_occlusion)
                .bind_buffer(0, 9, atmosphere_)
                .bind_buffer(0, 10, sun_)
                .bind_buffer(0, 11, camera)
                .draw(3, 1, 0, 0);
            return std::make_tuple(dst, atmosphere_, sun_, camera, sky_transmittance_lut, sky_multiscatter_lut, depth);
          });

      std::tie(final_attachment,
               atmosphere_buffer,
               sun_buffer,
               camera_buffer,
               sky_transmittance_lut_attachment,
               sky_multiscatter_lut_attachment,
               depth_attachment) = brdf_pass(std::move(final_attachment),
                                             std::move(atmosphere_buffer),
                                             std::move(sun_buffer),
                                             std::move(camera_buffer),
                                             std::move(sky_transmittance_lut_attachment),
                                             std::move(sky_multiscatter_lut_attachment),
                                             std::move(depth_attachment),
                                             std::move(albedo_attachment),
                                             std::move(normal_attachment),
                                             std::move(emissive_attachment),
                                             std::move(metallic_roughness_occlusion_attachment));
    } else {
      std::tie(result_attachment, depth_attachment, camera_buffer) = vuk::make_pass(
          "debug pass",
          [debug_view,
           debug_heatmap_scale]( //
              vuk::CommandBuffer& cmd_list,
              VUK_IA(vuk::eColorWrite) dst,
              VUK_IA(vuk::eFragmentSampled) visbuffer,
              VUK_IA(vuk::eFragmentSampled) depth,
              VUK_IA(vuk::eFragmentSampled) overdraw,
              VUK_IA(vuk::eFragmentSampled) albedo,
              VUK_IA(vuk::eFragmentSampled) normal,
              VUK_IA(vuk::eFragmentSampled) emissive,
              VUK_IA(vuk::eFragmentSampled) metallic_roughness_occlusion,
              VUK_BA(vuk::eFragmentRead) camera,
              VUK_BA(vuk::eFragmentRead) visible_meshlet_instances_indices) {
            auto linear_repeat_sampler = vuk::SamplerCreateInfo{
                .magFilter = vuk::Filter::eLinear,
                .minFilter = vuk::Filter::eLinear,
                .addressModeU = vuk::SamplerAddressMode::eRepeat,
                .addressModeV = vuk::SamplerAddressMode::eRepeat,
            };

            cmd_list //
                .bind_graphics_pipeline("debug")
                .set_rasterization({})
                .set_color_blend(dst, vuk::BlendPreset::eOff)
                .set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
                .set_viewport(0, vuk::Rect2D::framebuffer())
                .set_scissor(0, vuk::Rect2D::framebuffer())
                .bind_sampler(0, 0, linear_repeat_sampler)
                .bind_image(0, 1, visbuffer)
                .bind_image(0, 2, depth)
                .bind_image(0, 3, overdraw)
                .bind_image(0, 4, albedo)
                .bind_image(0, 5, normal)
                .bind_image(0, 6, emissive)
                .bind_image(0, 7, metallic_roughness_occlusion)
                .bind_buffer(0, 8, camera)
                .bind_buffer(0, 9, visible_meshlet_instances_indices)
                .push_constants(vuk::ShaderStageFlagBits::eFragment,
                                0,
                                PushConstants(std::to_underlying(debug_view), debug_heatmap_scale))
                .draw(3, 1, 0, 0);

            return std::make_tuple(dst, depth, camera);
          })(std::move(result_attachment),
             std::move(visbuffer_data_attachment),
             std::move(depth_attachment),
             std::move(overdraw_attachment),
             std::move(albedo_attachment),
             std::move(normal_attachment),
             std::move(emissive_attachment),
             std::move(metallic_roughness_occlusion_attachment),
             std::move(camera_buffer),
             std::move(visible_meshlet_instances_indices_buffer));
    }
  }

  // --- 2D Pass ---
  if (!render_queue_2d.sprite_data.empty()) {
    std::tie(final_attachment, //
             depth_attachment,
             camera_buffer,
             vertex_buffer_2d,
             material_buffer,
             transforms_buffer_value) =
        vuk::make_pass(
            "2d_forward_pass",
            [&rq2d = this->render_queue_2d,
             &descriptor_set = *this->descriptor_set_01](vuk::CommandBuffer& command_buffer,
                                                         VUK_IA(vuk::eColorWrite) target,
                                                         VUK_IA(vuk::eDepthStencilRW) depth,
                                                         VUK_BA(vuk::eVertexRead) vertex_buffer,
                                                         VUK_BA(vuk::eVertexRead) materials,
                                                         VUK_BA(vuk::eVertexRead) camera,
                                                         VUK_BA(vuk::eVertexRead) transforms_) {
              const auto vertex_pack_2d = vuk::Packed{
                  vuk::Format::eR32Uint, // 4 material_id
                  vuk::Format::eR32Uint, // 4 flags
                  vuk::Format::eR32Uint, // 4 transforms_id
              };

              for (const auto& batch : rq2d.batches) {
                if (batch.count < 1)
                  continue;

                command_buffer.bind_graphics_pipeline(batch.pipeline_name)
                    .set_depth_stencil(vuk::PipelineDepthStencilStateCreateInfo{
                        .depthTestEnable = true,
                        .depthWriteEnable = true,
                        .depthCompareOp = vuk::CompareOp::eGreaterOrEqual,
                    })
                    .set_dynamic_state(vuk::DynamicStateFlagBits::eScissor | vuk::DynamicStateFlagBits::eViewport)
                    .set_viewport(0, vuk::Rect2D::framebuffer())
                    .set_scissor(0, vuk::Rect2D::framebuffer())
                    .broadcast_color_blend(vuk::BlendPreset::eAlphaBlend)
                    .set_rasterization({.cullMode = vuk::CullModeFlagBits::eNone})
                    .bind_vertex_buffer(0, vertex_buffer, 0, vertex_pack_2d, vuk::VertexInputRate::eInstance)
                    .bind_persistent(1, descriptor_set)
                    .push_constants(
                        vuk::ShaderStageFlagBits::eVertex | vuk::ShaderStageFlagBits::eFragment,
                        0,
                        PushConstants(materials->device_address, camera->device_address, transforms_->device_address))
                    .draw(6, batch.count, 0, batch.offset);
              }

              return std::make_tuple(target, depth, camera, vertex_buffer, materials, transforms_);
            })(std::move(final_attachment),
               std::move(depth_attachment),
               std::move(vertex_buffer_2d),
               std::move(material_buffer),
               std::move(camera_buffer),
               std::move(transforms_buffer_value));
  }

  // --- Atmosphere Pass ---
  if (this->atmosphere.has_value() && this->sun.has_value() && !debugging) {
    auto sky_view_lut_attachment = vuk::declare_ia(
        "sky_view_lut",
        {.image_type = vuk::ImageType::e2D,
         .usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage,
         .extent = sky_view_lut_extent,
         .format = vuk::Format::eR16G16B16A16Sfloat,
         .sample_count = vuk::Samples::e1,
         .view_type = vuk::ImageViewType::e2D,
         .level_count = 1,
         .layer_count = 1});

    auto sky_aerial_perspective_attachment = vuk::declare_ia(
        "sky aerial perspective",
        {.image_type = vuk::ImageType::e3D,
         .usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage,
         .extent = sky_aerial_perspective_lut_extent,
         .sample_count = vuk::Samples::e1,
         .view_type = vuk::ImageViewType::e3D,
         .level_count = 1,
         .layer_count = 1});
    sky_aerial_perspective_attachment.same_format_as(sky_view_lut_attachment);

    std::tie(sky_view_lut_attachment,
             sky_transmittance_lut_attachment,
             sky_multiscatter_lut_attachment,
             atmosphere_buffer,
             sun_buffer,
             camera_buffer) = vuk::make_pass( //
        "sky view",
        [&descriptor_set = *this->descriptor_set_01](vuk::CommandBuffer& cmd_list,
                                                     VUK_BA(vuk::eComputeRead) atmosphere_,
                                                     VUK_BA(vuk::eComputeRead) sun_,
                                                     VUK_BA(vuk::eComputeRead) camera,
                                                     VUK_IA(vuk::eComputeSampled) sky_transmittance_lut,
                                                     VUK_IA(vuk::eComputeSampled) sky_multiscatter_lut,
                                                     VUK_IA(vuk::eComputeRW) sky_view_lut) {
          cmd_list.bind_compute_pipeline("sky_view_pipeline")
              .bind_persistent(1, descriptor_set)
              .bind_image(0, 0, sky_transmittance_lut)
              .bind_image(0, 1, sky_multiscatter_lut)
              .bind_image(0, 2, sky_view_lut)
              .bind_buffer(0, 3, atmosphere_)
              .bind_buffer(0, 4, sun_)
              .bind_buffer(0, 5, camera)
              .dispatch_invocations_per_pixel(sky_view_lut);

          return std::make_tuple(sky_view_lut, sky_transmittance_lut, sky_multiscatter_lut, atmosphere_, sun_, camera);
        })(std::move(atmosphere_buffer),
           std::move(sun_buffer),
           std::move(camera_buffer),
           std::move(sky_transmittance_lut_attachment),
           std::move(sky_multiscatter_lut_attachment),
           std::move(sky_view_lut_attachment));

    std::tie(sky_aerial_perspective_attachment,
             sky_transmittance_lut_attachment,
             atmosphere_buffer,
             sun_buffer,
             camera_buffer) = vuk::make_pass( //
        "sky aerial perspective",
        [&descriptor_set = *this->descriptor_set_01](vuk::CommandBuffer& cmd_list,
                                                     VUK_BA(vuk::eComputeRead) atmosphere_,
                                                     VUK_BA(vuk::eComputeRead) sun_,
                                                     VUK_BA(vuk::eComputeRead) camera,
                                                     VUK_IA(vuk::eComputeSampled) sky_transmittance_lut,
                                                     VUK_IA(vuk::eComputeSampled) sky_multiscatter_lut,
                                                     VUK_IA(vuk::eComputeRW) sky_aerial_perspective_lut) {
          cmd_list.bind_compute_pipeline("sky_aerial_perspective_pipeline")
              .bind_persistent(1, descriptor_set)
              .bind_image(0, 0, sky_transmittance_lut)
              .bind_image(0, 1, sky_multiscatter_lut)
              .bind_image(0, 2, sky_aerial_perspective_lut)
              .bind_buffer(0, 3, atmosphere_)
              .bind_buffer(0, 4, sun_)
              .bind_buffer(0, 5, camera)
              .dispatch_invocations_per_pixel(sky_aerial_perspective_lut);

          return std::make_tuple(sky_aerial_perspective_lut, sky_transmittance_lut, atmosphere_, sun_, camera);
        })(std::move(atmosphere_buffer),
           std::move(sun_buffer),
           std::move(camera_buffer),
           std::move(sky_transmittance_lut_attachment),
           std::move(sky_multiscatter_lut_attachment),
           std::move(sky_aerial_perspective_attachment));

    std::tie(final_attachment, depth_attachment, camera_buffer) = vuk::make_pass(
        "sky final",
        [&descriptor_set = *this->descriptor_set_01](vuk::CommandBuffer& cmd_list,
                                                     VUK_IA(vuk::eColorWrite) dst,
                                                     VUK_BA(vuk::eFragmentRead) atmosphere_,
                                                     VUK_BA(vuk::eFragmentRead) sun_,
                                                     VUK_BA(vuk::eFragmentRead) camera,
                                                     VUK_IA(vuk::eFragmentSampled) sky_transmittance_lut,
                                                     VUK_IA(vuk::eFragmentSampled) sky_aerial_perspective_lut,
                                                     VUK_IA(vuk::eFragmentSampled) sky_view_lut,
                                                     VUK_IA(vuk::eFragmentSampled) depth) {
          vuk::PipelineColorBlendAttachmentState blend_info = {
              .blendEnable = true,
              .srcColorBlendFactor = vuk::BlendFactor::eOne,
              .dstColorBlendFactor = vuk::BlendFactor::eSrcAlpha,
              .colorBlendOp = vuk::BlendOp::eAdd,
              .srcAlphaBlendFactor = vuk::BlendFactor::eZero,
              .dstAlphaBlendFactor = vuk::BlendFactor::eOne,
              .alphaBlendOp = vuk::BlendOp::eAdd,
          };

          cmd_list.bind_graphics_pipeline("sky_final_pipeline")
              .bind_persistent(1, descriptor_set)
              .set_rasterization({})
              .set_depth_stencil({})
              .set_color_blend(dst, blend_info)
              .set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
              .set_viewport(0, vuk::Rect2D::framebuffer())
              .set_scissor(0, vuk::Rect2D::framebuffer())
              .bind_image(0, 0, sky_transmittance_lut)
              .bind_image(0, 1, sky_aerial_perspective_lut)
              .bind_image(0, 2, sky_view_lut)
              .bind_image(0, 3, depth)
              .bind_buffer(0, 4, atmosphere_)
              .bind_buffer(0, 5, sun_)
              .bind_buffer(0, 6, camera)
              .draw(3, 1, 0, 0);

          return std::make_tuple(dst, depth, camera);
        })(std::move(final_attachment),
           std::move(atmosphere_buffer),
           std::move(sun_buffer),
           std::move(camera_buffer),
           std::move(sky_transmittance_lut_attachment),
           std::move(sky_aerial_perspective_attachment),
           std::move(sky_view_lut_attachment),
           std::move(depth_attachment));
  }

  // --- Post Processing ---
  if (!debugging) {
    PassConfig pass_config_flags = PassConfig::None;
    if (static_cast<bool>(RendererCVar::cvar_bloom_enable.get()))
      pass_config_flags |= PassConfig::EnableBloom;
    if (static_cast<bool>(RendererCVar::cvar_fxaa_enable.get()))
      pass_config_flags |= PassConfig::EnableFXAA;

    // --- FXAA Pass ---
    if (pass_config_flags & PassConfig::EnableFXAA) {
      final_attachment = vuk::make_pass("fxaa", [](vuk::CommandBuffer& cmd_list, VUK_IA(vuk::eColorWrite) out) {
        const glm::vec2 inverse_screen_size = 1.f / glm::vec2(out->extent.width, out->extent.height);
        cmd_list.bind_graphics_pipeline("fxaa_pipeline")
            .bind_image(0, 0, out)
            .set_rasterization({})
            .set_color_blend(out, {})
            .set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
            .set_viewport(0, vuk::Rect2D::framebuffer())
            .set_scissor(0, vuk::Rect2D::framebuffer())
            .bind_sampler(0, 1, vuk::LinearSamplerClamped)
            .push_constants(vuk::ShaderStageFlagBits::eFragment, 0, PushConstants(inverse_screen_size))
            .draw(3, 1, 0, 0);
        return out;
      })(final_attachment);
    }

    // --- Bloom Pass ---
    const f32 bloom_threshold = RendererCVar::cvar_bloom_threshold.get();
    const f32 bloom_clamp = RendererCVar::cvar_bloom_clamp.get();
    const u32 bloom_mip_count = static_cast<u32>(RendererCVar::cvar_bloom_mips.get());

    auto bloom_ia = vuk::ImageAttachment{
        .format = vuk::Format::eB10G11R11UfloatPack32,
        .sample_count = vuk::SampleCountFlagBits::e1,
        .level_count = bloom_mip_count,
        .layer_count = 1,
    };

    auto bloom_down_image = vuk::clear_image(vuk::declare_ia("bloom_down_image", bloom_ia), vuk::Black<float>);
    bloom_down_image.same_extent_as(final_attachment);
    bloom_down_image.same_format_as(final_attachment);

    bloom_ia.level_count = bloom_mip_count - 1;
    auto bloom_up_image = vuk::clear_image(vuk::declare_ia("bloom_up_image", bloom_ia), vuk::Black<float>);
    bloom_up_image.same_extent_as(final_attachment);

    if (pass_config_flags & PassConfig::EnableBloom) {
      std::tie(final_attachment, bloom_down_image) = vuk::make_pass(
          "bloom_prefilter",
          [bloom_threshold,
           bloom_clamp](vuk::CommandBuffer& cmd_list, VUK_IA(vuk::eComputeRead) src, VUK_IA(vuk::eComputeRW) out) {
            cmd_list //
                .bind_compute_pipeline("bloom_prefilter_pipeline")
                .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(bloom_threshold, bloom_clamp))
                .bind_image(0, 0, out)
                .bind_image(0, 1, src)
                .bind_sampler(0, 2, vuk::NearestMagLinearMinSamplerClamped)
                .dispatch_invocations_per_pixel(src);

            return std::make_tuple(src, out);
          })(final_attachment, bloom_down_image.mip(0));

      auto converge = vuk::make_pass(
          "bloom_converge", [](vuk::CommandBuffer& command_buffer, VUK_IA(vuk::eComputeRW) output) { return output; });
      auto prefiltered_downsample_image = converge(bloom_down_image);
      auto src_mip = prefiltered_downsample_image.mip(0);

      for (uint32_t i = 1; i < bloom_mip_count; i++) {
        src_mip = vuk::make_pass(
            "bloom_downsample",
            [](vuk::CommandBuffer& command_buffer, VUK_IA(vuk::eComputeSampled) src, VUK_IA(vuk::eComputeRW) out) {
              command_buffer.bind_compute_pipeline("bloom_downsample_pipeline")
                  .bind_image(0, 0, out)
                  .bind_image(0, 1, src)
                  .bind_sampler(0, 2, vuk::LinearMipmapNearestSamplerClamped)
                  .dispatch_invocations_per_pixel(src);
              return out;
            })(src_mip, prefiltered_downsample_image.mip(i));
      }

      // Upsampling
      // https://www.froyok.fr/blog/2021-12-ue4-custom-bloom/resources/code/bloom_down_up_demo.jpg

      auto downsampled_image = converge(prefiltered_downsample_image);
      auto upsample_src_mip = downsampled_image.mip(bloom_mip_count - 1);

      for (int32_t i = (int32_t)bloom_mip_count - 2; i >= 0; i--) {
        upsample_src_mip = vuk::make_pass( //
            "bloom_upsample",
            [](vuk::CommandBuffer& command_buffer,
               VUK_IA(vuk::eComputeRW) out,
               VUK_IA(vuk::eComputeSampled) src1,
               VUK_IA(vuk::eComputeSampled) src2) {
              command_buffer.bind_compute_pipeline("bloom_upsample_pipeline")
                  .bind_image(0, 0, out)
                  .bind_image(0, 1, src1)
                  .bind_image(0, 2, src2)
                  .bind_sampler(0, 3, vuk::NearestMagLinearMinSamplerClamped)
                  .dispatch_invocations_per_pixel(out);

              return out;
            })(bloom_up_image.mip(i), upsample_src_mip, downsampled_image.mip(i));
      }
    }

    // --- Auto Exposure Pass ---
    auto histogram_inf = this->histogram_info.value_or(GPU::HistogramInfo{});

    auto histogram_buffer = vk_context.alloc_transient_buffer(vuk::MemoryUsage::eGPUonly,
                                                              GPU::HISTOGRAM_BIN_COUNT * sizeof(u32));
    vuk::fill(histogram_buffer, 0);

    std::tie(final_attachment, histogram_buffer) = vuk::make_pass(
        "histogram generate",
        [histogram_inf](
            vuk::CommandBuffer& cmd_list, VUK_IA(vuk::eComputeRead) src, VUK_BA(vuk::eComputeRW) histogram) {
          cmd_list.bind_compute_pipeline("histogram_generate_pipeline")
              .bind_image(0, 0, src)
              .push_constants(vuk::ShaderStageFlagBits::eCompute,
                              0,
                              PushConstants( //
                                  histogram->device_address,
                                  glm::uvec2(src->extent.width, src->extent.height),
                                  histogram_inf.min_exposure,
                                  1.0f / (histogram_inf.max_exposure - histogram_inf.min_exposure)))
              .dispatch_invocations_per_pixel(src);

          return std::make_tuple(src, histogram);
        })(std::move(final_attachment), std::move(histogram_buffer));

    auto exposure_buffer_value = vuk::acquire_buf("exposure buffer", *this->exposure_buffer, vuk::eNone);

    if (histogram_info.has_value()) {
      exposure_buffer_value = vuk::make_pass(
          "histogram average",
          [pixel_count = f32(final_attachment->extent.width * final_attachment->extent.height), histogram_inf](
              vuk::CommandBuffer& cmd_list, VUK_BA(vuk::eComputeRW) histogram, VUK_BA(vuk::eComputeRW) exposure) {
            auto adaptation_time = glm::clamp(
                static_cast<f32>(
                    1.0f - glm::exp(-histogram_inf.adaptation_speed * App::get()->get_timestep().get_millis() * 0.001)),
                0.0f,
                1.0f);

            cmd_list //
                .bind_compute_pipeline("histogram_average_pipeline")
                .push_constants(vuk::ShaderStageFlagBits::eCompute,
                                0,
                                PushConstants(histogram->device_address,
                                              exposure->device_address,
                                              pixel_count,
                                              histogram_inf.min_exposure,
                                              histogram_inf.max_exposure - histogram_inf.min_exposure,
                                              adaptation_time,
                                              histogram_inf.ev100_bias))
                .dispatch(1);

            return exposure;
          })(std::move(histogram_buffer), std::move(exposure_buffer_value));
    }

    // --- Tonemap Pass ---
    result_attachment = vuk::make_pass(
        "tonemap",
        [pass_config_flags](vuk::CommandBuffer& cmd_list,
                            VUK_IA(vuk::eColorWrite) dst,
                            VUK_IA(vuk::eFragmentSampled) src,
                            VUK_IA(vuk::eFragmentSampled) bloom_src,
                            VUK_BA(vuk::eFragmentRead) exposure) {
          cmd_list.bind_graphics_pipeline("tonemap_pipeline")
              .set_rasterization({})
              .set_color_blend(dst, vuk::BlendPreset::eOff)
              .set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
              .set_viewport(0, vuk::Rect2D::framebuffer())
              .set_scissor(0, vuk::Rect2D::framebuffer())
              .bind_sampler(0, 0, {.magFilter = vuk::Filter::eLinear, .minFilter = vuk::Filter::eLinear})
              .bind_image(0, 1, src)
              .bind_image(0, 2, bloom_src)
              .push_constants(
                  vuk::ShaderStageFlagBits::eFragment, 0, PushConstants(exposure->device_address, pass_config_flags))
              .draw(3, 1, 0, 0);

          return dst;
        })(std::move(result_attachment),
           std::move(final_attachment),
           std::move(bloom_up_image),
           std::move(exposure_buffer_value));
  }

  return result_attachment;
}

auto EasyRenderPipeline::on_update(ox::Scene* scene) -> void {
  ZoneScoped;

  auto* asset_man = App::get_asset_manager();

  this->transforms = scene->transforms.slots_unsafe();
  this->dirty_transforms = scene->dirty_transforms;

  CameraComponent current_camera = {};
  CameraComponent frozen_camera = {};
  const auto freeze_culling = static_cast<bool>(RendererCVar::cvar_freeze_culling_frustum.get());

  scene->world
      .query_builder<const TransformComponent, const CameraComponent>() //
      .build()
      .each([&](flecs::entity e, const TransformComponent& tc, const CameraComponent& c) {
        if (freeze_culling && !this->saved_camera) {
          this->saved_camera = true;
          frozen_camera = current_camera;
        } else if (!freeze_culling && this->saved_camera) {
          this->saved_camera = false;
        }

        if (static_cast<bool>(RendererCVar::cvar_freeze_culling_frustum.get()) &&
            static_cast<bool>(RendererCVar::cvar_draw_camera_frustum.get())) {
          const auto proj = frozen_camera.get_projection_matrix() * frozen_camera.get_view_matrix();
          DebugRenderer::draw_frustum(proj, glm::vec4(0, 1, 0, 1), 1.0f, 0.0f); // reversed-z
        }

        current_camera = c;
      });

  CameraComponent cam = freeze_culling ? frozen_camera : current_camera;

  this->camera_data = GPU::CameraData{
      .position = glm::vec4(cam.position, 0.0f),
      .projection = cam.get_projection_matrix(),
      .inv_projection = cam.get_inv_projection_matrix(),
      .view = cam.get_view_matrix(),
      .inv_view = cam.get_inv_view_matrix(),
      .projection_view = cam.get_projection_matrix() * cam.get_view_matrix(),
      .inv_projection_view = cam.get_inverse_projection_view(),
      .previous_projection = cam.get_projection_matrix(),
      .previous_inv_projection = cam.get_inv_projection_matrix(),
      .previous_view = cam.get_view_matrix(),
      .previous_inv_view = cam.get_inv_view_matrix(),
      .previous_projection_view = cam.get_projection_matrix() * cam.get_view_matrix(),
      .previous_inv_projection_view = cam.get_inverse_projection_view(),
      .temporalaa_jitter = cam.jitter,
      .temporalaa_jitter_prev = cam.jitter_prev,
      .near_clip = cam.near_clip,
      .far_clip = cam.far_clip,
      .fov = cam.fov,
      .output_index = 0,
  };

  math::calc_frustum_planes(camera_data.projection_view, camera_data.frustum_planes);

  option<GPU::Atmosphere> atmosphere_data = nullopt;
  option<GPU::Sun> sun_data = nullopt;

  scene->world
      .query_builder<const TransformComponent, const LightComponent>() //
      .build()
      .each(
          [&sun_data, &atmosphere_data, cam](flecs::entity e, const TransformComponent& tc, const LightComponent& lc) {
            if (lc.type == LightComponent::LightType::Directional) {
              auto& sund = sun_data.emplace();
              sund.direction.x = glm::cos(tc.rotation.x) * glm::sin(tc.rotation.y);
              sund.direction.y = glm::sin(tc.rotation.x) * glm::sin(tc.rotation.y);
              sund.direction.z = glm::cos(tc.rotation.y);
              sund.intensity = lc.intensity;
            }

            if (e.has<AtmosphereComponent>()) {
              const auto& atmos_info = *e.get<AtmosphereComponent>();
              auto& atmos = atmosphere_data.emplace();
              atmos.rayleigh_scatter = atmos_info.rayleigh_scattering * 1e-3f;
              atmos.rayleigh_density = atmos_info.rayleigh_density;
              atmos.mie_scatter = atmos_info.mie_scattering * 1e-3f;
              atmos.mie_density = atmos_info.mie_density;
              atmos.mie_extinction = atmos_info.mie_extinction * 1e-3f;
              atmos.mie_asymmetry = atmos_info.mie_asymmetry;
              atmos.ozone_absorption = atmos_info.ozone_absorption * 1e-3f;
              atmos.ozone_height = atmos_info.ozone_height;
              atmos.ozone_thickness = atmos_info.ozone_thickness;
              atmos.aerial_perspective_start_km = atmos_info.aerial_perspective_start_km;

              f32 eye_altitude = cam.position.y * GPU::CAMERA_SCALE_UNIT;
              eye_altitude += atmos.planet_radius + GPU::PLANET_RADIUS_OFFSET;
              atmos.eye_position = glm::vec3(0.0f, eye_altitude, 0.0f);
            }
          });

  this->atmosphere = atmosphere_data;
  this->sun = sun_data;

  if (scene->meshes_dirty) {
    this->meshes_dirty = true;

    this->gpu_meshes.clear();
    this->gpu_meshlet_instances.clear();

    for (const auto& [rendering_mesh, transform_ids] : scene->rendering_meshes_map) {
      auto* model = asset_man->get_mesh(rendering_mesh.first);
      const auto& mesh = model->meshes[rendering_mesh.second];

      // Per mesh info
      auto mesh_offset = static_cast<u32>(this->gpu_meshes.size());
      auto& gpu_mesh = this->gpu_meshes.emplace_back();
      gpu_mesh.indices = model->indices->device_address;
      gpu_mesh.vertex_positions = model->vertex_positions->device_address;
      gpu_mesh.vertex_normals = model->vertex_normals->device_address;
      gpu_mesh.texture_coords = model->texture_coords->device_address;
      gpu_mesh.local_triangle_indices = model->local_triangle_indices->device_address;
      gpu_mesh.meshlet_bounds = model->meshlet_bounds->device_address;
      gpu_mesh.meshlets = model->meshlets->device_address;

      // Instancing
      for (const auto transform_id : transform_ids) {
        for (const auto primitive_index : mesh.primitive_indices) {
          auto& primitive = model->primitives[primitive_index];
          for (u32 meshlet_index = 0; meshlet_index < primitive.meshlet_count; meshlet_index++) {
            auto& meshlet_instance = gpu_meshlet_instances.emplace_back();
            meshlet_instance.mesh_index = mesh_offset;
            meshlet_instance.material_index = primitive.material_index;
            meshlet_instance.transform_index = SlotMap_decode_id(transform_id).index;
            meshlet_instance.meshlet_index = meshlet_index + primitive.meshlet_offset;
          }
        }
      }
    }

    scene->meshes_dirty = false;
  }

  this->render_queue_2d.init();

  scene->world
      .query_builder<const TransformComponent, const SpriteComponent>() //
      .build()
      .each([asset_man, &scene, &cam, &rq2d = this->render_queue_2d](
                flecs::entity e, const TransformComponent& tc, const SpriteComponent& comp) {
        const auto distance = glm::distance(glm::vec3(0.f, 0.f, cam.position.z), glm::vec3(0.f, 0.f, tc.position.z));
        if (auto* material = asset_man->get_asset(comp.material)) {
          if (auto transform_id = scene->get_entity_transform_id(e)) {
            rq2d.add(comp,
                     tc.position.y,
                     SlotMap_decode_id(*transform_id).index,
                     SlotMap_decode_id(material->material_id).index,
                     distance);
          } else {
            OX_LOG_WARN("No registered transform for sprite entity: {}", e.name().c_str());
          }
        }
      });

  option<GPU::HistogramInfo> hist_info = nullopt;

  scene->world
      .query_builder<const AutoExposureComponent>() //
      .build()
      .each([&hist_info](flecs::entity e, const AutoExposureComponent& c) {
        auto& i = hist_info.emplace();
        i.max_exposure = c.max_exposure;
        i.min_exposure = c.min_exposure;
        i.adaptation_speed = c.adaptation_speed;
        i.ev100_bias = c.ev100_bias;
      });

  this->histogram_info = hist_info;
}
} // namespace ox
