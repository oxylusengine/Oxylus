#include <vuk/RenderGraph.hpp>
#include <vuk/vsl/Core.hpp>

#include "Core/App.hpp"
#include "Render/RendererInstance.hpp"
#include "Render/Utils/VukCommon.hpp"

namespace ox {
// FSR3 runs the shading change pyramid over three mips; see fsr3_shading_change.slang
constexpr static u32 FSR3_SHADING_CHANGE_MIP_COUNT = 3;

auto RendererInstance::allocate_fsr3_resources(
  this RendererInstance& self, const glm::uvec2 render_size, const glm::uvec2 display_size
) -> void {
  ZoneScoped;

  if (self.fsr3_render_size == render_size && self.fsr3_display_size == display_size) {
    return;
  }

  auto& allocator = *self.renderer.render_context->superframe_allocator;

  auto allocate = [&](FSR3History& target, const vuk::Extent3D extent, const vuk::Format format) {
    target.attachment = vuk::ImageAttachment{
      .usage = vuk::ImageUsageFlagBits::eStorage | vuk::ImageUsageFlagBits::eSampled,
      .extent = extent,
      .format = format,
      .sample_count = vuk::Samples::e1,
      .view_type = vuk::ImageViewType::e2D,
      .base_level = 0,
      .level_count = 1,
      .base_layer = 0,
      .layer_count = 1,
    };
    target.image = *vuk::allocate_image(allocator, target.attachment);
    target.attachment.image = *target.image;
    target.view = *vuk::allocate_image_view(allocator, target.attachment);
    target.attachment.image_view = *target.view;
  };

  const auto render_extent = vuk::Extent3D{render_size.x, render_size.y, 1};
  const auto display_extent = vuk::Extent3D{display_size.x, display_size.y, 1};

  for (auto i = 0_u32; i < 2; i++) {
    allocate(self.fsr3_internal_upscaled_color[i], display_extent, vuk::Format::eR16G16B16A16Sfloat);
    allocate(self.fsr3_accumulation[i], render_extent, vuk::Format::eR8Unorm);
    allocate(self.fsr3_luma[i], render_extent, vuk::Format::eR16Sfloat);
    allocate(self.fsr3_luma_history[i], render_extent, vuk::Format::eR16G16B16A16Sfloat);
  }

  self.fsr3_render_size = render_size;
  self.fsr3_display_size = display_size;
  // the freshly allocated history is undefined, so the next frame treats every pixel as new
  self.fsr3_history_valid = false;
}

auto RendererInstance::apply_fsr3(this RendererInstance& self, FSR3Context& context)
  -> vuk::Value<vuk::ImageAttachment> {
  ZoneScoped;

  const auto& constants = context.constants;
  const auto render_size = glm::uvec2(constants.render_size);
  const auto display_size = glm::uvec2(constants.upscale_size);
  const auto render_extent = vuk::Extent3D{render_size.x, render_size.y, 1};
  const auto half_extent = vuk::Extent3D{std::max(render_size.x / 2, 1u), std::max(render_size.y / 2, 1u), 1};
  const auto display_extent = vuk::Extent3D{display_size.x, display_size.y, 1};

  auto constants_buffer = self.renderer.render_context->scratch_buffer(constants);

  const auto current = self.fsr3_history_ping ? 1_u32 : 0_u32;
  const auto previous = self.fsr3_history_ping ? 0_u32 : 1_u32;

  auto declare_render_ia = [&](const char* name, const vuk::Format format) {
    auto ia = vuk::declare_ia(
      name,
      {.usage = vuk::ImageUsageFlagBits::eStorage | vuk::ImageUsageFlagBits::eSampled,
       .extent = render_extent,
       .format = format,
       .sample_count = vuk::Samples::e1,
       .level_count = 1,
       .layer_count = 1}
    );
    return ia;
  };

  auto dilated_motion_vectors = declare_render_ia("fsr3 dilated motion vectors", vuk::Format::eR16G16Sfloat);
  auto dilated_depth = declare_render_ia("fsr3 dilated depth", vuk::Format::eR32Sfloat);
  auto farthest_depth = declare_render_ia("fsr3 farthest depth", vuk::Format::eR16Sfloat);
  auto dilated_reactive_masks = declare_render_ia("fsr3 dilated reactive masks", vuk::Format::eR8G8B8A8Unorm);

  // the depth scatter in prepare_inputs is an InterlockedMax, so it has to start from the reversed-z
  // "nothing here" value of 0
  auto reconstructed_prev_nearest_depth = declare_render_ia("fsr3 reconstructed prev depth", vuk::Format::eR32Uint);
  reconstructed_prev_nearest_depth = vuk::clear_image(std::move(reconstructed_prev_nearest_depth), vuk::Black<u32>);

  // sprites and particles author this while compositing; there is no separate transparency and
  // composition mask yet, so that one stays cleared
  auto input_reactive_mask = std::move(context.reactive_mask_attachment);
  auto input_transparency_mask = declare_render_ia("fsr3 input transparency mask", vuk::Format::eR8Unorm);
  input_transparency_mask = vuk::clear_image(std::move(input_transparency_mask), vuk::Black<f32>);

  auto farthest_depth_mip1 = vuk::declare_ia(
    "fsr3 farthest depth mip1",
    {.usage = vuk::ImageUsageFlagBits::eStorage | vuk::ImageUsageFlagBits::eSampled,
     .extent = half_extent,
     .format = vuk::Format::eR16Sfloat,
     .sample_count = vuk::Samples::e1,
     .level_count = 1,
     .layer_count = 1}
  );

  auto shading_change = vuk::declare_ia(
    "fsr3 shading change",
    {.usage = vuk::ImageUsageFlagBits::eStorage | vuk::ImageUsageFlagBits::eSampled,
     .extent = half_extent,
     .format = vuk::Format::eR8Unorm,
     .sample_count = vuk::Samples::e1,
     .level_count = 1,
     .layer_count = 1}
  );

  auto spd_mips = vuk::declare_ia(
    "fsr3 spd mips",
    {.usage = vuk::ImageUsageFlagBits::eStorage | vuk::ImageUsageFlagBits::eSampled,
     .extent = half_extent,
     .format = vuk::Format::eR16G16Sfloat,
     .sample_count = vuk::Samples::e1,
     .level_count = FSR3_SHADING_CHANGE_MIP_COUNT,
     .layer_count = 1}
  );

  // scattered writes from prepare_reactivity only touch thin feature pixels, so the rest has to be
  // cleared rather than left over from last frame
  auto new_locks = vuk::declare_ia(
    "fsr3 new locks",
    {.usage = vuk::ImageUsageFlagBits::eStorage,
     .extent = display_extent,
     .format = vuk::Format::eR8Unorm,
     .sample_count = vuk::Samples::e1,
     .level_count = 1,
     .layer_count = 1}
  );
  new_locks = vuk::clear_image(std::move(new_locks), vuk::Black<f32>);

  auto upscaled_output = vuk::declare_ia(
    "fsr3 upscaled output",
    {.usage = vuk::ImageUsageFlagBits::eStorage | vuk::ImageUsageFlagBits::eSampled |
              vuk::ImageUsageFlagBits::eColorAttachment,
     .extent = display_extent,
     .format = vuk::Format::eR16G16B16A16Sfloat,
     .sample_count = vuk::Samples::e1,
     .level_count = 1,
     .layer_count = 1}
  );

  // every one of these is fully overwritten by the pass that produces it, so discard rather than
  // acquire: there is nothing worth preserving from two frames ago
  auto internal_upscaled_color = vuk::discard_ia(
    "fsr3 internal upscaled color",
    self.fsr3_internal_upscaled_color[current].attachment
  );
  auto accumulation = vuk::discard_ia("fsr3 accumulation", self.fsr3_accumulation[current].attachment);
  auto current_luma = vuk::discard_ia("fsr3 current luma", self.fsr3_luma[current].attachment);
  auto luma_history = vuk::discard_ia("fsr3 luma history", self.fsr3_luma_history[current].attachment);

  // on a reset there is no meaningful previous frame; clear the far side of the ping-pong and let
  // the frame_index == 0 path in the shaders reject it anyway
  auto acquire_or_clear = [&](FSR3History& target, const char* name) {
    if (context.reset) {
      return vuk::clear_image(vuk::discard_ia(name, target.attachment), vuk::Black<f32>);
    }
    return vuk::acquire_ia(name, target.attachment, vuk::eComputeSampled);
  };

  auto internal_upscaled_color_prev = acquire_or_clear(self.fsr3_internal_upscaled_color[previous], "fsr3 prev color");
  auto accumulation_prev = acquire_or_clear(self.fsr3_accumulation[previous], "fsr3 prev accumulation");
  auto previous_luma = acquire_or_clear(self.fsr3_luma[previous], "fsr3 prev luma");
  auto luma_history_prev = acquire_or_clear(self.fsr3_luma_history[previous], "fsr3 prev luma history");

  auto luma_instability = declare_render_ia("fsr3 luma instability", vuk::Format::eR16Sfloat);

  // --- prepare inputs: dilate depth and motion, scatter reconstructed depth, extract luma ---
  auto prepare_inputs_pass = vuk::make_pass(
    "fsr3 prepare inputs",
    [](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eComputeRead) fsr3_constants,
      VUK_IA(vuk::eComputeSampled) motion_vectors,
      VUK_IA(vuk::eComputeSampled) depth,
      VUK_IA(vuk::eComputeSampled) color,
      VUK_IA(vuk::eComputeRW) out_dilated_motion_vectors,
      VUK_IA(vuk::eComputeRW) out_dilated_depth,
      VUK_IA(vuk::eComputeRW) out_reconstructed_depth,
      VUK_IA(vuk::eComputeRW) out_farthest_depth,
      VUK_IA(vuk::eComputeRW) out_current_luma
    ) {
      cmd_list.bind_compute_pipeline("fsr3_prepare_inputs")
        .bind_buffer(0, 0, fsr3_constants)
        .bind_image(0, 1, motion_vectors)
        .bind_image(0, 2, depth)
        .bind_image(0, 3, color)
        .bind_image(0, 4, out_dilated_motion_vectors)
        .bind_image(0, 5, out_dilated_depth)
        .bind_image(0, 6, out_reconstructed_depth)
        .bind_image(0, 7, out_farthest_depth)
        .bind_image(0, 8, out_current_luma)
        .dispatch_invocations_per_pixel(out_dilated_depth);

      return std::make_tuple(
        fsr3_constants,
        motion_vectors,
        depth,
        color,
        out_dilated_motion_vectors,
        out_dilated_depth,
        out_reconstructed_depth,
        out_farthest_depth,
        out_current_luma
      );
    }
  );

