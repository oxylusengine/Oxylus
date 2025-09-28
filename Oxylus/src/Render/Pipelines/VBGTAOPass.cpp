#include "Render/RendererPipeline.hpp"
#include "Render/Utils/VukCommon.hpp"

namespace ox {
auto vbgtao_pass(
  vuk::Value<vuk::ImageAttachment>& depth_attachment,
  vuk::Value<vuk::ImageAttachment>& final_attachment,
  vuk::Value<vuk::ImageAttachment>& normal_attachment,
  vuk::Value<vuk::ImageAttachment>& hilbert_noise_lut_attachment,
  vuk::Value<vuk::ImageAttachment>& vbgtao_noisy_occlusion_attachment,
  vuk::Value<vuk::ImageAttachment>& vbgtao_occlusion_attachment,
  vuk::Value<vuk::Buffer>& camera_buffer,
  GPU::VBGTAOSettings& gtao_settings
) -> void {
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
    std::move(depth_attachment),
    std::move(vbgtao_depth_attachment)
  );

  auto vbgtao_generate_pass = vuk::make_pass( //
        "vbgtao generate",
        [gtao_settings](vuk::CommandBuffer& command_buffer,
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
            .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(gtao_settings))
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
    std::move(vbgtao_depth_differences_attachment),
    vuk::Black<f32>
  );

  vbgtao_noisy_occlusion_attachment = vuk::clear_image(std::move(vbgtao_noisy_occlusion_attachment), vuk::White<f32>);

  std::tie(camera_buffer, normal_attachment, vbgtao_noisy_occlusion_attachment, vbgtao_depth_differences_attachment) =
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
        [gtao_settings](vuk::CommandBuffer& command_buffer,
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
            .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(occlusion_noisy_extent, gtao_settings))
            .dispatch_invocations_per_pixel(ambient_occlusion);

          return std::make_tuple(ambient_occlusion, noisy_occlusion);
        });

  std::tie(vbgtao_occlusion_attachment, vbgtao_noisy_occlusion_attachment) = vbgtao_denoise_pass(
    std::move(vbgtao_noisy_occlusion_attachment),
    std::move(vbgtao_depth_differences_attachment),
    std::move(vbgtao_occlusion_attachment)
  );
}
} // namespace ox
