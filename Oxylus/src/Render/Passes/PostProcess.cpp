#include <vir/RenderGraph.hpp>
#include <vir/Types.hpp>

#include "Asset/Texture.hpp"
#include "Render/RenderContext.hpp"
#include "Render/RendererInstance.hpp"
#include "Render/Utils/PushConstants.hpp"
#include "Render/Utils/Samplers.hpp"

namespace ox {
using HistogramGeneratePush = PushConstants<glm::uvec2, f32, f32>;
using HistogramAveragePush = PushConstants<f32, f32, f32, f32, f32>;
using BloomPrefilterPush = PushConstants<f32, f32, f32, vir::Extent3D>;
using TonemapPush = PushConstants<GPU::PostProcessSettings, glm::ivec2, f32>;

constexpr static auto TRANSIENT_BUFFER_USAGE = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

auto RendererInstance::bind_eye_adaptation_variables(
  this const RendererInstance& self, vir::Program& program, const PostProcessVariables& variables
) -> void {
  ZoneScoped;
  if (!program.has_variable(variables.histogram_average_push)) {
    return;
  }

  const auto& shape = self.frame_params.shape;
  const auto display_extent = vir::Extent3D{shape.display_size.x, shape.display_size.y, 1};
  const auto& settings = self.eye_adaptation;
  program.set_bytes(
    variables.histogram_generate_push,
    HistogramGeneratePush(
      glm::uvec2(display_extent.width, display_extent.height),
      settings.min_exposure,
      settings.max_exposure
    )
  );

  const auto pixel_count = static_cast<f32>(display_extent.width * display_extent.height);
  program.set_bytes(
    variables.histogram_average_push,
    HistogramAveragePush(
      pixel_count,
      settings.min_exposure,
      settings.max_exposure - settings.min_exposure,
      1.0f - glm::exp(-settings.adaptation_speed * self.frame_params.delta_time),
      settings.ev100_bias
    )
  );
}

auto RendererInstance::apply_eye_adaptation(
  vir::Module& module, RenderContext& render_context, FrameRecording& recording, PostProcessContext& context
) -> void {
  ZoneScoped;
  auto& prepared = recording.prepared;
  auto& variables = prepared.post_process;
  variables.histogram_generate_push = module.declare_bytes_var(
    "histogram generate push",
    sizeof(HistogramGeneratePush)
  );
  variables.histogram_average_push = module.declare_bytes_var("histogram average push", sizeof(HistogramAveragePush));

  auto& values = recording.values;

  auto histogram = module.transient_buffer({
    .size = GPU::HISTOGRAM_BIN_COUNT * sizeof(u32),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "histogram bins",
  });
  histogram = module.fill_buffer(histogram, 0);

  std::tie(context.final_attachment, histogram) = std::tuple_cat(
    module.begin_compute({{context.final_attachment, vir::Access::ComputeSampled}, {histogram, vir::Access::ComputeRW}})
      .with_name("histogram generate")
      .bind_compute_pipeline(render_context.pipeline("histogram_generate"))
      .bind_image(0, 0, context.final_attachment)
      .push_constant_address(0, histogram)
      .push_constants_from_at(8, variables.histogram_generate_push)
      .dispatch_invocations_per_pixel(context.final_attachment)
      .end_compute()
  );

  std::tie(std::ignore, values.exposure) = std::tuple_cat(
    module.begin_compute({{histogram, vir::Access::ComputeRW}, {values.exposure, vir::Access::ComputeRW}})
      .with_name("histogram average")
      .bind_compute_pipeline(render_context.pipeline("histogram_average"))
      .bind_buffer(0, 0, histogram)
      .bind_buffer(0, 1, values.exposure)
      .push_constants_from(variables.histogram_average_push)
      .dispatch(1, 1, 1)
      .end_compute()
  );
}

auto RendererInstance::bind_bloom_variables(
  this const RendererInstance& self, vir::Program& program, const PostProcessVariables& variables
) -> void {
  ZoneScoped;
  if (!program.has_variable(variables.bloom_prefilter_push)) {
    return;
  }

  const auto& shape = self.frame_params.shape;
  const auto display_extent = vir::Extent3D{shape.display_size.x, shape.display_size.y, 1};
  const auto& cvar = *self.frame_params.cvar;
  const auto bloom_extent = vir::Extent3D{
    .width = std::max(display_extent.width / 2, 1u),
    .height = std::max(display_extent.height / 2, 1u),
    .depth = 1,
  };
  program.set_bytes(
    variables.bloom_prefilter_push,
    BloomPrefilterPush(
      cvar.cvar_bloom_threshold.get(),
      cvar.cvar_bloom_soft_threshold.get(),
      cvar.cvar_bloom_clamp.get(),
      bloom_extent
    )
  );
  program.set_bytes(variables.bloom_radius, cvar.cvar_bloom_radius.get());
}

auto RendererInstance::apply_bloom(
  vir::Module& module, RenderContext& render_context, FrameRecording& recording, PostProcessContext& context
) -> void {
  ZoneScoped;
  auto& prepared = recording.prepared;
  auto& variables = prepared.post_process;
  variables.bloom_prefilter_push = module.declare_bytes_var("bloom prefilter push", sizeof(BloomPrefilterPush));
  variables.bloom_radius = module.declare_bytes_var("bloom radius", sizeof(f32));

  auto& values = recording.values;
  const auto& shape = recording.shape;

  const auto linear_border = render_context.cached_sampler_handle(LinearSamplerBorder);
  const auto extent = module.image_extent(context.bloom_upsampled_attachment).value_or(vir::Extent3D{1, 1, 1});
  const auto level_count = Texture::calculate_mip_count(extent);
  const auto format = module.image_format(context.bloom_upsampled_attachment)
                        .value_or(std::pair{vir::Format::B10G11R11UfloatPack32, vir::Samples::e1})
                        .first;

  auto downsampled = module.transient_image({
    .extent = extent,
    .format = format,
    .mip_levels = level_count,
    .name = "bloom downsampled",
  });
  downsampled = module.clear(downsampled, vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 1.0f));

