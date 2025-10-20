#include <vuk/runtime/CommandBuffer.hpp>

#include "Render/RendererInstance.hpp"
#include "Render/Utils/VukCommon.hpp"

namespace ox {
auto RendererInstance::apply_eye_adaptation(this RendererInstance& self, PostProcessContext& context) -> void {
  ZoneScoped;

  auto histogram_buffer = self.renderer.vk_context->alloc_transient_buffer(
    vuk::MemoryUsage::eGPUonly,
    GPU::HISTOGRAM_BIN_COUNT * sizeof(u32)
  );

  auto histogram_generate_pass = vuk::make_pass(
    "histogram generate",
    [settings = self.eye_adaptation](
      vuk::CommandBuffer& cmd_list,
      VUK_IA(vuk::eComputeSampled) src,
      VUK_BA(vuk::eComputeRW | vuk::eTransferWrite) histogram
    ) {
      cmd_list
          .fill_buffer(histogram, 0_u32)
          .bind_compute_pipeline("histogram_generate")
          .bind_image(0, 0, src)
          .push_constants(vuk::ShaderStageFlagBits::eCompute,
            0,
            PushConstants( //
              histogram->device_address,
              glm::uvec2(src->extent.width, src->extent.height),
              settings.min_exposure,
              1.0f / (settings.max_exposure - settings.min_exposure)))
          .dispatch_invocations_per_pixel(src);

      return std::make_tuple(src, histogram);
    }
  );

  std::tie(context.final_attachment, histogram_buffer) = histogram_generate_pass(
    std::move(context.final_attachment),
    std::move(histogram_buffer)
  );

  auto pixel_count = f32(context.final_attachment->extent.width * context.final_attachment->extent.height);
  auto histogram_average_pass = vuk::make_pass(
    "histogram average",
    [pixel_count, delta_time = context.delta_time, settings = self.eye_adaptation](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eComputeRW) histogram,
      VUK_BA(vuk::eComputeRW) exposure
    ) {
      cmd_list //
        .bind_compute_pipeline("histogram_average")
        .push_constants(
          vuk::ShaderStageFlagBits::eCompute,
          0,
          PushConstants(
            histogram->device_address,
            exposure->device_address,
            pixel_count,
            settings.min_exposure,
            settings.max_exposure - settings.min_exposure,
            1.0f - glm::exp(-settings.adaptation_speed * delta_time),
            settings.ev100_bias
          )
        )
        .dispatch(1);

      return exposure;
    }
  );

  self.prepared_frame.exposure_buffer = histogram_average_pass(
    std::move(histogram_buffer),
    std::move(self.prepared_frame.exposure_buffer)
  );
}

auto RendererInstance::apply_bloom(
  this RendererInstance&, PostProcessContext& context, f32 threshold, f32 clamp, u32 mip_count
) -> void {
  ZoneScoped;

  auto bloom_downsampled_attachment = vuk::declare_ia(
    "bloom downsampled",
    {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage,
     .format = vuk::Format::eB10G11R11UfloatPack32,
     .sample_count = vuk::SampleCountFlagBits::e1,
     .level_count = mip_count,
     .layer_count = 1}
  );
  bloom_downsampled_attachment.same_extent_as(context.bloom_upsampled_attachment);
  bloom_downsampled_attachment = vuk::clear_image(std::move(bloom_downsampled_attachment), vuk::Black<float>);

  auto bloom_prefilter_pass = vuk::make_pass(
    "bloom prefilter",
    [threshold, clamp](
      vuk::CommandBuffer& cmd_list, //
      VUK_IA(vuk::eComputeSampled) src,
      VUK_IA(vuk::eComputeRW) out
    ) {
      cmd_list //
        .bind_compute_pipeline("bloom_prefilter")
        .bind_image(0, 0, out)
        .bind_image(0, 1, src)
        .bind_sampler(0, 2, vuk::NearestMagLinearMinSamplerClamped)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(threshold, clamp, src->extent))
        .dispatch_invocations_per_pixel(src);

      return std::make_tuple(src, out);
    }
  );

  std::tie(context.final_attachment, bloom_downsampled_attachment) = bloom_prefilter_pass(
    std::move(context.final_attachment),
    std::move(bloom_downsampled_attachment)
  );

  auto bloom_downsample_pass = vuk::make_pass(
    "bloom downsample",
    [](vuk::CommandBuffer& cmd_list, VUK_IA(vuk::eComputeRW) bloom) {
      cmd_list //
        .bind_compute_pipeline("bloom_downsample")
        .bind_sampler(0, 2, vuk::LinearMipmapNearestSamplerClamped);

      auto extent = bloom->extent;
      for (auto i = 1_u32; i < bloom->level_count; i++) {
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

  bloom_downsampled_attachment = bloom_downsample_pass(std::move(bloom_downsampled_attachment));

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

      cmd_list //
        .bind_compute_pipeline("bloom_upsample")
        .bind_image(0, 1, bloom_downsampled->mip(bloom_upsampled->level_count - 1))
        .bind_sampler(0, 3, vuk::NearestMagLinearMinSamplerClamped);

      for (int32_t i = (int32_t)bloom_upsampled->level_count - 2; i >= 0; i--) {
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

  context.bloom_upsampled_attachment = bloom_upsample_pass(
    context.bloom_upsampled_attachment,
    bloom_downsampled_attachment
  );
}

auto RendererInstance::apply_tonemap(this RendererInstance& self, PostProcessContext& context, vuk::Format format)
  -> vuk::Value<vuk::ImageAttachment> {
  ZoneScoped;

  auto result_attachment = vuk::declare_ia(
    "result",
    {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
     .format = format,
     .sample_count = vuk::Samples::e1}
  );
  result_attachment.same_shape_as(context.final_attachment);
  result_attachment = vuk::clear_image(std::move(result_attachment), vuk::Black<f32>);

  auto tonemap_pass = vuk::make_pass(
    "tonemap",
    [scene_flags = self.gpu_scene_flags, pp = self.post_proces_settings](
      vuk::CommandBuffer& cmd_list,
      VUK_IA(vuk::eColorWrite) dst,
      VUK_IA(vuk::eFragmentSampled) src,
      VUK_IA(vuk::eFragmentSampled) bloom_src,
      VUK_BA(vuk::eFragmentUniformRead) exposure
    ) {
      const auto size = glm::ivec2(src->extent.width, src->extent.height);
      cmd_list.bind_graphics_pipeline("tonemap")
        .set_rasterization({})
        .set_color_blend(dst, vuk::BlendPreset::eOff)
        .set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
        .set_viewport(0, vuk::Rect2D::framebuffer())
        .set_scissor(0, vuk::Rect2D::framebuffer())
        .bind_sampler(0, 0, {.magFilter = vuk::Filter::eLinear, .minFilter = vuk::Filter::eLinear})
        .bind_image(0, 1, src)
        .bind_image(0, 2, bloom_src)
        .specialize_constants(0, std::to_underlying(scene_flags))
        .push_constants(vuk::ShaderStageFlagBits::eFragment, 0, PushConstants(exposure->device_address, pp, size))
        .draw(3, 1, 0, 0);

      return dst;
    }
  );

  return tonemap_pass(
    std::move(result_attachment),
    std::move(context.final_attachment),
    std::move(context.bloom_upsampled_attachment),
    std::move(self.prepared_frame.exposure_buffer)
  );
}
} // namespace ox
