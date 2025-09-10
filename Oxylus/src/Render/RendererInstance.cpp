#include "Render/RendererInstance.hpp"

#include "Asset/AssetManager.hpp"
#include "Asset/Model.hpp"
#include "Asset/Texture.hpp"
#include "Core/App.hpp"
#include "Memory/Stack.hpp"
#include "Render/DebugRenderer.hpp"
#include "Render/RendererConfig.hpp"
#include "Render/Utils/VukCommon.hpp"
#include "Render/Vulkan/VkContext.hpp"
#include "Scene/SceneGPU.hpp"

namespace ox {
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

template <>
struct RendererInstance::BufferTraits<GPU::Transforms> {
  using offset_type = u64;
  static constexpr std::string_view buffer_name = "transforms";
  static constexpr std::string_view pass_name = "update scene transforms";

  static auto get_buffer_ref(auto& self) -> auto& { return self.transforms_buffer; }
  static auto& get_prepared_buffer_ref(auto& self) { return self.prepared_frame.transforms_buffer; }

  static auto get_index(const auto& dirty_id) -> usize { return SlotMap_decode_id(dirty_id).index; }

  static auto get_element(const auto& gpu_data, usize index) -> const auto& { return gpu_data[index]; }
};

template <>
struct RendererInstance::BufferTraits<GPU::Material> {
  using offset_type = u32;
  static constexpr std::string_view buffer_name = "materials";
  static constexpr std::string_view pass_name = "update scene materials";

  static auto get_buffer_ref(auto& self) -> auto& { return self.materials_buffer; }
  static auto& get_prepared_buffer_ref(auto& self) { return self.prepared_frame.materials_buffer; }

  static auto get_index(const auto& dirty_id) -> usize { return static_cast<usize>(dirty_id); }

  static auto get_element(const auto& gpu_data, usize index) -> const auto& { return gpu_data[index]; }
};

template <>
struct RendererInstance::BufferTraits<GPU::PointLight> {
  using offset_type = u32;
  static constexpr std::string_view buffer_name = "point_lights";
  static constexpr std::string_view pass_name = "update point lights";

  static auto get_buffer_ref(auto& self) -> auto& { return self.point_lights_buffer; }
  static auto& get_prepared_buffer_ref(auto& self) { return self.prepared_frame.materials_buffer; }

  static auto get_index(const auto& dirty_id) -> usize { return static_cast<usize>(dirty_id); }

  static auto get_element(const auto& gpu_data, usize index) -> const auto& { return gpu_data[index]; }
};

template <typename T>
auto update_gpu_buffer(auto& self, auto& vk_context, const auto& gpu_data, const auto& dirty_ids) -> void {
  using traits = RendererInstance::BufferTraits<T>;

  const auto data_size_bytes = gpu_data.size_bytes();
  constexpr auto element_size = sizeof(T);

  auto& buffer_ref = traits::get_buffer_ref(self);

  const auto rebuild_needed = !buffer_ref || buffer_ref->size <= data_size_bytes;
  buffer_ref = vk_context.resize_buffer(std::move(buffer_ref), vuk::MemoryUsage::eGPUonly, data_size_bytes);

  if (rebuild_needed) {
    traits::get_prepared_buffer_ref(self) = vk_context.upload_staging(gpu_data, *buffer_ref);
  } else {
    const auto dirty_count = dirty_ids.size();
    const auto dirty_size_bytes = dirty_count * element_size;

    auto upload_buffer = vk_context.alloc_transient_buffer(vuk::MemoryUsage::eCPUtoGPU, dirty_size_bytes);
    auto* dst_ptr = reinterpret_cast<T*>(upload_buffer->mapped_ptr);

    std::vector<typename traits::offset_type> upload_offsets;
    upload_offsets.reserve(dirty_count);

    for (const auto& [i, dirty_id] : std::views::zip(std::views::iota(0_sz), dirty_ids)) {
      const auto index = traits::get_index(dirty_id);
      const auto& element = traits::get_element(gpu_data, index);
      std::memcpy(dst_ptr + i, &element, element_size);
      upload_offsets.push_back(static_cast<typename traits::offset_type>(index * element_size));
    }

    auto update_pass = vuk::make_pass(
      traits::pass_name,
      [upload_offsets = std::move(upload_offsets)](
        vuk::CommandBuffer& cmd_list,
        VUK_BA(vuk::Access::eTransferRead) src_buffer,
        VUK_BA(vuk::Access::eTransferWrite) dst_buffer
      ) {
        for (const auto& [i, offset] : std::views::zip(std::views::iota(0_sz), upload_offsets)) {
          const auto src_subrange = src_buffer->subrange(i * element_size, element_size);
          const auto dst_subrange = dst_buffer->subrange(offset, element_size);
          cmd_list.copy_buffer(src_subrange, dst_subrange);
        }
        return dst_buffer;
      }
    );

    auto buffer_handle = vuk::acquire_buf(traits::buffer_name, *buffer_ref, vuk::Access::eMemoryRead);
    traits::get_prepared_buffer_ref(self) = update_pass(std::move(upload_buffer), std::move(buffer_handle));
  }
}

template <typename T>
auto update_buffer_if_dirty(auto& self, auto& vk_context, const auto& gpu_data, const auto& dirty_ids) -> void {
  using traits = RendererInstance::BufferTraits<T>;

  if (!dirty_ids.empty()) {
    update_gpu_buffer<T>(self, vk_context, gpu_data, dirty_ids);
  } else {
    auto& buffer_ref = traits::get_buffer_ref(self);
    if (buffer_ref) {
      traits::get_prepared_buffer_ref(self) = vuk::acquire_buf(
        traits::buffer_name, *buffer_ref, vuk::Access::eMemoryRead
      );
    }
  }
}

RendererInstance::RendererInstance(Scene* owner_scene, Renderer& parent_renderer)
    : scene(owner_scene),
      renderer(parent_renderer) {

  render_queue_2d.init();
}

RendererInstance::~RendererInstance() {}

static auto cull_meshes(
  GPU::CullFlags cull_flags,
  u32 mesh_instance_count,
  VkContext& ctx,
  vuk::Value<vuk::Buffer>& meshes_buffer,
  vuk::Value<vuk::Buffer>& mesh_instances_buffer,
  vuk::Value<vuk::Buffer>& meshlet_instances_buffer,
  vuk::Value<vuk::Buffer>& visible_meshlet_instances_count_buffer,
  vuk::Value<vuk::Buffer>& transforms_buffer,
  vuk::Value<vuk::ImageAttachment>& hiz_attachment,
  vuk::Value<vuk::Buffer>& camera_buffer
) -> vuk::Value<vuk::Buffer> {
  ZoneScoped;

  auto vis_cull_meshes_pass = vuk::make_pass(
    "vis cull meshes",
    [cull_flags, mesh_instance_count](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eComputeRead) camera,
      VUK_BA(vuk::eComputeRead) meshes,
      VUK_BA(vuk::eComputeRead) transforms,
      VUK_IA(vuk::eComputeSampled) hiz,
      VUK_BA(vuk::eComputeRW) mesh_instances,
      VUK_BA(vuk::eComputeRW) meshlet_instances,
      VUK_BA(vuk::eComputeRW) visible_meshlet_instances_count
    ) {
      cmd_list //
        .bind_compute_pipeline("cull_meshes")
        .bind_buffer(0, 0, camera)
        .bind_buffer(0, 1, meshes)
        .bind_buffer(0, 2, transforms)
        .bind_image(0, 3, hiz)
        .bind_sampler(0, 4, hiz_sampler_info)
        .bind_buffer(0, 5, mesh_instances)
        .bind_buffer(0, 6, meshlet_instances)
        .bind_buffer(0, 7, visible_meshlet_instances_count)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(mesh_instance_count, cull_flags))
        .dispatch_invocations(mesh_instance_count);

      return std::make_tuple(
        camera, meshes, transforms, hiz, mesh_instances, meshlet_instances, visible_meshlet_instances_count
      );
    }
  );

  std::tie(
    camera_buffer,
    meshes_buffer,
    transforms_buffer,
    hiz_attachment,
    mesh_instances_buffer,
    meshlet_instances_buffer,
    visible_meshlet_instances_count_buffer
  ) =
    vis_cull_meshes_pass(
      std::move(camera_buffer),
      std::move(meshes_buffer),
      std::move(transforms_buffer),
      std::move(hiz_attachment),
      std::move(mesh_instances_buffer),
      std::move(meshlet_instances_buffer),
      std::move(visible_meshlet_instances_count_buffer)
    );

  auto generate_cull_commands_pass = vuk::make_pass(
    "generate cull commands",
    [](
      vuk::CommandBuffer& cmd_list, //
      VUK_BA(vuk::eComputeRead) visible_meshlet_instances_count,
      VUK_BA(vuk::eComputeRW) cull_meshlets_cmd
    ) {
      cmd_list //
        .bind_compute_pipeline("generate_cull_commands")
        .bind_buffer(0, 0, visible_meshlet_instances_count)
        .bind_buffer(0, 1, cull_meshlets_cmd)
        .dispatch(1);

      return std::make_tuple(visible_meshlet_instances_count, cull_meshlets_cmd);
    }
  );

  auto cull_meshlets_cmd_buffer = ctx.scratch_buffer<vuk::DispatchIndirectCommand>({.x = 0, .y = 1, .z = 1});
  std::tie(visible_meshlet_instances_count_buffer, cull_meshlets_cmd_buffer) = generate_cull_commands_pass(
    std::move(visible_meshlet_instances_count_buffer), std::move(cull_meshlets_cmd_buffer)
  );

  return cull_meshlets_cmd_buffer;
}

