#include "Render/RendererInstance.hpp"

#include "Asset/AssetManager.hpp"
#include "Asset/Mesh.hpp"
#include "Asset/Texture.hpp"
#include "Core/App.hpp"
#include "Render/DebugRenderer.hpp"
#include "Render/RendererConfig.hpp"
#include "Render/Utils/VukCommon.hpp"
#include "Render/Vulkan/VkContext.hpp"
#include "Scene/SceneGPU.hpp"

namespace ox {
RendererInstance::RendererInstance(Scene* owner_scene, Renderer& parent_renderer)
    : scene(owner_scene),
      renderer(parent_renderer) {

  render_queue_2d.init();
}

RendererInstance::~RendererInstance() {}

auto RendererInstance::render(this RendererInstance& self, const Renderer::RenderInfo& render_info)
    -> vuk::Value<vuk::ImageAttachment> {
  ZoneScoped;

  self.viewport_offset = render_info.viewport_offset;

  auto& vk_context = App::get_vkcontext();
  auto& bindless_set = vk_context.get_descriptor_set();

  self.camera_data.resolution = {render_info.extent.width, render_info.extent.height};
  auto camera_buffer = self.renderer.vk_context->scratch_buffer(std::span(&self.camera_data, 1));

  auto material_buffer = vuk::Value<vuk::Buffer>{};

  self.render_queue_2d.update();
  self.render_queue_2d.sort();
  auto vertex_buffer_2d = self.renderer.vk_context->scratch_buffer(std::span(self.render_queue_2d.sprite_data));

  const vuk::Extent3D sky_view_lut_extent = {.width = 312, .height = 192, .depth = 1};
  const vuk::Extent3D sky_aerial_perspective_lut_extent = {.width = 32, .height = 32, .depth = 32};

  auto atmosphere_buffer = vuk::Value<vuk::Buffer>{};
  if (self.atmosphere.has_value()) {
    self.atmosphere->sky_view_lut_size = sky_view_lut_extent;
    self.atmosphere->aerial_perspective_lut_size = sky_aerial_perspective_lut_extent;
    self.atmosphere->transmittance_lut_size = self.renderer.sky_transmittance_lut_view.get_extent();
    self.atmosphere->multiscattering_lut_size = self.renderer.sky_multiscatter_lut_view.get_extent();
    atmosphere_buffer = self.renderer.vk_context->scratch_buffer(self.atmosphere);
  }
  auto sun_buffer = vuk::Value<vuk::Buffer>{};
  if (self.sun.has_value()) {
    sun_buffer = self.renderer.vk_context->scratch_buffer(self.sun);
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
  auto depth_attachment = vuk::clear_image(vuk::declare_ia("depth_image", depth_ia), vuk::DepthZero);

  auto sky_transmittance_lut_attachment = self.renderer.sky_transmittance_lut_view.acquire(
      "sky_transmittance_lut", vuk::Access::eComputeSampled);
  auto sky_multiscatter_lut_attachment = self.renderer.sky_multiscatter_lut_view.acquire("sky_multiscatter_lut",
                                                                                         vuk::Access::eComputeSampled);

  const auto debug_view = static_cast<GPU::DebugView>(RendererCVar::cvar_debug_view.get());
  const f32 debug_heatmap_scale = 5.0;
  const auto debugging = debug_view != GPU::DebugView::None;

  auto transforms_buffer = std::move(self.prepared_frame.transforms_buffer);

  // --- 3D Pass ---
  if (self.prepared_frame.mesh_instance_count > 0) {
    auto meshes_buffer = std::move(self.prepared_frame.meshes_buffer);
    auto mesh_instances_buffer = std::move(self.prepared_frame.mesh_instances_buffer);
    auto meshlet_instances_buffer = std::move(self.prepared_frame.meshlet_instances_buffer);
    auto materials_buffer = std::move(self.prepared_frame.materials_buffer);
    auto reordered_indices_buffer = std::move(self.prepared_frame.reordered_indices_buffer);

    auto cull_flags = GPU::CullFlags::MicroTriangles | GPU::CullFlags::TriangleBackFace;
    if (static_cast<bool>(RendererCVar::cvar_culling_frustum.get())) {
      cull_flags |= GPU::CullFlags::MeshletFrustum;
    }
    if (static_cast<bool>(RendererCVar::cvar_culling_occlusion.get())) {
      cull_flags |= GPU::CullFlags::OcclusionCulling;
    }
    if (static_cast<bool>(RendererCVar::cvar_culling_triangle.get())) {
      cull_flags |= GPU::CullFlags::TriangleCulling;
    }

    const auto hiz_extent = vuk::Extent3D{
        .width = (depth_ia.extent.width + 63_u32) & ~63_u32,
        .height = (depth_ia.extent.height + 63_u32) & ~63_u32,
        .depth = 1,
    };

    static constexpr auto sampler_min_clamp_reduction_mode = VkSamplerReductionModeCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO,
        .pNext = nullptr,
        .reductionMode = VK_SAMPLER_REDUCTION_MODE_MIN,
    };
    static constexpr auto hiz_sampler_info = vuk::SamplerCreateInfo{
        .pNext = &sampler_min_clamp_reduction_mode,
        .magFilter = vuk::Filter::eLinear,
        .minFilter = vuk::Filter::eLinear,
        .mipmapMode = vuk::SamplerMipmapMode::eNearest,
        .addressModeU = vuk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vuk::SamplerAddressMode::eClampToEdge,
    };

    auto hiz_attachment = vuk::Value<vuk::ImageAttachment>{};
    if (self.hiz_view.get_extent() != hiz_extent) {
      if (self.hiz_view) {
        self.hiz_view.destroy();
      }

      self.hiz_view.create({}, {.preset = Preset::eSTT2D, .format = vuk::Format::eR32Sfloat, .extent = hiz_extent});
      self.hiz_view.set_name("hiz");

      hiz_attachment = self.hiz_view.acquire("hiz", vuk::eNone);
      hiz_attachment = vuk::clear_image(std::move(hiz_attachment), vuk::DepthZero);
    } else {
      hiz_attachment = self.hiz_view.acquire("hiz", vuk::eComputeRW);
    }

    auto vis_cull_meshes = vuk::make_pass(
        "vis cull meshes",
        [mesh_instance_count = self.prepared_frame.mesh_instance_count, cull_flags]( //
            vuk::CommandBuffer& cmd_list,
            VUK_BA(vuk::eComputeRead) camera,
            VUK_BA(vuk::eComputeRead) meshes,
            VUK_BA(vuk::eComputeRead) transforms,
            VUK_BA(vuk::eComputeRW) mesh_instances,
            VUK_BA(vuk::eComputeRW) meshlet_instances,
            VUK_BA(vuk::eComputeRW) visible_meshlet_instances_count) {
          cmd_list //
              .bind_compute_pipeline("cull_meshes")
              .bind_buffer(0, 0, camera)
              .bind_buffer(0, 1, meshes)
              .bind_buffer(0, 2, transforms)
              .bind_buffer(0, 3, mesh_instances)
              .bind_buffer(0, 4, meshlet_instances)
              .bind_buffer(0, 5, visible_meshlet_instances_count)
              .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(mesh_instance_count, cull_flags))
              .dispatch_invocations(mesh_instance_count);

          return std::make_tuple(
              camera, meshes, transforms, mesh_instances, meshlet_instances, visible_meshlet_instances_count);
        });

    auto visible_meshlet_instances_count_buffer = vk_context.scratch_buffer<u32>({0});

    std::tie(
        camera_buffer,
        meshes_buffer,
        transforms_buffer,
        mesh_instances_buffer,
        meshlet_instances_buffer,
        visible_meshlet_instances_count_buffer) = vis_cull_meshes(std::move(camera_buffer),
                                                                  std::move(meshes_buffer),
                                                                  std::move(transforms_buffer),
                                                                  std::move(mesh_instances_buffer),
                                                                  std::move(meshlet_instances_buffer),
                                                                  std::move(visible_meshlet_instances_count_buffer));

    auto generate_cull_commands_pass = vuk::make_pass( //
        "generate cull commands",
        [](vuk::CommandBuffer& cmd_list,
           VUK_BA(vuk::eComputeRead) visible_meshlet_instances_count,
           VUK_BA(vuk::eComputeRW) cull_meshlets_cmd) {
          cmd_list.bind_compute_pipeline("generate_cull_commands")
              .bind_buffer(0, 0, visible_meshlet_instances_count)
              .bind_buffer(0, 1, cull_meshlets_cmd)
              .dispatch(1);

          return std::make_tuple(visible_meshlet_instances_count, cull_meshlets_cmd);
        });

    auto cull_meshlets_cmd_buffer = vk_context.scratch_buffer<vuk::DispatchIndirectCommand>({.x = 0, .y = 1, .z = 1});
    std::tie(visible_meshlet_instances_count_buffer, cull_meshlets_cmd_buffer) = generate_cull_commands_pass(
        std::move(visible_meshlet_instances_count_buffer), std::move(cull_meshlets_cmd_buffer));

    auto vis_cull_meshlets_pass = vuk::make_pass(
        "vis cull meshlets",
        [cull_flags](vuk::CommandBuffer& cmd_list,
                     VUK_BA(vuk::eIndirectRead) dispatch_cmd,
                     VUK_BA(vuk::eComputeRead) camera,
                     VUK_BA(vuk::eComputeRead) meshlet_instances,
                     VUK_BA(vuk::eComputeRead) mesh_instances,
                     VUK_BA(vuk::eComputeRead) meshes,
                     VUK_BA(vuk::eComputeRead) transforms,
                     VUK_IA(vuk::eComputeRead) hiz,
                     VUK_BA(vuk::eComputeRead) visible_meshlet_instances_count,
                     VUK_BA(vuk::eComputeRW) cull_triangles_cmd,
                     VUK_BA(vuk::eComputeWrite) visible_meshlet_instances_indices) {
          cmd_list //
              .bind_compute_pipeline("cull_meshlets")
              .bind_buffer(0, 0, camera)
              .bind_buffer(0, 1, meshlet_instances)
              .bind_buffer(0, 2, mesh_instances)
              .bind_buffer(0, 3, meshes)
              .bind_buffer(0, 4, transforms)
              .bind_image(0, 5, hiz)
              .bind_sampler(0, 6, hiz_sampler_info)
              .bind_buffer(0, 7, visible_meshlet_instances_count)
              .bind_buffer(0, 8, cull_triangles_cmd)
              .bind_buffer(0, 9, visible_meshlet_instances_indices)
              .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, cull_flags)
              .dispatch_indirect(dispatch_cmd);

          return std::make_tuple(camera,
                                 meshlet_instances,
                                 mesh_instances,
                                 meshes,
                                 transforms,
                                 hiz,
                                 cull_triangles_cmd,
                                 visible_meshlet_instances_indices);
        });

