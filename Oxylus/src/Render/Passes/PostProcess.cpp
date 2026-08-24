#include <cstring>
#include <vuk/runtime/CommandBuffer.hpp>
#include <vuk/vsl/Core.hpp>

#include "Render/RendererInstance.hpp"
#include "Render/Utils/VukCommon.hpp"

namespace ox {
auto RendererInstance::apply_eye_adaptation(this RendererInstance& self, PostProcessContext& context) -> void {
  ZoneScoped;

  constexpr auto histogram_size_bytes = GPU::HISTOGRAM_BIN_COUNT * sizeof(u32);
  auto histogram_bin_indices_buffer = self.renderer.render_context->alloc_transient_buffer(
    vuk::MemoryUsage::eGPUonly,
    histogram_size_bytes
  );
  auto histogram_clear_buffer = self.renderer.render_context->alloc_transient_buffer(
    vuk::MemoryUsage::eCPUtoGPU,
    histogram_size_bytes
  );
  std::memset(histogram_clear_buffer->mapped_ptr, 0, histogram_size_bytes);
  histogram_bin_indices_buffer = vuk::copy(std::move(histogram_clear_buffer), std::move(histogram_bin_indices_buffer));

  auto histogram_generate_pass = vuk::make_pass(
    "histogram generate",
    [settings = self.eye_adaptation](
      vuk::CommandBuffer& cmd_list,
      VUK_IA(vuk::eComputeSampled) src,
      VUK_BA(vuk::eComputeRW) histogram_bin_indices
    ) {
      cmd_list
          .bind_compute_pipeline("histogram_generate")
          .bind_image(0, 0, src)
          .push_constants(vuk::ShaderStageFlagBits::eCompute,
            0,
            PushConstants( //
              histogram_bin_indices->device_address,
              glm::uvec2(src->extent.width, src->extent.height),
              settings.min_exposure,
              settings.max_exposure))
          .dispatch_invocations_per_pixel(src);

      return std::make_tuple(src, histogram_bin_indices);
    }
  );

  std::tie(context.final_attachment, histogram_bin_indices_buffer) = histogram_generate_pass(
    std::move(context.final_attachment),
    std::move(histogram_bin_indices_buffer)
  );

  auto pixel_count = f32(context.extent.width * context.extent.height);
  auto histogram_average_pass = vuk::make_pass(
    "histogram average",
    [pixel_count, delta_time = context.delta_time, settings = self.eye_adaptation](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eComputeRW) histogram,
      VUK_BA(vuk::eComputeRW) exposure
    ) {
      cmd_list //
        .bind_compute_pipeline("histogram_average")
        .bind_buffer(0, 0, histogram)
        .bind_buffer(0, 1, exposure)
        .push_constants(
          vuk::ShaderStageFlagBits::eCompute,
          0,
          PushConstants(
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
    std::move(histogram_bin_indices_buffer),
    std::move(self.prepared_frame.exposure_buffer)
  );
}

auto RendererInstance::apply_bloom(this RendererInstance& self, PostProcessContext& context, const RendererCVar& cvar)
  -> void {
  ZoneScoped;

  const auto threshold = cvar.cvar_bloom_threshold.get();
  const auto soft_threshold = cvar.cvar_bloom_soft_threshold.get();
  const auto radius = cvar.cvar_bloom_radius.get();
  const auto clamp_value = cvar.cvar_bloom_clamp.get();
  context.bloom_intensity = cvar.cvar_bloom_intensity.get();

  const auto bloom_extent = vuk::Extent3D{
    .width = std::max(context.final_attachment->extent.width / 2, 1u),
    .height = std::max(context.final_attachment->extent.height / 2, 1u),
    .depth = 1,
  };
  auto bloom_attachment = vuk::declare_ia(
    "bloom",
    {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage,
     .extent = bloom_extent,
     .format = vuk::Format::eB10G11R11UfloatPack32,
     .sample_count = vuk::SampleCountFlagBits::e1,
     .level_count = Texture::calculate_mip_count(bloom_extent),
     .layer_count = 1}
  );

  auto bloom_prefilter_pass = vuk::make_pass(
    "bloom prefilter",
    [threshold, soft_threshold, clamp_value, scene_flags = self.gpu_scene_flags](
      vuk::CommandBuffer& cmd_list, //
      VUK_IA(vuk::eComputeSampled) src,
      VUK_IA(vuk::eComputeRW) out,
      VUK_BA(vuk::eComputeUniformRead) exposure
    ) {
      cmd_list //
        .bind_compute_pipeline("bloom_prefilter")
        .bind_image(0, 0, out)
        .bind_image(0, 1, src)
        .bind_sampler(0, 2, vuk::LinearSamplerBorder)
        .bind_buffer(0, 3, exposure)
        .specialize_constants(0, std::to_underlying(scene_flags))
        .push_constants(
          vuk::ShaderStageFlagBits::eCompute,
          0,
          PushConstants(threshold, soft_threshold, clamp_value, out->extent)
        )
        .dispatch_invocations_per_pixel(out);

      return std::make_tuple(src, out, exposure);
    }
  );

  std::tie(context.final_attachment, bloom_attachment, self.prepared_frame.exposure_buffer) = bloom_prefilter_pass(
    std::move(context.final_attachment),
    std::move(bloom_attachment),
    std::move(self.prepared_frame.exposure_buffer)
  );

  auto bloom_downsample_pass = vuk::make_pass(
    "bloom downsample",
    [](vuk::CommandBuffer& cmd_list, VUK_IA(vuk::eComputeRW | vuk::eComputeSampled) bloom) {
      cmd_list //
        .bind_compute_pipeline("bloom_downsample")
        .bind_sampler(0, 2, vuk::LinearSamplerBorder);

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

      return bloom;
    }
  );

  bloom_attachment = bloom_downsample_pass(std::move(bloom_attachment));

  // https://www.froyok.fr/blog/2021-12-ue4-custom-bloom/resources/code/bloom_down_up_demo.jpg

  auto bloom_upsample_pass = vuk::make_pass(
    "bloom_upsample",
    [radius](vuk::CommandBuffer& cmd_list, VUK_IA(vuk::eComputeRW | vuk::eComputeSampled) bloom) {
      auto extent = bloom->extent;
      const auto last_mip = static_cast<i32>(bloom->level_count) - 1;

      cmd_list //
        .bind_compute_pipeline("bloom_upsample")
        .bind_sampler(0, 3, vuk::LinearSamplerClamped);

      for (int32_t i = last_mip; i > 0; i--) {
        auto mip_width = std::max(1_u32, extent.width >> (i - 1));
        auto mip_height = std::max(1_u32, extent.height >> (i - 1));
        auto low_mip = bloom->mip(i);
        auto high_mip = bloom->mip(i - 1);

        if (i == last_mip) {
          cmd_list.image_barrier(low_mip, vuk::eComputeWrite, vuk::eComputeSampled);
        } else {
          cmd_list.image_barrier(low_mip, vuk::eComputeRW, vuk::eComputeSampled);
        }
        cmd_list.image_barrier(high_mip, vuk::eComputeSampled, vuk::eComputeRW)
          .bind_image(0, 0, high_mip)
          .bind_image(0, 1, low_mip);
        cmd_list.push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(mip_width, mip_height, radius));
        cmd_list.dispatch_invocations(mip_width, mip_height);
      }

      return bloom;
    }
  );

  context.bloom_attachment = bloom_upsample_pass(std::move(bloom_attachment));
}

auto RendererInstance::apply_tonemap(this RendererInstance& self, PostProcessContext& context)
  -> vuk::Value<vuk::ImageAttachment> {
  ZoneScoped;

  auto tonemap_pass = vuk::make_pass(
    "tonemap",
    [scene_flags = self.gpu_scene_flags,
     pp = self.post_proces_settings,
     tt = self.tonemap_type,
     bloom_intensity = context.bloom_intensity](
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
        .bind_buffer(0, 3, exposure)
        .specialize_constants(0, std::to_underlying(scene_flags))
        .specialize_constants(1, std::to_underlying(tt))
        .push_constants(vuk::ShaderStageFlagBits::eFragment, 0, PushConstants(pp, size, bloom_intensity))
        .draw(3, 1, 0, 0);

      return dst;
    }
  );

  return tonemap_pass(
    std::move(context.dst_attachment),
    std::move(context.final_attachment),
    std::move(context.bloom_attachment),
    std::move(self.prepared_frame.exposure_buffer)
  );
}
} // namespace ox