static auto cull_meshlets(
  bool late,
  GPU::CullFlags cull_flags,
  VkContext& ctx,
  vuk::Value<vuk::ImageAttachment>& hiz_attachment,
  vuk::Value<vuk::Buffer>& cull_meshlets_cmd_buffer,
  vuk::Value<vuk::Buffer>& visible_meshlet_instances_count_buffer,
  vuk::Value<vuk::Buffer>& visible_meshlet_instances_indices_buffer,
  vuk::Value<vuk::Buffer>& meshlet_instance_visibility_mask_buffer,
  vuk::Value<vuk::Buffer>& reordered_indices_buffer,
  vuk::Value<vuk::Buffer>& meshes_buffer,
  vuk::Value<vuk::Buffer>& mesh_instances_buffer,
  vuk::Value<vuk::Buffer>& meshlet_instances_buffer,
  vuk::Value<vuk::Buffer>& transforms_buffer,
  vuk::Value<vuk::Buffer>& camera_buffer
) -> vuk::Value<vuk::Buffer> {
  ZoneScoped;
  memory::ScopedStack stack;

  //  ── CULL MESHLETS ───────────────────────────────────────────────────
  auto vis_cull_meshlets_pass = vuk::make_pass(
    stack.format("vis cull meshlets {}", late ? "late" : "early"),
    [late, cull_flags](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eIndirectRead) dispatch_cmd,
      VUK_BA(vuk::eComputeRead) camera,
      VUK_BA(vuk::eComputeRead) meshlet_instances,
      VUK_BA(vuk::eComputeRead) mesh_instances,
      VUK_BA(vuk::eComputeRead) meshes,
      VUK_BA(vuk::eComputeRead) transforms,
      VUK_IA(vuk::eComputeSampled) hiz,
      VUK_BA(vuk::eComputeRW) visible_meshlet_instances_count,
      VUK_BA(vuk::eComputeRW) visible_meshlet_instances_indices,
      VUK_BA(vuk::eComputeRW) meshlet_instance_visibility_mask,
      VUK_BA(vuk::eComputeRW) cull_triangles_cmd
    ) {
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
        .bind_buffer(0, 8, visible_meshlet_instances_indices)
        .bind_buffer(0, 9, meshlet_instance_visibility_mask)
        .bind_buffer(0, 10, cull_triangles_cmd)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, cull_flags)
        .specialize_constants(0, late)
        .dispatch_indirect(dispatch_cmd);

      return std::make_tuple(
        dispatch_cmd,
        camera,
        meshlet_instances,
        mesh_instances,
        meshes,
        transforms,
        hiz,
        visible_meshlet_instances_count,
        visible_meshlet_instances_indices,
        meshlet_instance_visibility_mask,
        cull_triangles_cmd
      );
    }
  );

  auto cull_triangles_cmd_buffer = ctx.scratch_buffer<vuk::DispatchIndirectCommand>({.x = 0, .y = 1, .z = 1});

  std::tie(
    cull_meshlets_cmd_buffer,
    camera_buffer,
    meshlet_instances_buffer,
    mesh_instances_buffer,
    meshes_buffer,
    transforms_buffer,
    hiz_attachment,
    visible_meshlet_instances_count_buffer,
    visible_meshlet_instances_indices_buffer,
    meshlet_instance_visibility_mask_buffer,
    cull_triangles_cmd_buffer
  ) =
    vis_cull_meshlets_pass(
      std::move(cull_meshlets_cmd_buffer),
      std::move(camera_buffer),
      std::move(meshlet_instances_buffer),
      std::move(mesh_instances_buffer),
      std::move(meshes_buffer),
      std::move(transforms_buffer),
      std::move(hiz_attachment),
      std::move(visible_meshlet_instances_count_buffer),
      std::move(visible_meshlet_instances_indices_buffer),
      std::move(meshlet_instance_visibility_mask_buffer),
      std::move(cull_triangles_cmd_buffer)
    );

  //  ── CULL TRIANGLES ──────────────────────────────────────────────────
  auto vis_cull_triangles_pass = vuk::make_pass(
    stack.format("vis cull triangles {}", late ? "late" : "early"),
    [late, cull_flags](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eIndirectRead) cull_triangles_cmd,
      VUK_BA(vuk::eComputeRead) camera,
      VUK_BA(vuk::eComputeRead) visible_meshlet_instances_count,
      VUK_BA(vuk::eComputeRead) visible_meshlet_instances_indices,
      VUK_BA(vuk::eComputeRead) meshlet_instances,
      VUK_BA(vuk::eComputeRead) mesh_instances,
      VUK_BA(vuk::eComputeRead) meshes,
      VUK_BA(vuk::eComputeRead) transforms,
      VUK_BA(vuk::eComputeRW) draw_indexed_cmd,
      VUK_BA(vuk::eComputeWrite) reordered_indices
    ) {
      cmd_list //
        .bind_compute_pipeline("cull_triangles")
        .bind_buffer(0, 0, camera)
        .bind_buffer(0, 1, visible_meshlet_instances_count)
        .bind_buffer(0, 2, visible_meshlet_instances_indices)
        .bind_buffer(0, 3, meshlet_instances)
        .bind_buffer(0, 4, mesh_instances)
        .bind_buffer(0, 5, meshes)
        .bind_buffer(0, 6, transforms)
        .bind_buffer(0, 7, draw_indexed_cmd)
        .bind_buffer(0, 8, reordered_indices)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, cull_flags)
        .specialize_constants(0, late)
        .dispatch_indirect(cull_triangles_cmd);

      return std::make_tuple(
        camera,
        visible_meshlet_instances_count,
        visible_meshlet_instances_indices,
        meshlet_instances,
        mesh_instances,
        meshes,
        transforms,
        draw_indexed_cmd,
        reordered_indices
      );
    }
  );

  auto draw_command_buffer = ctx.scratch_buffer<vuk::DrawIndexedIndirectCommand>({.instanceCount = 1});

  std::tie(
    camera_buffer,
    visible_meshlet_instances_count_buffer,
    visible_meshlet_instances_indices_buffer,
    meshlet_instances_buffer,
    mesh_instances_buffer,
    meshes_buffer,
    transforms_buffer,
    draw_command_buffer,
    reordered_indices_buffer
  ) =
    vis_cull_triangles_pass(
      std::move(cull_triangles_cmd_buffer),
      std::move(camera_buffer),
      std::move(visible_meshlet_instances_count_buffer),
      std::move(visible_meshlet_instances_indices_buffer),
      std::move(meshlet_instances_buffer),
      std::move(mesh_instances_buffer),
      std::move(meshes_buffer),
      std::move(transforms_buffer),
      std::move(draw_command_buffer),
      std::move(reordered_indices_buffer)
    );

  return draw_command_buffer;
}