    auto cull_triangles_cmd_buffer = vk_context.scratch_buffer<vuk::DispatchIndirectCommand>({.x = 0, .y = 1, .z = 1});
    auto visible_meshlet_instances_indices_buffer = std::move(
        self.prepared_frame.visible_meshlet_instances_indices_buffer);

    std::tie(camera_buffer,
             meshlet_instances_buffer,
             mesh_instances_buffer,
             meshes_buffer,
             transforms_buffer,
             hiz_attachment,
             cull_triangles_cmd_buffer,
             visible_meshlet_instances_indices_buffer) =
        vis_cull_meshlets_pass(std::move(cull_meshlets_cmd_buffer),
                               std::move(camera_buffer),
                               std::move(meshlet_instances_buffer),
                               std::move(mesh_instances_buffer),
                               std::move(meshes_buffer),
                               std::move(transforms_buffer),
                               std::move(hiz_attachment),
                               std::move(visible_meshlet_instances_count_buffer),
                               std::move(cull_triangles_cmd_buffer),
                               std::move(visible_meshlet_instances_indices_buffer));

    auto vis_cull_triangles_pass = vuk::make_pass(
        "vis cull triangles",
        [cull_flags](vuk::CommandBuffer& cmd_list,
                     VUK_BA(vuk::eIndirectRead) cull_triangles_cmd,
                     VUK_BA(vuk::eComputeRead) camera,
                     VUK_BA(vuk::eComputeRead) visible_meshlet_instances_indices,
                     VUK_BA(vuk::eComputeRead) meshlet_instances,
                     VUK_BA(vuk::eComputeRead) mesh_instances,
                     VUK_BA(vuk::eComputeRead) meshes,
                     VUK_BA(vuk::eComputeRead) transforms,
                     VUK_BA(vuk::eComputeRW) draw_indexed_cmd,
                     VUK_BA(vuk::eComputeWrite) reordered_indices) {
          cmd_list //
              .bind_compute_pipeline("cull_triangles")
              .bind_buffer(0, 0, camera)
              .bind_buffer(0, 1, visible_meshlet_instances_indices)
              .bind_buffer(0, 2, meshlet_instances)
              .bind_buffer(0, 3, mesh_instances)
              .bind_buffer(0, 4, meshes)
              .bind_buffer(0, 5, transforms)
              .bind_buffer(0, 6, draw_indexed_cmd)
              .bind_buffer(0, 7, reordered_indices)
              .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, cull_flags)
              .dispatch_indirect(cull_triangles_cmd);

          return std::make_tuple(camera,
                                 visible_meshlet_instances_indices,
                                 meshlet_instances,
                                 mesh_instances,
                                 meshes,
                                 transforms,
                                 draw_indexed_cmd,
                                 reordered_indices);
        });

    auto draw_command_buffer = vk_context.scratch_buffer<vuk::DrawIndexedIndirectCommand>({.instanceCount = 1});

    std::tie(camera_buffer,
             visible_meshlet_instances_indices_buffer,
             meshlet_instances_buffer,
             mesh_instances_buffer,
             meshes_buffer,
             transforms_buffer,
             draw_command_buffer,
             reordered_indices_buffer) = vis_cull_triangles_pass( //
        std::move(cull_triangles_cmd_buffer),
        std::move(camera_buffer),
        std::move(visible_meshlet_instances_indices_buffer),
        std::move(meshlet_instances_buffer),
        std::move(mesh_instances_buffer),
        std::move(meshes_buffer),
        std::move(transforms_buffer),
        std::move(draw_command_buffer),
        std::move(reordered_indices_buffer));

    auto visbuffer_attachment = vuk::declare_ia(
        "visbuffer data",
        {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
         .format = vuk::Format::eR32Uint,
         .sample_count = vuk::Samples::e1});
    visbuffer_attachment.same_shape_as(final_attachment);

    auto overdraw_attachment = vuk::declare_ia(
        "overdraw",
        {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
         .format = vuk::Format::eR32Uint,
         .sample_count = vuk::Samples::e1});
    overdraw_attachment.same_shape_as(final_attachment);

    std::tie(visbuffer_attachment, overdraw_attachment) = vuk::make_pass(
        "vis clear",
        []( //
            vuk::CommandBuffer& cmd_list,
            VUK_IA(vuk::eComputeWrite) visbuffer,
            VUK_IA(vuk::eComputeWrite) overdraw) {
          cmd_list //
              .bind_compute_pipeline("visbuffer_clear")
              .bind_image(0, 0, visbuffer)
              .bind_image(0, 1, overdraw)
              .push_constants(vuk::ShaderStageFlagBits::eCompute,
                              0,
                              PushConstants(glm::uvec2(visbuffer->extent.width, visbuffer->extent.height)))
              .dispatch_invocations_per_pixel(visbuffer);

          return std::make_tuple(visbuffer, overdraw);
        })(std::move(visbuffer_attachment), std::move(overdraw_attachment));

    auto vis_encode_pass = vuk::make_pass(
        "vis encode",
        [&descriptor_set = bindless_set]( //
            vuk::CommandBuffer& cmd_list,
            VUK_BA(vuk::eIndirectRead) triangle_indirect,
            VUK_BA(vuk::eIndexRead) index_buffer,
            VUK_BA(vuk::eVertexRead) camera,
            VUK_BA(vuk::eVertexRead) meshlet_instances,
            VUK_BA(vuk::eVertexRead) mesh_instances,
            VUK_BA(vuk::eVertexRead) meshes,
            VUK_BA(vuk::eVertexRead) transforms,
            VUK_BA(vuk::eFragmentRead) materials,
            VUK_IA(vuk::eColorRW) visbuffer,
            VUK_IA(vuk::eDepthStencilRW) depth,
            VUK_IA(vuk::eFragmentRW) overdraw) {
          cmd_list //
              .bind_graphics_pipeline("visbuffer_encode")
              .set_rasterization({.cullMode = vuk::CullModeFlagBits::eBack})
              .set_depth_stencil({.depthTestEnable = true,
                                  .depthWriteEnable = true,
                                  .depthCompareOp = vuk::CompareOp::eGreaterOrEqual})
              .set_color_blend(visbuffer, vuk::BlendPreset::eOff)
              .set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
              .set_viewport(0, vuk::Rect2D::framebuffer())
              .set_scissor(0, vuk::Rect2D::framebuffer())
              .bind_persistent(1, descriptor_set)
              .bind_buffer(0, 0, camera)
              .bind_buffer(0, 1, meshlet_instances)
              .bind_buffer(0, 2, mesh_instances)
              .bind_buffer(0, 3, meshes)
              .bind_buffer(0, 4, transforms)
              .bind_buffer(0, 5, materials)
              .bind_image(0, 6, overdraw)
              .bind_index_buffer(index_buffer, vuk::IndexType::eUint32)
              .draw_indexed_indirect(1, triangle_indirect);

          return std::make_tuple(
              camera, meshlet_instances, mesh_instances, meshes, transforms, materials, visbuffer, depth, overdraw);
        });

    std::tie(camera_buffer,
             meshlet_instances_buffer,
             mesh_instances_buffer,
             meshes_buffer,
             transforms_buffer,
             materials_buffer,
             visbuffer_attachment,
             depth_attachment,
             overdraw_attachment) = vis_encode_pass( //
        std::move(draw_command_buffer),
        std::move(reordered_indices_buffer),
        std::move(camera_buffer),
        std::move(meshlet_instances_buffer),
        std::move(mesh_instances_buffer),
        std::move(meshes_buffer),
        std::move(transforms_buffer),
        std::move(materials_buffer),
        std::move(visbuffer_attachment),
        std::move(depth_attachment),
        std::move(overdraw_attachment));

    std::tie(depth_attachment, hiz_attachment) = vuk::make_pass(
        "hiz generate",
        [](vuk::CommandBuffer& cmd_list, VUK_IA(vuk::eComputeSampled) src, VUK_IA(vuk::eComputeRW) dst) {
          auto extent = dst->extent;
          auto mip_count = dst->level_count;
          OX_CHECK_LT(mip_count, 13u);

          auto dispatch_x = (extent.width + 63) >> 6;
          auto dispatch_y = (extent.height + 63) >> 6;

          auto sampler_info = vuk::SamplerCreateInfo{
              .pNext = &sampler_min_clamp_reduction_mode,
              .minFilter = vuk::Filter::eLinear,
              .mipmapMode = vuk::SamplerMipmapMode::eNearest,
              .addressModeU = vuk::SamplerAddressMode::eClampToEdge,
              .addressModeV = vuk::SamplerAddressMode::eClampToEdge,
          };

          // clang-format off
          cmd_list
              .bind_compute_pipeline("hiz_pipeline")
              .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(mip_count, dispatch_x * dispatch_y, glm::mat2(1.0f)))
              .specialize_constants(0, extent.width == extent.height && (extent.width & (extent.width - 1)) == 0 ? 1u : 0u)
              .specialize_constants(1, extent.width)
              .specialize_constants(2, extent.height);
          // clang-format on

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
    albedo_attachment.same_shape_as(visbuffer_attachment);
    albedo_attachment = vuk::clear_image(std::move(albedo_attachment), vuk::Black<f32>);

    auto normal_attachment = vuk::declare_ia(
        "normal",
        {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
         .format = vuk::Format::eR16G16B16A16Sfloat,
         .sample_count = vuk::Samples::e1});
    normal_attachment.same_shape_as(visbuffer_attachment);
    normal_attachment = vuk::clear_image(std::move(normal_attachment), vuk::Black<f32>);

    auto emissive_attachment = vuk::declare_ia(
        "emissive",
        {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
         .format = vuk::Format::eB10G11R11UfloatPack32,
         .sample_count = vuk::Samples::e1});
    emissive_attachment.same_shape_as(visbuffer_attachment);
    emissive_attachment = vuk::clear_image(std::move(emissive_attachment), vuk::Black<f32>);

    auto metallic_roughness_occlusion_attachment = vuk::declare_ia(
        "metallic roughness occlusion",
        {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
         .format = vuk::Format::eR8G8B8A8Unorm,
         .sample_count = vuk::Samples::e1});
    metallic_roughness_occlusion_attachment.same_shape_as(visbuffer_attachment);
    metallic_roughness_occlusion_attachment = vuk::clear_image(std::move(metallic_roughness_occlusion_attachment),
                                                               vuk::Black<f32>);

    auto vis_decode_pass = vuk::make_pass( //
        "vis decode",
        [&descriptor_set = bindless_set](  //
            vuk::CommandBuffer& cmd_list,
            VUK_BA(vuk::eFragmentRead) camera,
            VUK_BA(vuk::eFragmentRead) meshlet_instances,
            VUK_BA(vuk::eFragmentRead) mesh_instances,
            VUK_BA(vuk::eFragmentRead) meshes,
            VUK_BA(vuk::eFragmentRead) transforms,
            VUK_BA(vuk::eFragmentRead) materials,
            VUK_IA(vuk::eFragmentRead) visbuffer,
            VUK_IA(vuk::eColorRW) albedo,
            VUK_IA(vuk::eColorRW) normal,
            VUK_IA(vuk::eColorRW) emissive,
            VUK_IA(vuk::eColorRW) metallic_roughness_occlusion) {
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
              .bind_persistent(1, descriptor_set)
              .bind_buffer(0, 0, camera)
              .bind_buffer(0, 1, meshlet_instances)
              .bind_buffer(0, 2, mesh_instances)
              .bind_buffer(0, 3, meshes)
              .bind_buffer(0, 4, transforms)
              .bind_buffer(0, 5, materials)
              .bind_image(0, 6, visbuffer)
              .draw(3, 1, 0, 1);

          return std::make_tuple(camera,
                                 meshlet_instances,
                                 mesh_instances,
                                 meshes,
                                 transforms,
                                 visbuffer,
                                 albedo,
                                 normal,
                                 emissive,
                                 metallic_roughness_occlusion);
        });

    std::tie(camera_buffer,
             meshlet_instances_buffer,
             mesh_instances_buffer,
             meshes_buffer,
             transforms_buffer,
             visbuffer_attachment,
             albedo_attachment,
             normal_attachment,
             emissive_attachment,
             metallic_roughness_occlusion_attachment) = vis_decode_pass( //
        std::move(camera_buffer),
        std::move(meshlet_instances_buffer),
        std::move(mesh_instances_buffer),
        std::move(meshes_buffer),
        std::move(transforms_buffer),
        std::move(materials_buffer),
        std::move(visbuffer_attachment),
        std::move(albedo_attachment),
        std::move(normal_attachment),
        std::move(emissive_attachment),
        std::move(metallic_roughness_occlusion_attachment));

    if (!debugging && self.atmosphere.has_value() && self.sun.has_value()) {
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
      const auto debug_attachment_ia = vuk::ImageAttachment{
          .usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
          .extent = render_info.extent,
          .format = vuk::Format::eR16G16B16A16Sfloat,
          .sample_count = vuk::Samples::e1,
          .level_count = 1,
          .layer_count = 1,
      };
      auto debug_attachment = vuk::clear_image(vuk::declare_ia("debug_attachment", debug_attachment_ia),
                                               vuk::Black<float>);

      std::tie(debug_attachment, depth_attachment, camera_buffer) = vuk::make_pass(
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
              VUK_IA(vuk::eFragmentSampled) hiz,
              VUK_BA(vuk::eFragmentRead) camera,
              VUK_BA(vuk::eFragmentRead) visible_meshlet_instances_indices,
              VUK_BA(vuk::eFragmentRead) meshlet_instances,
              VUK_BA(vuk::eFragmentRead) meshes,
              VUK_BA(vuk::eFragmentRead) transforms_) {
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
                .bind_sampler(0, 1, hiz_sampler_info)
                .bind_image(0, 2, visbuffer)
                .bind_image(0, 3, depth)
                .bind_image(0, 4, overdraw)
                .bind_image(0, 5, albedo)
                .bind_image(0, 6, normal)
                .bind_image(0, 7, emissive)
                .bind_image(0, 8, metallic_roughness_occlusion)
                .bind_image(0, 9, hiz)
                .bind_buffer(0, 10, camera)
                .bind_buffer(0, 11, visible_meshlet_instances_indices)
                .bind_buffer(0, 12, meshlet_instances)
                .bind_buffer(0, 13, meshes)
                .bind_buffer(0, 14, transforms_)
                .push_constants(vuk::ShaderStageFlagBits::eFragment,
                                0,
                                PushConstants(std::to_underlying(debug_view), debug_heatmap_scale))
                .draw(3, 1, 0, 0);

            return std::make_tuple(dst, depth, camera);
          })(std::move(debug_attachment),
             std::move(visbuffer_attachment),
             std::move(depth_attachment),
             std::move(overdraw_attachment),
             std::move(albedo_attachment),
             std::move(normal_attachment),
             std::move(emissive_attachment),
             std::move(metallic_roughness_occlusion_attachment),
             std::move(hiz_attachment),
             std::move(camera_buffer),
             std::move(visible_meshlet_instances_indices_buffer),
             std::move(meshlet_instances_buffer),
             std::move(meshes_buffer),
             std::move(transforms_buffer));

      return debug_attachment; // Early return debug attachment
    }
  }

  // --- 2D Pass ---
  if (!self.render_queue_2d.sprite_data.empty()) {
    std::tie(final_attachment, //
             depth_attachment,
             camera_buffer,
             vertex_buffer_2d,
             material_buffer,
             transforms_buffer) =
        vuk::make_pass(
            "2d_forward_pass",
            [&rq2d = self.render_queue_2d]( //
                vuk::CommandBuffer& command_buffer,
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
               std::move(transforms_buffer));
  }

  // --- Atmosphere Pass ---
  if (self.atmosphere.has_value() && self.sun.has_value() && !debugging) {
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
        [](vuk::CommandBuffer& cmd_list,
           VUK_BA(vuk::eComputeRead) atmosphere_,
           VUK_BA(vuk::eComputeRead) sun_,
           VUK_BA(vuk::eComputeRead) camera,
           VUK_IA(vuk::eComputeSampled) sky_transmittance_lut,
           VUK_IA(vuk::eComputeSampled) sky_multiscatter_lut,
           VUK_IA(vuk::eComputeRW) sky_view_lut) {
          auto linear_clamp_sampler = vuk::SamplerCreateInfo{
              .magFilter = vuk::Filter::eLinear,
              .minFilter = vuk::Filter::eLinear,
              .addressModeU = vuk::SamplerAddressMode::eClampToEdge,
              .addressModeV = vuk::SamplerAddressMode::eClampToEdge,
              .addressModeW = vuk::SamplerAddressMode::eClampToEdge,
          };

          cmd_list.bind_compute_pipeline("sky_view_pipeline")
              .bind_sampler(0, 0, linear_clamp_sampler)
              .bind_image(0, 1, sky_transmittance_lut)
              .bind_image(0, 2, sky_multiscatter_lut)
              .bind_image(0, 3, sky_view_lut)
              .bind_buffer(0, 4, atmosphere_)
              .bind_buffer(0, 5, sun_)
              .bind_buffer(0, 6, camera)
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
        [](vuk::CommandBuffer& cmd_list,
           VUK_BA(vuk::eComputeRead) atmosphere_,
           VUK_BA(vuk::eComputeRead) sun_,
           VUK_BA(vuk::eComputeRead) camera,
           VUK_IA(vuk::eComputeSampled) sky_transmittance_lut,
           VUK_IA(vuk::eComputeSampled) sky_multiscatter_lut,
           VUK_IA(vuk::eComputeRW) sky_aerial_perspective_lut) {
          auto linear_clamp_sampler = vuk::SamplerCreateInfo{
              .magFilter = vuk::Filter::eLinear,
              .minFilter = vuk::Filter::eLinear,
              .addressModeU = vuk::SamplerAddressMode::eClampToEdge,
              .addressModeV = vuk::SamplerAddressMode::eClampToEdge,
              .addressModeW = vuk::SamplerAddressMode::eClampToEdge,
          };

          cmd_list.bind_compute_pipeline("sky_aerial_perspective_pipeline")
              .bind_sampler(0, 0, linear_clamp_sampler)
              .bind_image(0, 1, sky_transmittance_lut)
              .bind_image(0, 2, sky_multiscatter_lut)
              .bind_image(0, 3, sky_aerial_perspective_lut)
              .bind_buffer(0, 4, atmosphere_)
              .bind_buffer(0, 5, sun_)
              .bind_buffer(0, 6, camera)
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
        [](vuk::CommandBuffer& cmd_list,
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

          auto linear_clamp_sampler = vuk::SamplerCreateInfo{
              .magFilter = vuk::Filter::eLinear,
              .minFilter = vuk::Filter::eLinear,
              .addressModeU = vuk::SamplerAddressMode::eClampToEdge,
              .addressModeV = vuk::SamplerAddressMode::eClampToEdge,
              .addressModeW = vuk::SamplerAddressMode::eClampToEdge,
          };

          cmd_list.bind_graphics_pipeline("sky_final_pipeline")
              .set_rasterization({})
              .set_depth_stencil({})
              .set_color_blend(dst, blend_info)
              .set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
              .set_viewport(0, vuk::Rect2D::framebuffer())
              .set_scissor(0, vuk::Rect2D::framebuffer())
              .bind_sampler(0, 0, linear_clamp_sampler)
              .bind_image(0, 1, sky_transmittance_lut)
              .bind_image(0, 2, sky_aerial_perspective_lut)
              .bind_image(0, 3, sky_view_lut)
              .bind_image(0, 4, depth)
              .bind_buffer(0, 5, atmosphere_)
              .bind_buffer(0, 6, sun_)
              .bind_buffer(0, 7, camera)
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
    auto histogram_inf = self.histogram_info.value_or(GPU::HistogramInfo{});

    auto histogram_buffer = self.renderer.vk_context->alloc_transient_buffer(vuk::MemoryUsage::eGPUonly,
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

    auto exposure_buffer_value = vuk::acquire_buf("exposure buffer", *self.renderer.exposure_buffer, vuk::eNone);

    if (self.histogram_info.has_value()) {
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

  auto debug_renderer_enabled = (bool)RendererCVar::cvar_enable_debug_renderer.get();

  if (debug_renderer_enabled &&
      (self.prepared_frame.line_index_count > 0 || self.prepared_frame.triangle_index_count > 0)) {
    auto debug_renderer_verticies_buffer = std::move(self.prepared_frame.debug_renderer_verticies_buffer);
    auto debug_renderer_pass = vuk::make_pass(
        "debug_renderer_pass",
        [line_index_count = self.prepared_frame.line_index_count](vuk::CommandBuffer& cmd_list,
                                                                  VUK_IA(vuk::eColorWrite) dst,
                                                                  VUK_BA(vuk::eFragmentRead) dbg_vtx,
                                                                  VUK_BA(vuk::eFragmentRead) camera) {
          auto& dbg_index_buffer = *DebugRenderer::get_instance()->get_global_index_buffer();

          cmd_list.bind_graphics_pipeline("debug_renderer_pipeline")
              .set_depth_stencil(vuk::PipelineDepthStencilStateCreateInfo{
                  .depthTestEnable = false,
                  .depthWriteEnable = false,
                  .depthCompareOp = vuk::CompareOp::eGreaterOrEqual,
              })
              .set_dynamic_state(vuk::DynamicStateFlagBits::eScissor | vuk::DynamicStateFlagBits::eViewport)
              .broadcast_color_blend({})
              .set_rasterization({.polygonMode = vuk::PolygonMode::eLine, .cullMode = vuk::CullModeFlagBits::eNone, .lineWidth = 3.f})
              .set_primitive_topology(vuk::PrimitiveTopology::eLineList)
              .set_viewport(0, vuk::Rect2D::framebuffer())
              .set_scissor(0, vuk::Rect2D::framebuffer())
              .bind_vertex_buffer(0, dbg_vtx, 0, DebugRenderer::vertex_pack)
              .bind_index_buffer(dbg_index_buffer, vuk::IndexType::eUint32)
              .bind_buffer(0, 0, camera)
              .draw_indexed(line_index_count, 1, 0, 0, 0);

          return std::make_tuple(dst, camera);
        });

    std::tie(result_attachment,
             camera_buffer) = debug_renderer_pass(result_attachment, debug_renderer_verticies_buffer, camera_buffer);
  }

  return debugging ? final_attachment : result_attachment;
}

auto RendererInstance::update(this RendererInstance& self, RendererInstanceUpdateInfo& info) -> void {
  ZoneScoped;

  auto* asset_man = App::get_asset_manager();
  auto& vk_context = App::get_vkcontext();

  CameraComponent current_camera = {};
  CameraComponent frozen_camera = {};
  const auto freeze_culling = static_cast<bool>(RendererCVar::cvar_freeze_culling_frustum.get());

  self.scene->world
      .query_builder<const TransformComponent, const CameraComponent>() //
      .build()
      .each([&](flecs::entity e, const TransformComponent& tc, const CameraComponent& c) {
        if (freeze_culling && !self.saved_camera) {
          self.saved_camera = true;
          frozen_camera = current_camera;
        } else if (!freeze_culling && self.saved_camera) {
          self.saved_camera = false;
        }

        if (static_cast<bool>(RendererCVar::cvar_freeze_culling_frustum.get()) &&
            static_cast<bool>(RendererCVar::cvar_draw_camera_frustum.get())) {
          const auto proj = frozen_camera.get_projection_matrix() * frozen_camera.get_view_matrix();
          DebugRenderer::draw_frustum(proj, glm::vec4(0, 1, 0, 1), frozen_camera.near_clip, frozen_camera.far_clip);
        }

        current_camera = c;
      });

  CameraComponent cam = freeze_culling ? frozen_camera : current_camera;

  self.camera_data = GPU::CameraData{
      .position = glm::vec4(cam.position, 0.0f),
      .projection = cam.get_projection_matrix(),
      .inv_projection = cam.get_inv_projection_matrix(),
      .view = cam.get_view_matrix(),
      .inv_view = cam.get_inv_view_matrix(),
      .projection_view = cam.get_projection_matrix() * cam.get_view_matrix(),
      .inv_projection_view = cam.get_inverse_projection_view(),
      .previous_projection = self.previous_camera_data.projection,
      .previous_inv_projection = self.previous_camera_data.inv_projection,
      .previous_view = self.previous_camera_data.view,
      .previous_inv_view = self.previous_camera_data.inv_view,
      .previous_projection_view = self.previous_camera_data.projection_view,
      .previous_inv_projection_view = self.previous_camera_data.inv_projection_view,
      .temporalaa_jitter = cam.jitter,
      .temporalaa_jitter_prev = self.previous_camera_data.temporalaa_jitter_prev,
      .near_clip = cam.near_clip,
      .far_clip = cam.far_clip,
      .fov = cam.fov,
      .output_index = 0,
  };

  self.previous_camera_data = self.camera_data;

  math::calc_frustum_planes(self.camera_data.projection_view, self.camera_data.frustum_planes);

  option<GPU::Atmosphere> atmosphere_data = nullopt;
  option<GPU::Sun> sun_data = nullopt;

  self.scene->world
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

            if (const auto* atmos_info = e.try_get<AtmosphereComponent>()) {
              auto& atmos = atmosphere_data.emplace();
              atmos.rayleigh_scatter = atmos_info->rayleigh_scattering * 1e-3f;
              atmos.rayleigh_density = atmos_info->rayleigh_density;
              atmos.mie_scatter = atmos_info->mie_scattering * 1e-3f;
              atmos.mie_density = atmos_info->mie_density;
              atmos.mie_extinction = atmos_info->mie_extinction * 1e-3f;
              atmos.mie_asymmetry = atmos_info->mie_asymmetry;
              atmos.ozone_absorption = atmos_info->ozone_absorption * 1e-3f;
              atmos.ozone_height = atmos_info->ozone_height;
              atmos.ozone_thickness = atmos_info->ozone_thickness;
              atmos.aerial_perspective_start_km = atmos_info->aerial_perspective_start_km;

              f32 eye_altitude = cam.position.y * GPU::CAMERA_SCALE_UNIT;
              eye_altitude += atmos.planet_radius + GPU::PLANET_RADIUS_OFFSET;
              atmos.eye_position = glm::vec3(0.0f, eye_altitude, 0.0f);
            }
          });

  self.atmosphere = atmosphere_data;
  self.sun = sun_data;

  self.render_queue_2d.init();

  self.scene->world
      .query_builder<const TransformComponent, const SpriteComponent>() //
      .build()
      .each([asset_man, &scene = self.scene, &cam, &rq2d = self.render_queue_2d](
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

  self.scene->world
      .query_builder<const AutoExposureComponent>() //
      .build()
      .each([&hist_info](flecs::entity e, const AutoExposureComponent& c) {
        auto& i = hist_info.emplace();
        i.max_exposure = c.max_exposure;
        i.min_exposure = c.min_exposure;
        i.adaptation_speed = c.adaptation_speed;
        i.ev100_bias = c.ev100_bias;
      });

  self.histogram_info = hist_info;

  if (!info.dirty_transform_ids.empty()) {
    auto rebuild_transforms = !self.transforms_buffer ||
                              self.transforms_buffer->size <= info.gpu_transforms.size_bytes();
    self.transforms_buffer = vk_context.resize_buffer(
        std::move(self.transforms_buffer), vuk::MemoryUsage::eGPUonly, info.gpu_transforms.size_bytes());

    if (rebuild_transforms) {
      // If we resize buffer, we need to refill it again, so individual uploads are not required.
      self.prepared_frame.transforms_buffer = vk_context.upload_staging(info.gpu_transforms, *self.transforms_buffer);
    } else {
      // Buffer is not resized, upload individual transforms.

      auto dirty_transforms_count = info.dirty_transform_ids.size();
      auto dirty_transforms_size_bytes = dirty_transforms_count * sizeof(GPU::Transforms);
      auto upload_buffer = vk_context.alloc_transient_buffer(vuk::MemoryUsage::eCPUtoGPU, dirty_transforms_size_bytes);
      auto* dst_transform_ptr = reinterpret_cast<GPU::Transforms*>(upload_buffer->mapped_ptr);
      auto upload_offsets = std::vector<u64>(dirty_transforms_count);

      for (const auto& [dirty_transform_id, offset] : std::views::zip(info.dirty_transform_ids, upload_offsets)) {
        auto index = SlotMap_decode_id(dirty_transform_id).index;
        const auto& transform = info.gpu_transforms[index];
        std::memcpy(dst_transform_ptr, &transform, sizeof(GPU::Transforms));
        offset = index * sizeof(GPU::Transforms);
        dst_transform_ptr++;
      }

      auto update_transforms_pass = vuk::make_pass(
          "update scene transforms",
          [=](vuk::CommandBuffer& cmd_list, //
              VUK_BA(vuk::Access::eTransferRead) src_buffer,
              VUK_BA(vuk::Access::eTransferWrite) dst_buffer) {
            for (usize i = 0; i < upload_offsets.size(); i++) {
              auto offset = upload_offsets[i];
              auto src_subrange = src_buffer->subrange(i * sizeof(GPU::Transforms), sizeof(GPU::Transforms));
              auto dst_subrange = dst_buffer->subrange(offset, sizeof(GPU::Transforms));
              cmd_list.copy_buffer(src_subrange, dst_subrange);
            }

            return dst_buffer;
          });

      self.prepared_frame.transforms_buffer = vuk::acquire_buf(
          "transforms", *self.transforms_buffer, vuk::Access::eMemoryRead);
      self.prepared_frame.transforms_buffer = update_transforms_pass(std::move(upload_buffer),
                                                                     std::move(self.prepared_frame.transforms_buffer));
    }
  } else if (self.transforms_buffer) {
    self.prepared_frame.transforms_buffer = vuk::acquire_buf(
        "transforms", *self.transforms_buffer, vuk::Access::eMemoryRead);
  }

  if (!info.dirty_material_indices.empty()) {
    auto rebuild_materials = !self.materials_buffer || self.materials_buffer->size <= info.gpu_materials.size_bytes();
    self.materials_buffer = vk_context.resize_buffer(
        std::move(self.materials_buffer), vuk::MemoryUsage::eGPUonly, info.gpu_materials.size_bytes());

    if (rebuild_materials) {
      self.prepared_frame.materials_buffer = vk_context.upload_staging(info.gpu_materials, *self.materials_buffer);
    } else {
      // TODO: Literally repeating code, find a solution to this
      auto dirty_materials_count = info.dirty_material_indices.size();
      auto dirty_materials_size_bytes = dirty_materials_count * sizeof(GPU::Material);
      auto upload_buffer = vk_context.alloc_transient_buffer(vuk::MemoryUsage::eCPUtoGPU, dirty_materials_size_bytes);
      auto* dst_materials_ptr = reinterpret_cast<GPU::Material*>(upload_buffer->mapped_ptr);
      auto upload_offsets = std::vector<u32>(dirty_materials_count);

      for (const auto& [dirty_material, index, offset] :
           std::views::zip(info.gpu_materials, info.dirty_material_indices, upload_offsets)) {
        std::memcpy(dst_materials_ptr, &dirty_material, sizeof(GPU::Material));
        offset = index * sizeof(GPU::Material);
        dst_materials_ptr++;
      }

      auto update_materials_pass = vuk::make_pass(
          "update scene materials",
          [=](vuk::CommandBuffer& cmd_list, //
              VUK_BA(vuk::Access::eTransferRead) src_buffer,
              VUK_BA(vuk::Access::eTransferWrite) dst_buffer) {
            for (usize i = 0; i < upload_offsets.size(); i++) {
              auto offset = upload_offsets[i];
              auto src_subrange = src_buffer->subrange(i * sizeof(GPU::Material), sizeof(GPU::Material));
              auto dst_subrange = dst_buffer->subrange(offset, sizeof(GPU::Material));
              cmd_list.copy_buffer(src_subrange, dst_subrange);
            }

            return dst_buffer;
          });

      self.prepared_frame.materials_buffer = vuk::acquire_buf(
          "materials", *self.materials_buffer, vuk::Access::eMemoryRead);
      self.prepared_frame.materials_buffer = update_materials_pass(std::move(upload_buffer),
                                                                   std::move(self.prepared_frame.materials_buffer));
    }
  } else if (self.materials_buffer) {
    self.prepared_frame.materials_buffer = vuk::acquire_buf(
        "materials", *self.materials_buffer, vuk::Access::eMemoryRead);
  }

  if (!info.gpu_meshes.empty()) {
    self.meshes_buffer = vk_context.resize_buffer(
        std::move(self.meshes_buffer), vuk::MemoryUsage::eGPUonly, info.gpu_meshes.size_bytes());
    self.prepared_frame.meshes_buffer = vk_context.upload_staging(info.gpu_meshes, *self.meshes_buffer);
  } else if (self.meshes_buffer) {
    self.prepared_frame.meshes_buffer = vuk::acquire_buf("meshes", *self.meshes_buffer, vuk::Access::eMemoryRead);
  }

  if (!info.gpu_mesh_instances.empty()) {
    self.mesh_instances_buffer = vk_context.resize_buffer(
        std::move(self.mesh_instances_buffer), vuk::MemoryUsage::eGPUonly, info.gpu_mesh_instances.size_bytes());
    self.prepared_frame.mesh_instances_buffer = vk_context.upload_staging(info.gpu_mesh_instances,
                                                                          *self.mesh_instances_buffer);
  } else if (self.mesh_instances_buffer) {
    self.prepared_frame.mesh_instances_buffer = vuk::acquire_buf(
        "mesh instances", *self.mesh_instances_buffer, vuk::Access::eMemoryRead);
  }

  if (info.max_meshlet_instance_count > 0) {
    self.prepared_frame.meshlet_instances_buffer = vk_context.alloc_transient_buffer(
        vuk::MemoryUsage::eGPUonly, info.max_meshlet_instance_count * sizeof(GPU::MeshletInstance));
    self.prepared_frame.visible_meshlet_instances_indices_buffer = vk_context.alloc_transient_buffer(
        vuk::MemoryUsage::eGPUonly, info.max_meshlet_instance_count * sizeof(u32));
    self.prepared_frame.reordered_indices_buffer = vk_context.alloc_transient_buffer(
        vuk::MemoryUsage::eGPUonly, info.max_meshlet_instance_count * Mesh::MAX_MESHLET_PRIMITIVES * 3 * sizeof(u32));
  }

  self.prepared_frame.mesh_instance_count = info.mesh_instance_count;

  auto debug_renderer_enabled = (bool)RendererCVar::cvar_enable_debug_renderer.get();

  if (debug_renderer_enabled) {
    const auto& lines = DebugRenderer::get_instance()->get_lines(false);
    auto [line_vertices, line_index_count] = DebugRenderer::get_vertices_from_lines(lines);

    const auto& triangles = DebugRenderer::get_instance()->get_triangles(false);
    auto [triangle_vertices, triangle_index_count] = DebugRenderer::get_vertices_from_triangles(triangles);

    const u32 index_count = line_index_count + triangle_index_count;
    OX_CHECK_LT(index_count, DebugRenderer::MAX_LINE_INDICES, "Increase DebugRenderer::MAX_LINE_INDICES");

    self.prepared_frame.line_index_count = line_index_count;
    self.prepared_frame.triangle_index_count = triangle_index_count;

    std::vector<DebugRenderer::Vertex> vertices = line_vertices;
    vertices.insert(vertices.end(), triangle_vertices.begin(), triangle_vertices.end());
    std::span<DebugRenderer::Vertex> vertices_span = line_vertices;

    if (!vertices.empty()) {
      self.debug_renderer_verticies_buffer = vk_context.resize_buffer(
          std::move(self.debug_renderer_verticies_buffer), vuk::MemoryUsage::eGPUonly, vertices_span.size_bytes());
      self.prepared_frame.debug_renderer_verticies_buffer = vk_context.upload_staging(
          vertices_span, *self.debug_renderer_verticies_buffer);
    } else if (self.debug_renderer_verticies_buffer) {
      self.prepared_frame.debug_renderer_verticies_buffer = vuk::acquire_buf(
          "debug_renderer_verticies_buffer", *self.debug_renderer_verticies_buffer, vuk::Access::eMemoryRead);
    }

    DebugRenderer::reset();
  }
}
} // namespace ox