  std::tie(
    constants_buffer,
    context.velocity_attachment,
    context.depth_attachment,
    context.color_attachment,
    dilated_motion_vectors,
    dilated_depth,
    reconstructed_prev_nearest_depth,
    farthest_depth,
    current_luma
  ) =
    prepare_inputs_pass(
      std::move(constants_buffer),
      std::move(context.velocity_attachment),
      std::move(context.depth_attachment),
      std::move(context.color_attachment),
      std::move(dilated_motion_vectors),
      std::move(dilated_depth),
      std::move(reconstructed_prev_nearest_depth),
      std::move(farthest_depth),
      std::move(current_luma)
    );

  // --- farthest depth mip 1 ---
  auto farthest_depth_mip1_pass = vuk::make_pass(
    "fsr3 farthest depth mip1",
    [](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eComputeRead) fsr3_constants,
      VUK_IA(vuk::eComputeSampled) src,
      VUK_IA(vuk::eComputeRW) dst
    ) {
      cmd_list.bind_compute_pipeline("fsr3_farthest_depth_mip1")
        .bind_buffer(0, 0, fsr3_constants)
        .bind_image(0, 1, src)
        .bind_image(0, 2, dst)
        .dispatch_invocations_per_pixel(dst);

      return std::make_tuple(fsr3_constants, src, dst);
    }
  );

  std::tie(constants_buffer, farthest_depth, farthest_depth_mip1) = farthest_depth_mip1_pass(
    std::move(constants_buffer),
    std::move(farthest_depth),
    std::move(farthest_depth_mip1)
  );

  // --- shading change pyramid: mip 0 from the luma pair, then the reduction chain ---
  // The whole chain lives in one pass with manual barriers between mips, the same shape as the
  // bloom downsample. A pass per mip would leave each mip's write unconsumed and vuk would cull it,
  // leaving the pyramid undefined and the shading change signal reading garbage.
  auto shading_change_pyramid_pass = vuk::make_pass(
    "fsr3 shading change pyramid",
    [](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eComputeRead) fsr3_constants,
      VUK_IA(vuk::eComputeSampled) luma_current,
      VUK_IA(vuk::eComputeSampled) luma_previous,
      VUK_IA(vuk::eComputeSampled) motion_vectors,
      VUK_BA(vuk::eComputeRead) exposure,
      VUK_IA(vuk::eComputeRW) mips
    ) {
      const auto mip0_width = std::max(1_u32, mips->extent.width);
      const auto mip0_height = std::max(1_u32, mips->extent.height);

      cmd_list.bind_compute_pipeline("fsr3_shading_change_pyramid")
        .bind_buffer(0, 0, fsr3_constants)
        .bind_image(0, 1, luma_current)
        .bind_image(0, 2, luma_previous)
        .bind_image(0, 3, motion_vectors)
        .bind_buffer(0, 4, exposure)
        .bind_image(0, 5, mips->mip(0))
        .dispatch_invocations(mip0_width, mip0_height);

      cmd_list.bind_compute_pipeline("fsr3_downsample");
      for (auto mip = 1_u32; mip < mips->level_count; mip++) {
        const auto dst_size = glm::ivec2(std::max(1_u32, mip0_width >> mip), std::max(1_u32, mip0_height >> mip));
        const auto src_size = glm::ivec2(
          std::max(1_u32, mip0_width >> (mip - 1)),
          std::max(1_u32, mip0_height >> (mip - 1))
        );

        cmd_list.image_barrier(mips->mip(mip - 1), vuk::eComputeWrite, vuk::eComputeSampled)
          .bind_image(0, 0, mips->mip(mip - 1))
          .bind_image(0, 1, mips->mip(mip))
          .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(dst_size, src_size))
          .dispatch_invocations(static_cast<u32>(dst_size.x), static_cast<u32>(dst_size.y));
      }

      cmd_list.image_barrier(mips, vuk::eComputeSampled, vuk::eComputeRW);

      return std::make_tuple(fsr3_constants, luma_current, luma_previous, motion_vectors, exposure, mips);
    }
  );

  std::tie(constants_buffer, current_luma, previous_luma, dilated_motion_vectors, context.exposure_buffer, spd_mips) =
    shading_change_pyramid_pass(
      std::move(constants_buffer),
      std::move(current_luma),
      std::move(previous_luma),
      std::move(dilated_motion_vectors),
      std::move(context.exposure_buffer),
      std::move(spd_mips)
    );

  // --- shading change ---
  auto shading_change_pass = vuk::make_pass(
    "fsr3 shading change",
    [](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eComputeRead) fsr3_constants,
      VUK_IA(vuk::eComputeSampled) mips,
      VUK_IA(vuk::eComputeRW) dst
    ) {
      cmd_list.bind_compute_pipeline("fsr3_shading_change")
        .bind_buffer(0, 0, fsr3_constants)
        .bind_image(0, 1, mips)
        .bind_sampler(0, 2, vuk::LinearSamplerClamped)
        .bind_image(0, 3, dst)
        .dispatch_invocations_per_pixel(dst);

      return std::make_tuple(fsr3_constants, mips, dst);
    }
  );

  std::tie(constants_buffer, spd_mips, shading_change) = shading_change_pass(
    std::move(constants_buffer),
    std::move(spd_mips),
    std::move(shading_change)
  );

  // --- prepare reactivity: disocclusion, motion divergence, accumulation, thin feature locks ---
  auto prepare_reactivity_pass = vuk::make_pass(
    "fsr3 prepare reactivity",
    [](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eComputeRead) fsr3_constants,
      VUK_IA(vuk::eComputeSampled) motion_vectors,
      VUK_IA(vuk::eComputeSampled) depth,
      VUK_IA(vuk::eComputeSampled) reconstructed_depth,
      VUK_IA(vuk::eComputeSampled) luma,
      VUK_IA(vuk::eComputeSampled) shading_change_src,
      VUK_IA(vuk::eComputeSampled) accumulation_src,
      VUK_IA(vuk::eComputeSampled) reactive_mask,
      VUK_IA(vuk::eComputeSampled) transparency_mask,
      VUK_BA(vuk::eComputeRead) exposure,
      VUK_IA(vuk::eComputeRW) out_masks,
      VUK_IA(vuk::eComputeRW) out_accumulation,
      VUK_IA(vuk::eComputeRW) out_new_locks
    ) {
      cmd_list.bind_compute_pipeline("fsr3_prepare_reactivity")
        .bind_buffer(0, 0, fsr3_constants)
        .bind_image(0, 1, motion_vectors)
        .bind_image(0, 2, depth)
        .bind_image(0, 3, reconstructed_depth)
        .bind_image(0, 4, luma)
        .bind_image(0, 5, shading_change_src)
        .bind_image(0, 6, accumulation_src)
        .bind_image(0, 7, reactive_mask)
        .bind_image(0, 8, transparency_mask)
        .bind_buffer(0, 9, exposure)
        .bind_sampler(0, 10, vuk::LinearSamplerClamped)
        .bind_image(0, 11, out_masks)
        .bind_image(0, 12, out_accumulation)
        .bind_image(0, 13, out_new_locks)
        .dispatch_invocations_per_pixel(out_masks);

      return std::make_tuple(
        fsr3_constants,
        motion_vectors,
        depth,
        reconstructed_depth,
        luma,
        shading_change_src,
        accumulation_src,
        reactive_mask,
        transparency_mask,
        exposure,
        out_masks,
        out_accumulation,
        out_new_locks
      );
    }
  );

  std::tie(
    constants_buffer,
    dilated_motion_vectors,
    dilated_depth,
    reconstructed_prev_nearest_depth,
    current_luma,
    shading_change,
    accumulation_prev,
    context.reactive_mask_attachment,
    input_transparency_mask,
    context.exposure_buffer,
    dilated_reactive_masks,
    accumulation,
    new_locks
  ) =
    prepare_reactivity_pass(
      std::move(constants_buffer),
      std::move(dilated_motion_vectors),
      std::move(dilated_depth),
      std::move(reconstructed_prev_nearest_depth),
      std::move(current_luma),
      std::move(shading_change),
      std::move(accumulation_prev),
      std::move(input_reactive_mask),
      std::move(input_transparency_mask),
      std::move(context.exposure_buffer),
      std::move(dilated_reactive_masks),
      std::move(accumulation),
      std::move(new_locks)
    );

  // --- luma instability ---
  auto luma_instability_pass = vuk::make_pass(
    "fsr3 luma instability",
    [](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eComputeRead) fsr3_constants,
      VUK_IA(vuk::eComputeSampled) motion_vectors,
      VUK_IA(vuk::eComputeSampled) masks,
      VUK_IA(vuk::eComputeSampled) luma,
      VUK_IA(vuk::eComputeSampled) history_prev,
      VUK_IA(vuk::eComputeSampled) farthest_depth_src,
      VUK_BA(vuk::eComputeRead) exposure,
      VUK_IA(vuk::eComputeRW) out_history,
      VUK_IA(vuk::eComputeRW) out_instability
    ) {
      cmd_list.bind_compute_pipeline("fsr3_luma_instability")
        .bind_buffer(0, 0, fsr3_constants)
        .bind_image(0, 1, motion_vectors)
        .bind_image(0, 2, masks)
        .bind_image(0, 3, luma)
        .bind_image(0, 4, history_prev)
        .bind_image(0, 5, farthest_depth_src)
        .bind_buffer(0, 6, exposure)
        .bind_sampler(0, 7, vuk::LinearSamplerClamped)
        .bind_image(0, 8, out_history)
        .bind_image(0, 9, out_instability)
        .dispatch_invocations_per_pixel(out_instability);

      return std::make_tuple(
        fsr3_constants,
        motion_vectors,
        masks,
        luma,
        history_prev,
        farthest_depth_src,
        exposure,
        out_history,
        out_instability
      );
    }
  );

  std::tie(
    constants_buffer,
    dilated_motion_vectors,
    dilated_reactive_masks,
    current_luma,
    luma_history_prev,
    farthest_depth_mip1,
    context.exposure_buffer,
    luma_history,
    luma_instability
  ) =
    luma_instability_pass(
      std::move(constants_buffer),
      std::move(dilated_motion_vectors),
      std::move(dilated_reactive_masks),
      std::move(current_luma),
      std::move(luma_history_prev),
      std::move(farthest_depth_mip1),
      std::move(context.exposure_buffer),
      std::move(luma_history),
      std::move(luma_instability)
    );

  // --- accumulate ---
  const auto apply_sharpening = context.sharpness > 0.0f;

  auto accumulate_pass = vuk::make_pass(
    "fsr3 accumulate",
    [apply_sharpening](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::eComputeRead) fsr3_constants,
      VUK_IA(vuk::eComputeSampled) color,
      VUK_IA(vuk::eComputeSampled) motion_vectors,
      VUK_IA(vuk::eComputeSampled) masks,
      VUK_IA(vuk::eComputeSampled) instability,
      VUK_IA(vuk::eComputeSampled) farthest_depth_src,
      VUK_IA(vuk::eComputeSampled) history_prev,
      VUK_BA(vuk::eComputeRead) exposure,
      VUK_IA(vuk::eComputeRW) locks,
      VUK_IA(vuk::eComputeRW) out_history,
      VUK_IA(vuk::eComputeRW) out_upscaled
    ) {
      cmd_list.bind_compute_pipeline("fsr3_accumulate")
        .bind_buffer(0, 0, fsr3_constants)
        .bind_image(0, 1, color)
        .bind_image(0, 2, motion_vectors)
        .bind_image(0, 3, masks)
        .bind_image(0, 4, instability)
        .bind_image(0, 5, farthest_depth_src)
        .bind_image(0, 6, history_prev)
        .bind_buffer(0, 7, exposure)
        .bind_sampler(0, 8, vuk::LinearSamplerClamped)
        .bind_image(0, 9, locks)
        .bind_image(0, 10, out_history)
        .bind_image(0, 11, out_upscaled)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(static_cast<u32>(apply_sharpening)))
        .dispatch_invocations_per_pixel(out_history);

      return std::make_tuple(
        fsr3_constants,
        color,
        motion_vectors,
        masks,
        instability,
        farthest_depth_src,
        history_prev,
        exposure,
        locks,
        out_history,
        out_upscaled
      );
    }
  );

  std::tie(
    constants_buffer,
    context.color_attachment,
    dilated_motion_vectors,
    dilated_reactive_masks,
    luma_instability,
    farthest_depth_mip1,
    internal_upscaled_color_prev,
    context.exposure_buffer,
    new_locks,
    internal_upscaled_color,
    upscaled_output
  ) =
    accumulate_pass(
      std::move(constants_buffer),
      std::move(context.color_attachment),
      std::move(dilated_motion_vectors),
      std::move(dilated_reactive_masks),
      std::move(luma_instability),
      std::move(farthest_depth_mip1),
      std::move(internal_upscaled_color_prev),
      std::move(context.exposure_buffer),
      std::move(new_locks),
      std::move(internal_upscaled_color),
      std::move(upscaled_output)
    );

  // --- rcas sharpening, which owns the final write when it runs ---
  if (apply_sharpening) {
    auto rcas_pass = vuk::make_pass(
      "fsr3 rcas",
      [sharpness = context.sharpness](
        vuk::CommandBuffer& cmd_list,
        VUK_BA(vuk::eComputeRead) fsr3_constants,
        VUK_IA(vuk::eComputeSampled) src,
        VUK_BA(vuk::eComputeRead) exposure,
        VUK_IA(vuk::eComputeRW) dst
      ) {
        // the SDK's sharpness is in stops, exp2(-stops); the cvar is already linear
        cmd_list.bind_compute_pipeline("fsr3_rcas")
          .bind_buffer(0, 0, fsr3_constants)
          .bind_image(0, 1, src)
          .bind_buffer(0, 2, exposure)
          .bind_image(0, 3, dst)
          .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(sharpness))
          .dispatch_invocations_per_pixel(dst);

        return std::make_tuple(fsr3_constants, src, exposure, dst);
      }
    );

    auto sharpened_output = vuk::declare_ia(
      "fsr3 sharpened output",
      {.usage = vuk::ImageUsageFlagBits::eStorage | vuk::ImageUsageFlagBits::eSampled |
                vuk::ImageUsageFlagBits::eColorAttachment,
       .extent = display_extent,
       .format = vuk::Format::eR16G16B16A16Sfloat,
       .sample_count = vuk::Samples::e1,
       .level_count = 1,
       .layer_count = 1}
    );

    std::tie(constants_buffer, internal_upscaled_color, context.exposure_buffer, sharpened_output) = rcas_pass(
      std::move(constants_buffer),
      std::move(internal_upscaled_color),
      std::move(context.exposure_buffer),
      std::move(sharpened_output)
    );

    upscaled_output = std::move(sharpened_output);
  }

  // --- diagnostic view, replaces the output with one of the intermediates ---
  if (context.debug_view != 0) {
    auto debug_pass = vuk::make_pass(
      "fsr3 debug view",
      [mode = context.debug_view](
        vuk::CommandBuffer& cmd_list,
        VUK_BA(vuk::eComputeRead) fsr3_constants,
        VUK_IA(vuk::eComputeSampled) motion_vectors,
        VUK_IA(vuk::eComputeSampled) masks,
        VUK_IA(vuk::eComputeSampled) instability,
        VUK_IA(vuk::eComputeSampled) depth,
        VUK_IA(vuk::eComputeRW) dst
      ) {
        cmd_list.bind_compute_pipeline("fsr3_debug_view")
          .bind_buffer(0, 0, fsr3_constants)
          .bind_image(0, 1, motion_vectors)
          .bind_image(0, 2, masks)
          .bind_image(0, 3, instability)
          .bind_image(0, 4, depth)
          .bind_sampler(0, 5, vuk::LinearSamplerClamped)
          .bind_image(0, 6, dst)
          .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(mode))
          .dispatch_invocations_per_pixel(dst);

        return std::make_tuple(fsr3_constants, motion_vectors, masks, instability, depth, dst);
      }
    );

    std::tie(
      constants_buffer,
      dilated_motion_vectors,
      dilated_reactive_masks,
      luma_instability,
      dilated_depth,
      upscaled_output
    ) =
      debug_pass(
        std::move(constants_buffer),
        std::move(dilated_motion_vectors),
        std::move(dilated_reactive_masks),
        std::move(luma_instability),
        std::move(dilated_depth),
        std::move(upscaled_output)
      );
  }

  // the history writes ride along on passes that are already kept alive by their other outputs
  // (accumulate feeds upscaled_output, prepare_inputs feeds the dilated targets, and so on), so
  // there is nothing further to release here
  self.fsr3_history_ping = !self.fsr3_history_ping;
  self.fsr3_history_valid = true;

  return upscaled_output;
}
} // namespace ox