static auto draw_visbuffer(
  bool late,
  vuk::PersistentDescriptorSet& descriptor_set,
  vuk::Value<vuk::ImageAttachment>& depth_attachment,
  vuk::Value<vuk::ImageAttachment>& visbuffer_attachment,
  vuk::Value<vuk::ImageAttachment>& overdraw_attachment,
  vuk::Value<vuk::Buffer>& draw_command_buffer,
  vuk::Value<vuk::Buffer>& reordered_indices_buffer,
  vuk::Value<vuk::Buffer>& meshes_buffer,
  vuk::Value<vuk::Buffer>& mesh_instances_buffer,
  vuk::Value<vuk::Buffer>& meshlet_instances_buffer,
  vuk::Value<vuk::Buffer>& transforms_buffer,
  vuk::Value<vuk::Buffer>& materials_buffer,
  vuk::Value<vuk::Buffer>& camera_buffer
) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  auto vis_encode_pass = vuk::make_pass(
    stack.format("vis encode {}", late ? "late" : "early"),
    [&descriptor_set](
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
      VUK_IA(vuk::eFragmentRW) overdraw
    ) {
      cmd_list //
        .bind_graphics_pipeline("visbuffer_encode")
        .set_rasterization({.cullMode = vuk::CullModeFlagBits::eBack})
        .set_depth_stencil(
          {.depthTestEnable = true, .depthWriteEnable = true, .depthCompareOp = vuk::CompareOp::eGreaterOrEqual}
        )
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
        index_buffer,
        camera,
        meshlet_instances,
        mesh_instances,
        meshes,
        transforms,
        materials,
        visbuffer,
        depth,
        overdraw
      );
    }
  );

  std::tie(
    reordered_indices_buffer,
    camera_buffer,
    meshlet_instances_buffer,
    mesh_instances_buffer,
    meshes_buffer,
    transforms_buffer,
    materials_buffer,
    visbuffer_attachment,
    depth_attachment,
    overdraw_attachment
  ) =
    vis_encode_pass(
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
      std::move(overdraw_attachment)
    );
}

static auto
draw_hiz(vuk::Value<vuk::ImageAttachment>& hiz_attachment, vuk::Value<vuk::ImageAttachment>& depth_attachment) -> void {
  ZoneScoped;

  auto hiz_generate_pass = vuk::make_pass(
    "hiz generate",
    [](
      vuk::CommandBuffer& cmd_list, //
      VUK_IA(vuk::eComputeSampled) src,
      VUK_IA(vuk::eComputeRW) dst
    ) {
      auto extent = dst->extent;
      auto mip_count = dst->level_count;

      cmd_list //
        .bind_compute_pipeline("hiz")
        .bind_sampler(0, 0, hiz_sampler_info);

      for (auto i = 0_u32; i < mip_count; i++) {
        auto mip_width = std::max(1_u32, extent.width >> i);
        auto mip_height = std::max(1_u32, extent.height >> i);

        if (i == 0) {
          cmd_list.bind_image(0, 1, src);
        } else {
          auto prev_mip = dst->mip(i - 1);
          cmd_list.image_barrier(prev_mip, vuk::eComputeWrite, vuk::eComputeSampled);
          cmd_list.bind_image(0, 1, prev_mip);
        }

        auto cur_mip = dst->mip(i);
        cmd_list.bind_image(0, 2, cur_mip);
        cmd_list.push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(mip_width, mip_height, i));
        cmd_list.dispatch_invocations(mip_width, mip_height);
      }

      cmd_list.image_barrier(dst, vuk::eComputeSampled, vuk::eComputeRW);

      return std::make_tuple(src, dst);
    }
  );

  std::tie(depth_attachment, hiz_attachment) = hiz_generate_pass(
    std::move(depth_attachment), std::move(hiz_attachment)
  );
}

