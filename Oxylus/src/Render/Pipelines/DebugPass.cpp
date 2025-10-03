#include "Render/RendererPipeline.hpp"
#include "Render/Utils/VukCommon.hpp"

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

auto debug_pass(
  GPU::DebugView debug_view,
  f32 debug_heatmap_scale,
  vuk::Value<vuk::ImageAttachment>& debug_attachment,
  vuk::Value<vuk::ImageAttachment>& visbuffer_attachment,
  vuk::Value<vuk::ImageAttachment>& depth_attachment,
  vuk::Value<vuk::ImageAttachment>& overdraw_attachment,
  vuk::Value<vuk::ImageAttachment>& albedo_attachment,
  vuk::Value<vuk::ImageAttachment>& normal_attachment,
  vuk::Value<vuk::ImageAttachment>& emissive_attachment,
  vuk::Value<vuk::ImageAttachment>& metallic_roughness_occlusion_attachment,
  vuk::Value<vuk::ImageAttachment>& hiz_attachment,
  vuk::Value<vuk::ImageAttachment>& vbgtao_noisy_occlusion_attachment,
  vuk::Value<vuk::Buffer>& camera_buffer,
  vuk::Value<vuk::Buffer>& visible_meshlet_instances_indices_buffer,
  vuk::Value<vuk::Buffer>& meshlet_instances_buffer,
  vuk::Value<vuk::Buffer>& meshes_buffer,
  vuk::Value<vuk::Buffer>& transforms_buffer
) -> void {
  auto debug_pass = vuk::make_pass(
    "debug pass",
    [debug_view, debug_heatmap_scale](
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
      VUK_BA(vuk::eFragmentRead) transforms_
    ) {
      cmd_list //
        .bind_graphics_pipeline("debug")
        .set_rasterization({})
        .set_color_blend(dst, vuk::BlendPreset::eOff)
        .set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
        .set_viewport(0, vuk::Rect2D::framebuffer())
        .set_scissor(0, vuk::Rect2D::framebuffer())
        .bind_sampler(0, 0, vuk::LinearSamplerRepeated)
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
        .push_constants(
          vuk::ShaderStageFlagBits::eFragment,
          0,
          PushConstants(std::to_underlying(debug_view), debug_heatmap_scale)
        )
        .draw(3, 1, 0, 0);

      return std::make_tuple(dst, depth, camera, gtao);
    }
  );

  std::tie(debug_attachment, depth_attachment, camera_buffer, vbgtao_noisy_occlusion_attachment) = debug_pass(
    std::move(debug_attachment),
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
    std::move(transforms_buffer)
  );
}
} // namespace ox
