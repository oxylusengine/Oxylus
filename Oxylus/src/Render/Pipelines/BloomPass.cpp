#include "Render/RendererPipeline.hpp"
#include "Render/Utils/VukCommon.hpp"

namespace ox {
auto bloom_pass(
  f32 bloom_threshold,
  f32 bloom_clamp,
  vuk::Value<vuk::ImageAttachment>& final_attachment,
  vuk::Value<vuk::ImageAttachment>& bloom_down_image,
  vuk::Value<vuk::ImageAttachment>& bloom_up_image
) -> void {
  auto bloom_prefilter_pass = vuk::make_pass(
    "bloom prefilter",
    [bloom_threshold,
     bloom_clamp](vuk::CommandBuffer& cmd_list, VUK_IA(vuk::eComputeSampled) src, VUK_IA(vuk::eComputeRW) out) {
      cmd_list //
        .bind_compute_pipeline("bloom_prefilter_pipeline")
        .bind_image(0, 0, out)
        .bind_image(0, 1, src)
        .bind_sampler(0, 2, vuk::NearestMagLinearMinSamplerClamped)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(bloom_threshold, bloom_clamp, src->extent))
        .dispatch_invocations_per_pixel(src);

      return std::make_tuple(src, out);
    }
  );

  std::tie(final_attachment, bloom_down_image) = bloom_prefilter_pass(
    std::move(final_attachment),
    std::move(bloom_down_image)
  );

  auto bloom_downsample_pass = vuk::make_pass(
    "bloom downsample",
    [](vuk::CommandBuffer& cmd_list, VUK_IA(vuk::eComputeRW) bloom) {
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

        cmd_list.image_barrier(prev_mip, vuk::eComputeWrite, vuk::eComputeSampled)
          .bind_image(0, 0, mip)
          .bind_image(0, 1, prev_mip)
          .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(mip_width, mip_height))
          .dispatch_invocations(mip_width, mip_height);
      }

      cmd_list.image_barrier(bloom, vuk::eComputeSampled, vuk::eComputeRW);

      return bloom;
    }
  );

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
} // namespace ox