auto RendererInstance::render(this RendererInstance& self, const Renderer::RenderInfo& render_info)
  -> vuk::Value<vuk::ImageAttachment> {
  ZoneScoped;

  self.viewport_size = {render_info.extent.width, render_info.extent.height};
  self.viewport_offset = render_info.viewport_offset;

  auto& vk_context = App::get_vkcontext();
  auto& bindless_set = vk_context.get_descriptor_set();

  self.camera_data.resolution = {render_info.extent.width, render_info.extent.height};
  auto camera_buffer = self.renderer.vk_context->scratch_buffer(std::span(&self.camera_data, 1));

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

    self.gpu_scene.atmosphere = *self.atmosphere;
    self.gpu_scene.scene_flags |= GPU::SceneFlags::HasAtmosphere;
  }
  auto sun_buffer = vuk::Value<vuk::Buffer>{};
  if (self.sun.has_value()) {
    sun_buffer = self.renderer.vk_context->scratch_buffer(self.sun);

    self.gpu_scene.sun = *self.sun;
    self.gpu_scene.scene_flags |= GPU::SceneFlags::HasSun;
  }

  auto scene_buffer = self.renderer.vk_context->scratch_buffer(std::span(&self.gpu_scene, 1));

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
     .sample_count = vuk::Samples::e1}
  );
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

  const auto hiz_extent = vuk::Extent3D{
    .width = (depth_ia.extent.width + 1_u32) >> 1_u32,
    .height = (depth_ia.extent.height + 1_u32) >> 1_u32,
    .depth = 1,
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
    hiz_attachment = self.hiz_view.acquire("hiz", vuk::eComputeSampled);
  }

  auto sky_transmittance_lut_attachment = self.renderer.sky_transmittance_lut_view.acquire(
    "sky_transmittance_lut", vuk::Access::eComputeSampled
  );
  auto sky_multiscatter_lut_attachment = self.renderer.sky_multiscatter_lut_view.acquire(
    "sky_multiscatter_lut", vuk::Access::eComputeSampled
  );

  const auto debug_view = static_cast<GPU::DebugView>(RendererCVar::cvar_debug_view.get());
  const f32 debug_heatmap_scale = 5.0;
  const auto debugging = debug_view != GPU::DebugView::None;

  auto transforms_buffer = std::move(self.prepared_frame.transforms_buffer);
  auto materials_buffer = std::move(self.prepared_frame.materials_buffer);

  if (static_cast<bool>(RendererCVar::cvar_bloom_enable.get()))
    self.gpu_scene.scene_flags |= GPU::SceneFlags::HasBloom;
  if (static_cast<bool>(RendererCVar::cvar_fxaa_enable.get()))
    self.gpu_scene.scene_flags |= GPU::SceneFlags::HasFXAA;
  if (static_cast<bool>(RendererCVar::cvar_vbgtao_enable.get()))
    self.gpu_scene.scene_flags |= GPU::SceneFlags::HasGTAO;

  // --- 3D Pass ---
  if (self.prepared_frame.mesh_instance_count > 0) {
    auto meshes_buffer = std::move(self.prepared_frame.meshes_buffer);
    auto mesh_instances_buffer = std::move(self.prepared_frame.mesh_instances_buffer);
    auto point_lights_buffer = std::move(self.prepared_frame.point_lights_buffer);
    auto spot_lights_buffer = std::move(self.prepared_frame.spot_lights_buffer);
    auto meshlet_instance_visibility_mask_buffer = std::move(
      self.prepared_frame.meshlet_instance_visibility_mask_buffer
    );
    auto meshlet_instances_buffer = vk_context.alloc_transient_buffer(
      vuk::MemoryUsage::eGPUonly, self.prepared_frame.max_meshlet_instance_count * sizeof(GPU::MeshletInstance)
    );
    auto visible_meshlet_instances_indices_buffer = vk_context.alloc_transient_buffer(
      vuk::MemoryUsage::eGPUonly, self.prepared_frame.max_meshlet_instance_count * sizeof(u32)
    );
    auto reordered_indices_buffer = vk_context.alloc_transient_buffer(
      vuk::MemoryUsage::eGPUonly,
      self.prepared_frame.max_meshlet_instance_count * Model::MAX_MESHLET_PRIMITIVES * 3 * sizeof(u32)
    );
    auto visible_meshlet_instances_count_buffer = vk_context.scratch_buffer<u32[3]>({});

    auto cull_flags = GPU::CullFlags::MicroTriangles | GPU::CullFlags::TriangleBackFace;
    if (static_cast<bool>(RendererCVar::cvar_culling_frustum.get())) {
      cull_flags |= GPU::CullFlags::MeshletFrustum;
    }
    if (static_cast<bool>(RendererCVar::cvar_culling_occlusion.get())) {
      cull_flags |= GPU::CullFlags::MeshletOcclusion;
    }
    if (static_cast<bool>(RendererCVar::cvar_culling_triangle.get())) {
      cull_flags |= GPU::CullFlags::TriangleBackFace;
      cull_flags |= GPU::CullFlags::MicroTriangles;
    }

    auto visbuffer_attachment = vuk::declare_ia(
      "visbuffer",
      {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
       .format = vuk::Format::eR32Uint,
       .sample_count = vuk::Samples::e1}
    );
    visbuffer_attachment.same_shape_as(final_attachment);

    auto overdraw_attachment = vuk::declare_ia(
      "overdraw",
      {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage,
       .format = vuk::Format::eR32Uint,
       .sample_count = vuk::Samples::e1}
    );
    overdraw_attachment.same_shape_as(final_attachment);

    auto vis_clear_pass = vuk::make_pass(
      "vis clear",
      [](
        vuk::CommandBuffer& cmd_list, //
        VUK_IA(vuk::eComputeWrite) visbuffer,
        VUK_IA(vuk::eComputeWrite) overdraw
      ) {
        cmd_list //
          .bind_compute_pipeline("visbuffer_clear")
          .bind_image(0, 0, visbuffer)
          .bind_image(0, 1, overdraw)
          .push_constants(
            vuk::ShaderStageFlagBits::eCompute,
            0,
            PushConstants(glm::uvec2(visbuffer->extent.width, visbuffer->extent.height))
          )
          .dispatch_invocations_per_pixel(visbuffer);

        return std::make_tuple(visbuffer, overdraw);
      }
    );

    std::tie(visbuffer_attachment, overdraw_attachment) = vis_clear_pass(
      std::move(visbuffer_attachment), std::move(overdraw_attachment)
    );

    auto cull_meshlets_cmd_buffer = cull_meshes(
      cull_flags,
      self.prepared_frame.mesh_instance_count,
      vk_context,
      meshes_buffer,
      mesh_instances_buffer,
      meshlet_instances_buffer,
      visible_meshlet_instances_count_buffer,
      transforms_buffer,
      hiz_attachment,
      camera_buffer
    );

    auto early_draw_visbuffer_cmd_buffer = cull_meshlets(
      false,
      cull_flags,
      vk_context,
      hiz_attachment,
      cull_meshlets_cmd_buffer,
      visible_meshlet_instances_count_buffer,
      visible_meshlet_instances_indices_buffer,
      meshlet_instance_visibility_mask_buffer,
      reordered_indices_buffer,
      meshes_buffer,
      mesh_instances_buffer,
      meshlet_instances_buffer,
      transforms_buffer,
      camera_buffer
    );

    draw_visbuffer(
      false,
      bindless_set,
      depth_attachment,
      visbuffer_attachment,
      overdraw_attachment,
      early_draw_visbuffer_cmd_buffer,
      reordered_indices_buffer,
      meshes_buffer,
      mesh_instances_buffer,
      meshlet_instances_buffer,
      transforms_buffer,
      materials_buffer,
      camera_buffer
    );

    draw_hiz(hiz_attachment, depth_attachment);

    auto late_draw_visbuffer_cmd_buffer = cull_meshlets(
      true,
      cull_flags,
      vk_context,
      hiz_attachment,
      cull_meshlets_cmd_buffer,
      visible_meshlet_instances_count_buffer,
      visible_meshlet_instances_indices_buffer,
      meshlet_instance_visibility_mask_buffer,
      reordered_indices_buffer,
      meshes_buffer,
      mesh_instances_buffer,
      meshlet_instances_buffer,
      transforms_buffer,
      camera_buffer
    );

    draw_visbuffer(
      true,
      bindless_set,
      depth_attachment,
      visbuffer_attachment,
      overdraw_attachment,
      late_draw_visbuffer_cmd_buffer,
      reordered_indices_buffer,
      meshes_buffer,
      mesh_instances_buffer,
      meshlet_instances_buffer,
      transforms_buffer,
      materials_buffer,
      camera_buffer
    );

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
                                 materials,
                                 visbuffer,
                                 albedo,
                                 normal,
                                 emissive,
                                 metallic_roughness_occlusion);
        });

    auto albedo_attachment = vuk::declare_ia(
      "albedo",
      {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
       .format = vuk::Format::eR8G8B8A8Srgb,
       .sample_count = vuk::Samples::e1}
    );
    albedo_attachment.same_shape_as(visbuffer_attachment);
    albedo_attachment = vuk::clear_image(std::move(albedo_attachment), vuk::Black<f32>);

    auto normal_attachment = vuk::declare_ia(
      "normal",
      {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
       .format = vuk::Format::eR16G16B16A16Sfloat,
       .sample_count = vuk::Samples::e1}
    );
    normal_attachment.same_shape_as(visbuffer_attachment);
    normal_attachment = vuk::clear_image(std::move(normal_attachment), vuk::Black<f32>);

    auto emissive_attachment = vuk::declare_ia(
      "emissive",
      {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
       .format = vuk::Format::eB10G11R11UfloatPack32,
       .sample_count = vuk::Samples::e1}
    );
    emissive_attachment.same_shape_as(visbuffer_attachment);
    emissive_attachment = vuk::clear_image(std::move(emissive_attachment), vuk::Black<f32>);

    auto metallic_roughness_occlusion_attachment = vuk::declare_ia(
      "metallic roughness occlusion",
      {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
       .format = vuk::Format::eR8G8B8A8Unorm,
       .sample_count = vuk::Samples::e1}
    );
    metallic_roughness_occlusion_attachment.same_shape_as(visbuffer_attachment);
    metallic_roughness_occlusion_attachment = vuk::clear_image(
      std::move(metallic_roughness_occlusion_attachment), vuk::Black<f32>
    );

    std::tie(camera_buffer,
             meshlet_instances_buffer,
             mesh_instances_buffer,
             meshes_buffer,
             transforms_buffer,
             materials_buffer,
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

    auto vbgtao_occlusion_attachment = vuk::declare_ia(
      "vbgtao occlusion",
      {.format = vuk::Format::eR16Sfloat,
       .sample_count = vuk::Samples::e1,
       .view_type = vuk::ImageViewType::e2D,
       .level_count = 1,
       .layer_count = 1}
    );
    vbgtao_occlusion_attachment.same_extent_as(depth_attachment);
    vbgtao_occlusion_attachment = vuk::clear_image(std::move(vbgtao_occlusion_attachment), vuk::White<f32>);

    auto vbgtao_noisy_occlusion_attachment = vuk::declare_ia(
      "vbgtao noisy occlusion",
      {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage,
       .format = vuk::Format::eR16Sfloat,
       .sample_count = vuk::Samples::e1}
    );
    vbgtao_noisy_occlusion_attachment.same_shape_as(final_attachment);

    if (self.vbgtao_info.has_value() && (self.gpu_scene.scene_flags & GPU::SceneFlags::HasGTAO)) {
      auto vbgtao_prefilter_pass = vuk::make_pass(
        "vbgtao prefilter",
        [](
          vuk::CommandBuffer& command_buffer, //
          VUK_IA(vuk::eComputeSampled) depth_input,
          VUK_IA(vuk::eComputeRW) dst_image
        ) {
          auto nearest_clamp_sampler = vuk::SamplerCreateInfo{
            .magFilter = vuk::Filter::eNearest,
            .minFilter = vuk::Filter::eNearest,
            .addressModeU = vuk::SamplerAddressMode::eClampToEdge,
            .addressModeV = vuk::SamplerAddressMode::eClampToEdge,
            .addressModeW = vuk::SamplerAddressMode::eClampToEdge,
          };

          command_buffer.bind_compute_pipeline("vbgtao_prefilter_pipeline")
            .bind_image(0, 0, depth_input)
            .bind_image(0, 1, dst_image->mip(0))
            .bind_image(0, 2, dst_image->mip(1))
            .bind_image(0, 3, dst_image->mip(2))
            .bind_image(0, 4, dst_image->mip(3))
            .bind_image(0, 5, dst_image->mip(4))
            .bind_sampler(0, 6, nearest_clamp_sampler)
            .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, depth_input->extent)
            .dispatch((depth_input->extent.width + 16 - 1) / 16, (depth_input->extent.height + 16 - 1) / 16);

          return std::make_tuple(depth_input, dst_image);
        }
      );

      auto vbgtao_depth_attachment = vuk::declare_ia(
        "vbgtao depth",
        {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage,
         .format = vuk::Format::eR32Sfloat,
         .sample_count = vuk::Samples::e1,
         .level_count = 5,
         .layer_count = 1}
      );
      vbgtao_depth_attachment.same_extent_as(depth_attachment);
      vbgtao_depth_attachment = vuk::clear_image(std::move(vbgtao_depth_attachment), vuk::Black<f32>);

      std::tie(depth_attachment, vbgtao_depth_attachment) = vbgtao_prefilter_pass(
        std::move(depth_attachment), std::move(vbgtao_depth_attachment)
      );

      auto vbgtao_generate_pass = vuk::make_pass( //
          "vbgtao generate",
          [inf = *self.vbgtao_info](vuk::CommandBuffer& command_buffer,
                                    VUK_BA(vuk::eComputeUniformRead) camera,
                                    VUK_IA(vuk::eComputeSampled) prefiltered_depth,
                                    VUK_IA(vuk::eComputeSampled) normals,
                                    VUK_IA(vuk::eComputeSampled) hilbert_noise,
                                    VUK_IA(vuk::eComputeRW) ambient_occlusion,
                                    VUK_IA(vuk::eComputeRW) depth_differences) {
            auto nearest_clamp_sampler = vuk::SamplerCreateInfo{
                .magFilter = vuk::Filter::eNearest,
                .minFilter = vuk::Filter::eNearest,
                .mipmapMode = vuk::SamplerMipmapMode::eNearest,
                .addressModeU = vuk::SamplerAddressMode::eClampToEdge,
                .addressModeV = vuk::SamplerAddressMode::eClampToEdge,
                .addressModeW = vuk::SamplerAddressMode::eClampToEdge,
            };

            auto linear_clamp_sampler = vuk::SamplerCreateInfo{
                .magFilter = vuk::Filter::eLinear,
                .minFilter = vuk::Filter::eLinear,
                .mipmapMode = vuk::SamplerMipmapMode::eLinear,
                .addressModeU = vuk::SamplerAddressMode::eClampToEdge,
                .addressModeV = vuk::SamplerAddressMode::eClampToEdge,
                .addressModeW = vuk::SamplerAddressMode::eClampToEdge,
            };

            command_buffer.bind_compute_pipeline("vbgtao_main_pipeline")
                .bind_buffer(0, 0, camera)
                .bind_image(0, 1, prefiltered_depth)
                .bind_image(0, 2, normals)
                .bind_image(0, 3, hilbert_noise)
                .bind_image(0, 4, ambient_occlusion)
                .bind_image(0, 5, depth_differences)
                .bind_sampler(0, 6, nearest_clamp_sampler)
                .bind_sampler(0, 7, linear_clamp_sampler)
                .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(inf))
                .dispatch_invocations_per_pixel(ambient_occlusion);

            return std::make_tuple(camera, normals, ambient_occlusion, depth_differences);
          });

      auto vbgtao_depth_differences_attachment = vuk::declare_ia(
        "vbgtao depth differences",
        {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage,
         .format = vuk::Format::eR32Uint,
         .sample_count = vuk::Samples::e1}
      );
      vbgtao_depth_differences_attachment.same_shape_as(final_attachment);
      vbgtao_depth_differences_attachment = vuk::clear_image(
        std::move(vbgtao_depth_differences_attachment), vuk::Black<f32>
      );

      auto hilbert_noise_lut_attachment = self.renderer.hilbert_noise_lut.acquire(
        "hilbert noise", vuk::eComputeSampled
      );

      vbgtao_noisy_occlusion_attachment = vuk::clear_image(
        std::move(vbgtao_noisy_occlusion_attachment), vuk::White<f32>
      );

      std::tie(
        camera_buffer, normal_attachment, vbgtao_noisy_occlusion_attachment, vbgtao_depth_differences_attachment
      ) =
        vbgtao_generate_pass(
          std::move(camera_buffer),
          std::move(vbgtao_depth_attachment),
          std::move(normal_attachment),
          std::move(hilbert_noise_lut_attachment),
          std::move(vbgtao_noisy_occlusion_attachment),
          std::move(vbgtao_depth_differences_attachment)
        );

      auto vbgtao_denoise_pass = vuk::make_pass( //
          "vbgtao denoise",
          [inf = *self.vbgtao_info](vuk::CommandBuffer& command_buffer,
                                    VUK_IA(vuk::eComputeSampled) noisy_occlusion,
                                    VUK_IA(vuk::eComputeSampled) depth_differences,
                                    VUK_IA(vuk::eComputeRW) ambient_occlusion) {
            auto nearest_clamp_sampler = vuk::SamplerCreateInfo{
                .magFilter = vuk::Filter::eNearest,
                .minFilter = vuk::Filter::eNearest,
                .mipmapMode = vuk::SamplerMipmapMode::eNearest,
                .addressModeU = vuk::SamplerAddressMode::eClampToEdge,
                .addressModeV = vuk::SamplerAddressMode::eClampToEdge,
                .addressModeW = vuk::SamplerAddressMode::eClampToEdge,
            };

            glm::ivec2 occlusion_noisy_extent = {noisy_occlusion->extent.width, noisy_occlusion->extent.height};
            command_buffer.bind_compute_pipeline("vbgtao_denoise_pipeline")
                .bind_image(0, 0, noisy_occlusion)
                .bind_image(0, 1, depth_differences)
                .bind_image(0, 2, ambient_occlusion)
                .bind_sampler(0, 3, nearest_clamp_sampler)
                .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(occlusion_noisy_extent, inf))
                .dispatch_invocations_per_pixel(ambient_occlusion);

            return std::make_tuple(ambient_occlusion, noisy_occlusion);
          });

      std::tie(vbgtao_occlusion_attachment, vbgtao_noisy_occlusion_attachment) = vbgtao_denoise_pass(
        std::move(vbgtao_noisy_occlusion_attachment),
        std::move(vbgtao_depth_differences_attachment),
        std::move(vbgtao_occlusion_attachment)
      );
    }

    if (!debugging && self.atmosphere.has_value() && self.sun.has_value()) {
      // --- BRDF ---
      auto brdf_pass = vuk::make_pass(
          "brdf",
          [scene_flags = self.gpu_scene.scene_flags]( //
              vuk::CommandBuffer& cmd_list,
              VUK_IA(vuk::eColorWrite) dst,
              VUK_IA(vuk::eFragmentSampled) sky_transmittance_lut,
              VUK_IA(vuk::eFragmentSampled) sky_multiscatter_lut,
              VUK_IA(vuk::eFragmentSampled) depth,
              VUK_IA(vuk::eFragmentSampled) albedo,
              VUK_IA(vuk::eFragmentSampled) normal,
              VUK_IA(vuk::eFragmentSampled) emissive,
              VUK_IA(vuk::eFragmentSampled) metallic_roughness_occlusion,
              VUK_IA(vuk::eFragmentSampled) gtao,
              VUK_BA(vuk::eFragmentRead) scene,
              VUK_BA(vuk::eFragmentRead) camera,
              VUK_BA(vuk::eFragmentRead) point_lights,
              VUK_BA(vuk::eFragmentRead) spot_lights) {
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

            cmd_list.bind_graphics_pipeline("brdf")
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
                .bind_image(0, 9, gtao)
                .bind_buffer(0, 10, scene)
                .bind_buffer(0, 11, camera)
                .push_constants(vuk::ShaderStageFlagBits::eFragment, 0, PushConstants(scene_flags))
                .draw(3, 1, 0, 0);
            return std::make_tuple(dst, sky_transmittance_lut, sky_multiscatter_lut, depth, scene, camera);
          });

      std::tie(
        final_attachment,
        sky_transmittance_lut_attachment,
        sky_multiscatter_lut_attachment,
        depth_attachment,
        scene_buffer,
        camera_buffer
      ) =
        brdf_pass(
          std::move(final_attachment),
          std::move(sky_transmittance_lut_attachment),
          std::move(sky_multiscatter_lut_attachment),
          std::move(depth_attachment),
          std::move(albedo_attachment),
          std::move(normal_attachment),
          std::move(emissive_attachment),
          std::move(metallic_roughness_occlusion_attachment),
          std::move(vbgtao_occlusion_attachment),
          std::move(scene_buffer),
          std::move(camera_buffer),
          std::move(point_lights_buffer),
          std::move(spot_lights_buffer)
        );
    } else {
      const auto debug_attachment_ia = vuk::ImageAttachment{
        .usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
        .extent = render_info.extent,
        .format = vuk::Format::eR16G16B16A16Sfloat,
        .sample_count = vuk::Samples::e1,
        .level_count = 1,
        .layer_count = 1,
      };
      auto debug_attachment = vuk::clear_image(
        vuk::declare_ia("debug_attachment", debug_attachment_ia), vuk::Black<float>
      );

      std::tie(debug_attachment, depth_attachment, camera_buffer, vbgtao_noisy_occlusion_attachment) = vuk::make_pass(
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
              VUK_IA(vuk::eFragmentSampled) gtao,
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
                .bind_image(0, 10, gtao)
                .bind_buffer(0, 11, camera)
                .bind_buffer(0, 12, visible_meshlet_instances_indices)
                .bind_buffer(0, 13, meshlet_instances)
                .bind_buffer(0, 14, meshes)
                .bind_buffer(0, 15, transforms_)
                .push_constants(vuk::ShaderStageFlagBits::eFragment,
                                0,
                                PushConstants(std::to_underlying(debug_view), debug_heatmap_scale))
                .draw(3, 1, 0, 0);

            return std::make_tuple(dst, depth, camera, gtao);
          })(std::move(debug_attachment),
             std::move(visbuffer_attachment),
             std::move(depth_attachment),
             std::move(overdraw_attachment),
             std::move(albedo_attachment),
             std::move(normal_attachment),
             std::move(emissive_attachment),
             std::move(metallic_roughness_occlusion_attachment),
             std::move(hiz_attachment),
             std::move(vbgtao_noisy_occlusion_attachment),
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
             materials_buffer,
             transforms_buffer) =
        vuk::make_pass(
            "2d_forward_pass",
            [rq2d = self.render_queue_2d, &descriptor_set = bindless_set]( //
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
                    .bind_persistent(1, descriptor_set)
                    .draw(6, batch.count, 0, batch.offset);
              }

              return std::make_tuple(target, depth, camera, vertex_buffer, materials, transforms_);
            })(std::move(final_attachment),
               std::move(depth_attachment),
               std::move(vertex_buffer_2d),
               std::move(materials_buffer),
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
       .layer_count = 1}
    );

    auto sky_aerial_perspective_attachment = vuk::declare_ia(
      "sky aerial perspective",
      {.image_type = vuk::ImageType::e3D,
       .usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage,
       .extent = sky_aerial_perspective_lut_extent,
       .sample_count = vuk::Samples::e1,
       .view_type = vuk::ImageViewType::e3D,
       .level_count = 1,
       .layer_count = 1}
    );
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

    std::tie(final_attachment, depth_attachment, camera_buffer) = vuk::
      make_pass("sky final", [](vuk::CommandBuffer& cmd_list, VUK_IA(vuk::eColorWrite) dst, VUK_BA(vuk::eFragmentRead) atmosphere_, VUK_BA(vuk::eFragmentRead) sun_, VUK_BA(vuk::eFragmentRead) camera, VUK_IA(vuk::eFragmentSampled) sky_transmittance_lut, VUK_IA(vuk::eFragmentSampled) sky_aerial_perspective_lut, VUK_IA(vuk::eFragmentSampled) sky_view_lut, VUK_IA(vuk::eFragmentSampled) depth) {
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
      })(std::move(final_attachment), std::move(atmosphere_buffer), std::move(sun_buffer), std::move(camera_buffer), std::move(sky_transmittance_lut_attachment), std::move(sky_aerial_perspective_attachment), std::move(sky_view_lut_attachment), std::move(depth_attachment));
  }

  // --- Post Processing ---
  if (!debugging) {
    // --- FXAA Pass ---
    if (self.gpu_scene.scene_flags & GPU::SceneFlags::HasFXAA) {
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

    if (self.gpu_scene.scene_flags & GPU::SceneFlags::HasBloom) {
      auto bloom_prefilter_pass = vuk::
        make_pass("bloom prefilter", [bloom_threshold, bloom_clamp](//
          vuk::CommandBuffer& cmd_list, VUK_IA(vuk::eComputeRead) src, VUK_IA(vuk::eComputeRW) out) {
          cmd_list //
            .bind_compute_pipeline("bloom_prefilter_pipeline")
            .bind_image(0, 0, out)
            .bind_image(0, 1, src)
            .bind_sampler(0, 2, vuk::NearestMagLinearMinSamplerClamped)
            .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(bloom_threshold, bloom_clamp))
            .dispatch_invocations_per_pixel(src);

          return std::make_tuple(src, out);
        });

      std::tie(final_attachment, bloom_down_image) = bloom_prefilter_pass(
        std::move(final_attachment), std::move(bloom_down_image)
      );

      auto bloom_downsample_pass = vuk::
        make_pass("bloom downsample", [](//
          vuk::CommandBuffer& cmd_list, VUK_IA(vuk::eComputeSampled) bloom) {
          cmd_list //
            .bind_compute_pipeline("bloom_downsample_pipeline")
            .bind_sampler(0, 2, vuk::LinearMipmapNearestSamplerClamped);

            auto extent = bloom->extent;
            auto mip_count = bloom->level_count;
            for (auto i = 1_u32; i < mip_count; i++) {
              auto mip_width = std::max(1_u32, extent.width >> i);
              auto mip_height = std::max(1_u32, extent.height >> i);
              auto prev_mip = bloom->mip(i - 1);
              auto mip = bloom->mip(i);

              cmd_list.image_barrier(prev_mip, vuk::eComputeWrite, vuk::eComputeSampled);
              cmd_list.bind_image(0, 0, mip);
              cmd_list.bind_image(0, 1, prev_mip);
              cmd_list.push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(mip_width, mip_height));
              cmd_list.dispatch_invocations(mip_width, mip_height);
            }

            cmd_list.image_barrier(bloom, vuk::eComputeSampled, vuk::eComputeRW);

          return bloom;
        });

      bloom_down_image = bloom_downsample_pass(std::move(bloom_down_image));

      // Upsampling
      // https://www.froyok.fr/blog/2021-12-ue4-custom-bloom/resources/code/bloom_down_up_demo.jpg

      auto bloom_upsample_pass = vuk::make_pass(
        "bloom_upsample",
        [](
          vuk::CommandBuffer& cmd_list, //
          VUK_IA(vuk::eComputeRW) bloom_upsampled,
          VUK_IA(vuk::eComputeSampled) bloom_downsampled
        ) {
          auto extent = bloom_upsampled->extent;
          auto mip_count = bloom_upsampled->level_count;

          cmd_list //
            .bind_compute_pipeline("bloom_upsample_pipeline")
            .bind_image(0, 1, bloom_downsampled->mip(mip_count - 1))
            .bind_sampler(0, 3, vuk::NearestMagLinearMinSamplerClamped);

          for (int32_t i = (int32_t)mip_count - 2; i >= 0; i--) {
            auto mip_width = std::max(1_u32, extent.width >> i);
            auto mip_height = std::max(1_u32, extent.height >> i);

            cmd_list.image_barrier(bloom_upsampled->mip(i), vuk::eComputeWrite, vuk::eComputeWrite);
            cmd_list.bind_image(0, 0, bloom_upsampled->mip(i));
            cmd_list.bind_image(0, 2, bloom_downsampled->mip(i));
            cmd_list.push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(mip_width, mip_height));
            cmd_list.dispatch_invocations(mip_width, mip_height);
          }

          return bloom_upsampled;
        }
      );

      bloom_up_image = bloom_upsample_pass(bloom_up_image, bloom_down_image);
    }

    // --- Auto Exposure Pass ---
    auto histogram_inf = self.histogram_info.value_or(GPU::HistogramInfo{});

    auto histogram_buffer = self.renderer.vk_context->alloc_transient_buffer(
      vuk::MemoryUsage::eGPUonly, GPU::HISTOGRAM_BIN_COUNT * sizeof(u32)
    );
    vuk::fill(histogram_buffer, 0);

    std::tie(final_attachment, histogram_buffer) = vuk::
      make_pass("histogram generate", [histogram_inf](vuk::CommandBuffer& cmd_list, VUK_IA(vuk::eComputeRead) src, VUK_BA(vuk::eComputeRW) histogram) {
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
      exposure_buffer_value = vuk::
        make_pass("histogram average", [pixel_count = f32(final_attachment->extent.width * final_attachment->extent.height), histogram_inf](vuk::CommandBuffer& cmd_list, VUK_BA(vuk::eComputeRW) histogram, VUK_BA(vuk::eComputeRW) exposure) {
          auto adaptation_time = glm::clamp(
            static_cast<f32>(
              1.0f - glm::exp(-histogram_inf.adaptation_speed * App::get()->get_timestep().get_millis() * 0.001)
            ),
            0.0f,
            1.0f
          );

          cmd_list //
            .bind_compute_pipeline("histogram_average_pipeline")
            .push_constants(
              vuk::ShaderStageFlagBits::eCompute,
              0,
              PushConstants(
                histogram->device_address,
                exposure->device_address,
                pixel_count,
                histogram_inf.min_exposure,
                histogram_inf.max_exposure - histogram_inf.min_exposure,
                adaptation_time,
                histogram_inf.ev100_bias
              )
            )
            .dispatch(1);

          return exposure;
        })(std::move(histogram_buffer), std::move(exposure_buffer_value));
    }

    // --- Tonemap Pass ---
    result_attachment = vuk::
      make_pass("tonemap", [scene_flags = self.gpu_scene.scene_flags, pp = self.post_proces_settings](vuk::CommandBuffer& cmd_list, VUK_IA(vuk::eColorWrite) dst, VUK_IA(vuk::eFragmentSampled) src, VUK_IA(vuk::eFragmentSampled) bloom_src, VUK_BA(vuk::eFragmentRead) exposure) {
        const auto size = glm::ivec2(src->extent.width, src->extent.height);
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
            vuk::ShaderStageFlagBits::eFragment, 0, PushConstants(exposure->device_address, scene_flags, pp, size)
          )
          .draw(3, 1, 0, 0);

        return dst;
      })(std::move(result_attachment), std::move(final_attachment), std::move(bloom_up_image), std::move(exposure_buffer_value));
  }

  auto debug_renderer_enabled = (bool)RendererCVar::cvar_enable_debug_renderer.get();

  if (debug_renderer_enabled &&
      (self.prepared_frame.line_index_count > 0 || self.prepared_frame.triangle_index_count > 0)) {
    auto debug_renderer_verticies_buffer = std::move(self.prepared_frame.debug_renderer_verticies_buffer);
    auto debug_renderer_pass = vuk::make_pass(
      "debug_renderer_pass",
      [line_index_count = self.prepared_frame.line_index_count](
        vuk::CommandBuffer& cmd_list,
        VUK_IA(vuk::eColorWrite) dst,
        VUK_IA(vuk::eFragmentSampled) depth_img,
        VUK_BA(vuk::eFragmentRead) dbg_vtx,
        VUK_BA(vuk::eFragmentRead) camera
      ) {
        auto& dbg_index_buffer = *DebugRenderer::get_instance()->get_global_index_buffer();

        cmd_list.bind_graphics_pipeline("debug_renderer_pipeline")
          .set_depth_stencil(
            vuk::PipelineDepthStencilStateCreateInfo{
              .depthTestEnable = false,
              .depthWriteEnable = false,
              .depthCompareOp = vuk::CompareOp::eGreaterOrEqual,
            }
          )
          .set_dynamic_state(vuk::DynamicStateFlagBits::eScissor | vuk::DynamicStateFlagBits::eViewport)
          .broadcast_color_blend(vuk::BlendPreset::eAlphaBlend)
          .set_rasterization(
            {.polygonMode = vuk::PolygonMode::eLine, .cullMode = vuk::CullModeFlagBits::eNone, .lineWidth = 3.f}
          )
          .set_primitive_topology(vuk::PrimitiveTopology::eLineList)
          .set_viewport(0, vuk::Rect2D::framebuffer())
          .set_scissor(0, vuk::Rect2D::framebuffer())
          .bind_vertex_buffer(0, dbg_vtx, 0, DebugRenderer::vertex_pack)
          .bind_index_buffer(dbg_index_buffer, vuk::IndexType::eUint32)
          .bind_buffer(0, 0, camera)
          .bind_image(0, 1, depth_img)
          .draw_indexed(line_index_count, 1, 0, 0, 0);

        return std::make_tuple(dst, camera, depth_img);
      }
    );

    std::tie(result_attachment, camera_buffer, depth_attachment) = debug_renderer_pass(
      result_attachment, depth_attachment, debug_renderer_verticies_buffer, camera_buffer
    );
  }

  return debugging ? final_attachment : result_attachment;
}