  const auto prefiltered = module.mip(downsampled, 0);
  auto prefilter_results = //
    module
      .begin_compute(
        {{context.final_attachment, vir::Access::ComputeSampled},
         {prefiltered, vir::Access::ComputeWrite},
         {values.exposure, vir::Access::ComputeUniformRead}}
      )
      .with_name("bloom prefilter")
      .bind_compute_pipeline(render_context.pipeline("bloom_prefilter"))
      .bind_image(0, 0, prefiltered)
      .bind_image(0, 1, context.final_attachment)
      .bind_sampler(0, 2, linear_border)
      .bind_buffer(0, 3, values.exposure)
      .specialize_constant(0, std::to_underlying(shape.scene_flags))
      .push_constants_from(variables.bloom_prefilter_push)
      .dispatch_invocations(extent.width, extent.height, 1)
      .end_compute();
  context.final_attachment = prefilter_results[0];
  downsampled = module.whole(prefilter_results[1]);
  values.exposure = prefilter_results[2];

  if (level_count == 1) {
    context.bloom_upsampled_attachment = downsampled;
    return;
  }

  for (auto i = 1_u32; i < level_count; i++) {
    const auto mip_width = std::max(1_u32, extent.width >> i);
    const auto mip_height = std::max(1_u32, extent.height >> i);
    const auto src = module.mip(downsampled, i - 1);
    const auto dst = module.mip(downsampled, i);
    downsampled = module.whole( //
      module.begin_compute({{src, vir::Access::ComputeSampled}, {dst, vir::Access::ComputeWrite}})
        .with_name("bloom downsample")
        .bind_compute_pipeline(render_context.pipeline("bloom_downsample"))
        .bind_image(0, 0, dst)
        .bind_image(0, 1, src)
        .bind_sampler(0, 2, linear_border)
        .push_constants(PushConstants(mip_width, mip_height))
        .dispatch_invocations(mip_width, mip_height, 1)
        .end_compute()[1]
    );
  }