auto RendererInstance::update(this RendererInstance& self, RendererInstanceUpdateInfo& info) -> void {
  ZoneScoped;

  auto* asset_man = App::get_asset_manager();
  auto& vk_context = App::get_vkcontext();

  self.gpu_scene.scene_flags = {};

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

  std::vector<GPU::PointLight> point_lights = {};
  std::vector<GPU::SpotLight> spot_lights = {};

  self.scene->world
    .query_builder<const TransformComponent, const LightComponent>() //
    .build()
    .each([&sun_data, &atmosphere_data, cam, &point_lights, &spot_lights](
            flecs::entity e, const TransformComponent& tc, const LightComponent& lc
          ) {
      if (lc.type == LightComponent::LightType::Directional) {
        auto& sund = sun_data.emplace();
        sund.direction.x = glm::cos(tc.rotation.x) * glm::sin(tc.rotation.y);
        sund.direction.y = glm::sin(tc.rotation.x) * glm::sin(tc.rotation.y);
        sund.direction.z = glm::cos(tc.rotation.y);
        sund.intensity = lc.intensity;
      } else if (lc.type == LightComponent::LightType::Point) {
        const glm::vec3 world_pos = Scene::get_world_position(e);
        point_lights.emplace_back(
          GPU::PointLight{
            .position = world_pos,
            .color = lc.color,
            .intensity = lc.intensity,
            .cutoff = lc.radius,
          }
        );
      } else if (lc.type == LightComponent::LightType::Spot) {
        const glm::vec3 direction = {
          glm::cos(tc.rotation.x) * glm::sin(tc.rotation.y),
          -glm::sin(tc.rotation.x),
          glm::cos(tc.rotation.x) * glm::cos(tc.rotation.y),
        };

        const glm::vec3 world_pos = Scene::get_world_position(e);
        spot_lights.emplace_back(
          GPU::SpotLight{
            .position = world_pos,
            .direction = glm::normalize(direction),
            .color = lc.color,
            .intensity = lc.intensity,
            .cutoff = lc.radius,
            .inner_cone_angle = lc.inner_cone_angle,
            .outer_cone_angle = lc.outer_cone_angle,
          }
        );
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

  if (point_lights.empty()) {
    point_lights.emplace_back(GPU::PointLight{});
  }
  self.point_lights_buffer = vk_context.resize_buffer(
    std::move(self.point_lights_buffer), vuk::MemoryUsage::eGPUonly, std::span(point_lights).size_bytes()
  );
  self.prepared_frame.point_lights_buffer = vk_context.upload_staging(
    std::span(point_lights), *self.point_lights_buffer
  );

  if (spot_lights.empty()) {
    spot_lights.emplace_back(GPU::SpotLight{});
  }
  self.spot_lights_buffer = vk_context.resize_buffer(
    std::move(self.spot_lights_buffer), vuk::MemoryUsage::eGPUonly, std::span(spot_lights).size_bytes()
  );
  self.prepared_frame.spot_lights_buffer = vk_context.upload_staging(std::span(spot_lights), *self.spot_lights_buffer);

  self.gpu_scene.light_settings.point_light_count = (u32)point_lights.size();
  self.gpu_scene.light_settings.spot_light_count = (u32)spot_lights.size();

  self.gpu_scene.point_lights = self.point_lights_buffer->device_address;
  self.gpu_scene.spot_lights = self.spot_lights_buffer->device_address;

  self.render_queue_2d.init();

  self.scene->world
    .query_builder<const TransformComponent, const SpriteComponent>() //
    .build()
    .each([asset_man, &scene = self.scene, &cam, &rq2d = self.render_queue_2d](
            flecs::entity e, const TransformComponent& tc, const SpriteComponent& comp
          ) {
      const auto distance = glm::distance(glm::vec3(0.f, 0.f, cam.position.z), glm::vec3(0.f, 0.f, tc.position.z));
      if (auto* material = asset_man->get_asset(comp.material)) {
        if (auto transform_id = scene->get_entity_transform_id(e)) {
          rq2d.add(
            comp,
            tc.position.y,
            SlotMap_decode_id(*transform_id).index,
            SlotMap_decode_id(material->material_id).index,
            distance
          );
        } else {
          OX_LOG_WARN("No registered transform for sprite entity: {}", e.name().c_str());
        }
      }
    });

  self.scene->world
    .query_builder<const TransformComponent, const ParticleComponent>() //
    .build()
    .each([asset_man, &scene = self.scene, &cam, &rq2d = self.render_queue_2d](
            flecs::entity e, const TransformComponent& tc, const ParticleComponent& comp
          ) {
      if (comp.life_remaining <= 0.0f)
        return;

      const auto distance = glm::distance(glm::vec3(0.f, 0.f, cam.position.z), glm::vec3(0.f, 0.f, tc.position.z));

      auto particle_system_component = e.parent().try_get<ParticleSystemComponent>();
      if (particle_system_component) {
        if (auto* material = asset_man->get_asset(particle_system_component->material)) {
          if (auto transform_id = scene->get_entity_transform_id(e)) {
            SpriteComponent sprite_comp = {.sort_y = true};

            rq2d.add(
              sprite_comp,
              tc.position.y,
              SlotMap_decode_id(*transform_id).index,
              SlotMap_decode_id(material->material_id).index,
              distance
            );
          } else {
            OX_LOG_WARN("No registered transform for sprite entity: {}", e.name().c_str());
          }
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

  self.scene->world
    .query_builder<const TransformComponent, const VignetteComponent>() //
    .build()
    .each([&](flecs::entity e, const TransformComponent& tc, const VignetteComponent& c) {
      self.post_proces_settings.vignette_amount = c.amount;

      self.gpu_scene.scene_flags |= GPU::SceneFlags::HasVignette;
    });

  self.scene->world
    .query_builder<const TransformComponent, const ChromaticAberrationComponent>() //
    .build()
    .each([&](flecs::entity e, const TransformComponent& tc, const ChromaticAberrationComponent& c) {
      self.post_proces_settings.chromatic_aberration_amount = c.amount;

      self.gpu_scene.scene_flags |= GPU::SceneFlags::HasChromaticAberration;
    });

  self.scene->world
    .query_builder<const TransformComponent, const FilmGrainComponent>() //
    .build()
    .each([&](flecs::entity e, const TransformComponent& tc, const FilmGrainComponent& c) {
      self.post_proces_settings.film_grain_amount = c.amount;
      self.post_proces_settings.film_grain_scale = c.scale;
      self.post_proces_settings.film_grain_seed = vk_context.num_frames % 16;

      self.gpu_scene.scene_flags |= GPU::SceneFlags::HasFilmGrain;
    });

  update_buffer_if_dirty<GPU::Transforms>(self, vk_context, info.gpu_transforms, info.dirty_transform_ids);
  update_buffer_if_dirty<GPU::Material>(self, vk_context, info.gpu_materials, info.dirty_material_indices);

  auto zero_fill_pass = vuk::make_pass(
    "zero fill", [](vuk::CommandBuffer& command_buffer, VUK_BA(vuk::eTransferWrite) dst) {
      command_buffer.fill_buffer(dst, 0_u32);
      return dst;
    }
  );

  if (!info.gpu_meshes.empty()) {
    self.meshes_buffer = vk_context.resize_buffer(
      std::move(self.meshes_buffer), vuk::MemoryUsage::eGPUonly, info.gpu_meshes.size_bytes()
    );
    self.prepared_frame.meshes_buffer = vk_context.upload_staging(info.gpu_meshes, *self.meshes_buffer);
  } else if (self.meshes_buffer) {
    self.prepared_frame.meshes_buffer = vuk::acquire_buf("meshes", *self.meshes_buffer, vuk::Access::eMemoryRead);
  }

  if (!info.gpu_mesh_instances.empty()) {
    self.mesh_instances_buffer = vk_context.resize_buffer(
      std::move(self.mesh_instances_buffer), vuk::MemoryUsage::eGPUonly, info.gpu_mesh_instances.size_bytes()
    );
    self.prepared_frame.mesh_instances_buffer = vk_context.upload_staging(
      info.gpu_mesh_instances, *self.mesh_instances_buffer
    );

    auto meshlet_instance_visibility_mask_size_bytes = (info.max_meshlet_instance_count + 31) / 32 * sizeof(u32);
    self.meshlet_instance_visibility_mask_buffer = vk_context.resize_buffer(
      std::move(self.meshlet_instance_visibility_mask_buffer),
      vuk::MemoryUsage::eGPUonly,
      meshlet_instance_visibility_mask_size_bytes
    );
    auto meshlet_instance_visibility_mask_buffer = vuk::acquire_buf(
      "meshlet instances visibility mask", *self.meshlet_instance_visibility_mask_buffer, vuk::eNone
    );
    self.prepared_frame.meshlet_instance_visibility_mask_buffer = zero_fill_pass(
      std::move(meshlet_instance_visibility_mask_buffer)
    );
  } else if (self.mesh_instances_buffer) {
    self.prepared_frame.mesh_instances_buffer = vuk::acquire_buf(
      "mesh instances", *self.mesh_instances_buffer, vuk::Access::eMemoryRead
    );
    self.prepared_frame.meshlet_instance_visibility_mask_buffer = vuk::acquire_buf(
      "meshlet instances visibility mask", *self.meshlet_instance_visibility_mask_buffer, vuk::eMemoryRead
    );
  }

  self.prepared_frame.mesh_instance_count = info.mesh_instance_count;
  self.prepared_frame.max_meshlet_instance_count = info.max_meshlet_instance_count;

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
        std::move(self.debug_renderer_verticies_buffer), vuk::MemoryUsage::eGPUonly, vertices_span.size_bytes()
      );
      self.prepared_frame.debug_renderer_verticies_buffer = vk_context.upload_staging(
        vertices_span, *self.debug_renderer_verticies_buffer
      );
    } else if (self.debug_renderer_verticies_buffer) {
      self.prepared_frame.debug_renderer_verticies_buffer = vuk::acquire_buf(
        "debug_renderer_verticies_buffer", *self.debug_renderer_verticies_buffer, vuk::Access::eMemoryRead
      );
    }

    DebugRenderer::reset();
  }

  auto gtao_enabled = (bool)RendererCVar::cvar_vbgtao_enable.get();
  if (gtao_enabled && self.viewport_size.x > 0) {
    auto& vbgtao_info = self.vbgtao_info.emplace();
    vbgtao_info.thickness = RendererCVar::cvar_vbgtao_thickness.get();
    vbgtao_info.effect_radius = RendererCVar::cvar_vbgtao_radius.get();

    switch (RendererCVar::cvar_vbgtao_quality_level.get()) {
      case 0: {
        vbgtao_info.slice_count = 1;
        vbgtao_info.samples_per_slice_side = 2;
        break;
      }
      case 1: {
        vbgtao_info.slice_count = 2;
        vbgtao_info.samples_per_slice_side = 2;
        break;
      }
      case 2: {
        vbgtao_info.slice_count = 3;
        vbgtao_info.samples_per_slice_side = 3;
        break;
      }
      case 3: {
        vbgtao_info.slice_count = 9;
        vbgtao_info.samples_per_slice_side = 3;
        break;
      }
    }

    // vbgtao_info.noise_index = (RendererCVar::cvar_gtao_denoise_passes.get() > 0) ? (frameCounter % 64) : (0); //
    // TODO: If we have TAA
    vbgtao_info.noise_index = 0;
    vbgtao_info.final_power = RendererCVar::cvar_vbgtao_final_power.get();
  }
}
} // namespace ox