  // https://www.froyok.fr/blog/2021-12-ue4-custom-bloom/resources/code/bloom_down_up_demo.jpg
  const auto linear_clamped = render_context.cached_sampler_handle(LinearSamplerClamped);
  auto upsampled = context.bloom_upsampled_attachment;
  const auto last_mip = level_count - 1;
  for (auto i = last_mip; i > 0; i--) {
    const auto mip_width = std::max(1_u32, extent.width >> (i - 1));
    const auto mip_height = std::max(1_u32, extent.height >> (i - 1));
    const auto upsample_src = i == last_mip ? module.mip(downsampled, last_mip) : module.mip(upsampled, i);
    const auto downsample_src = module.mip(downsampled, i - 1);
    const auto dst = module.mip(upsampled, i - 1);
    const auto results = //
      module
        .begin_compute(
          {{upsample_src, vir::Access::ComputeSampled},
           {downsample_src, vir::Access::ComputeSampled},
           {dst, vir::Access::ComputeWrite}}
        )
        .with_name("bloom upsample")
        .bind_compute_pipeline(render_context.pipeline("bloom_upsample"))
        .bind_image(0, 0, dst)
        .bind_image(0, 1, upsample_src)
        .bind_image(0, 2, downsample_src)
        .bind_sampler(0, 3, linear_clamped)
        .push_constants(PushConstants(mip_width, mip_height))
        .push_constants_from_at(8, variables.bloom_radius)
        .dispatch_invocations(mip_width, mip_height, 1)
        .end_compute();
    if (i == last_mip) {
      downsampled = module.whole(results[0]);
    }

    upsampled = module.whole(results[2]);
  }

  context.bloom_upsampled_attachment = upsampled;
}

auto RendererInstance::bind_tonemap_variables(
  this const RendererInstance& self, vir::Program& program, const PostProcessVariables& variables
) -> void {
  ZoneScoped;
  if (!program.has_variable(variables.tonemap_push)) {
    return;
  }

  const auto& shape = self.frame_params.shape;
  const auto display_extent = vir::Extent3D{shape.display_size.x, shape.display_size.y, 1};
  program.set_bytes(
    variables.tonemap_push,
    TonemapPush(
      self.post_proces_settings,
      glm::ivec2(display_extent.width, display_extent.height),
      self.frame_params.bloom_intensity
    )
  );
}

auto RendererInstance::apply_tonemap(
  vir::Module& module, RenderContext& render_context, FrameRecording& recording, PostProcessContext& context
) -> vir::ValueID {
  ZoneScoped;
  auto& prepared = recording.prepared;
  auto& variables = prepared.post_process;
  variables.tonemap_push = module.declare_bytes_var("tonemap push", sizeof(TonemapPush));

  auto& values = recording.values;
  const auto& shape = recording.shape;

  static constexpr auto tonemap_sampler = vir::SamplerInfo{
    .mag_filter = VK_FILTER_LINEAR,
    .min_filter = VK_FILTER_LINEAR,
    .mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
    .address_mode_u = VK_SAMPLER_ADDRESS_MODE_REPEAT,
    .address_mode_v = VK_SAMPLER_ADDRESS_MODE_REPEAT,
    .address_mode_w = VK_SAMPLER_ADDRESS_MODE_REPEAT,
  };
  std::tie(
    context.dst_attachment,
    context.final_attachment,
    context.bloom_upsampled_attachment,
    values.exposure
  ) = std::tuple_cat( //
    module
      .begin_rendering(
        {{context.dst_attachment, vir::Access::ColorWrite},
         {context.final_attachment, vir::Access::FragmentSampled},
         {context.bloom_upsampled_attachment, vir::Access::FragmentSampled},
         {values.exposure, vir::Access::FragmentUniformRead}}
      )
      .with_name("tonemap")
      .bind_graphics_pipeline(render_context.pipeline("tonemap"))
      .set_viewport(0, vir::Rect2D::framebuffer())
      .set_scissor(0, vir::Rect2D::framebuffer())
      .bind_sampler(0, 0, render_context.cached_sampler_handle(tonemap_sampler))
      .bind_image(0, 1, context.final_attachment)
      .bind_image(0, 2, context.bloom_upsampled_attachment)
      .bind_buffer(0, 3, values.exposure)
      .specialize_constant(0, std::to_underlying(shape.scene_flags))
      .specialize_constant(1, std::to_underlying(shape.tonemap))
      .push_constants_from(prepared.post_process.tonemap_push)
      .draw(3, 1)
      .end_rendering()
  );

  return context.dst_attachment;
}
} // namespace ox
