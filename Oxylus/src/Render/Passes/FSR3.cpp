#include <vir/RenderGraph.hpp>
#include <vir/Types.hpp>

#include "Render/RenderContext.hpp"
#include "Render/RendererInstance.hpp"
#include "Render/Utils/PushConstants.hpp"
#include "Render/Utils/Samplers.hpp"
#include "Utils/Log.hpp"

namespace ox {
// FSR3 runs the shading change pyramid over three mips; see fsr3_shading_change.slang
constexpr static u32 FSR3_SHADING_CHANGE_MIP_COUNT = 3;
// history rests here between frames, which is how the passes that read it last leave it
constexpr static auto FSR3_HISTORY_REST = vir::Access::ComputeSampled;

using FSR3AccumulatePush = PushConstants<u32>;
using FSR3SharpnessPush = PushConstants<f32>;
using FSR3DebugModePush = PushConstants<u32>;

constexpr static auto TRANSIENT_BUFFER_USAGE = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

auto RendererInstance::bind_fsr3_variables(
  this const RendererInstance& self, vir::Program& program, const FSR3Variables& variables
) -> void {
  ZoneScoped;

  if (self.frame_params.shape.upscaler == UpscalerBackend::None) {
    return;
  }

  const auto& params = self.frame_params;
  program.set_bytes(variables.constants, params.fsr3_constants);
  program.set(variables.is_reset, params.fsr3_reset);
  program.set_bytes(variables.accumulate_push, FSR3AccumulatePush(static_cast<u32>(params.fsr3_sharpness > 0.0f)));
  program.set(variables.is_sharpening, params.fsr3_sharpness > 0.0f);
  program.set_bytes(variables.sharpness, FSR3SharpnessPush(params.fsr3_sharpness));
  program.set(variables.is_debugging, params.fsr3_debug_view != 0);
  program.set_bytes(variables.debug_mode, FSR3DebugModePush(params.fsr3_debug_view));

  // the two halves of each history pair swap every frame
  const auto current_index = params.fsr3_history_ping ? 1 : 0;
  const auto previous_index = 1 - current_index;
  program.set(variables.internal_upscaled_color, self.fsr3_internal_upscaled_color[current_index].attachment_info());
  program.set(variables.accumulation, self.fsr3_accumulation[current_index].attachment_info());
  program.set(variables.current_luma, self.fsr3_luma[current_index].attachment_info());
  program.set(variables.luma_history, self.fsr3_luma_history[current_index].attachment_info());
  program.set(
    variables.internal_upscaled_color_prev,
    self.fsr3_internal_upscaled_color[previous_index].attachment_info()
  );
  program.set(variables.accumulation_prev, self.fsr3_accumulation[previous_index].attachment_info());
  program.set(variables.previous_luma, self.fsr3_luma[previous_index].attachment_info());
  program.set(variables.luma_history_prev, self.fsr3_luma_history[previous_index].attachment_info());
}

auto RendererInstance::sharpen_fsr3(
  vir::Module& module,
  RenderContext& render_context,
  vir::ValueID enabled,
  vir::ValueID sharpness,
  vir::ValueID constants,
  vir::ValueID history,
  vir::ValueID exposure,
  vir::ValueID output
) -> std::array<vir::ValueID, 4> {
  // both arms keep the same physical output so every later use sees its writes and layout
  return module.set_condition(
    enabled,
    [&](vir::Module& arm) {
      return arm
        .begin_compute(
          {{constants, vir::Access::ComputeUniformRead},
           {history, vir::Access::ComputeSampled},
           {exposure, vir::Access::ComputeUniformRead},
           {output, vir::Access::ComputeRW}}
        )
        .with_name("fsr3 rcas")
        .bind_compute_pipeline(render_context.pipeline("fsr3_rcas"))
        .bind_buffer(0, 0, constants)
        .bind_image(0, 1, history)
        .bind_buffer(0, 2, exposure)
        .bind_image(0, 3, output)
        .push_constants_from(sharpness)
        .dispatch_invocations_per_pixel(output)
        .end_compute();
    },
    [&](vir::Module&) { return std::array{constants, history, exposure, output}; }
  );
}

auto reactive_mask_blend_state() -> vir::ColorBlendAttachmentState {
  return {
    .is_blend_enabled = true,
    .src_color_blend_factor = VK_BLEND_FACTOR_ONE,
    .dst_color_blend_factor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
    .color_blend_op = VK_BLEND_OP_ADD,
    .src_alpha_blend_factor = VK_BLEND_FACTOR_ONE,
    .dst_alpha_blend_factor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    .alpha_blend_op = VK_BLEND_OP_ADD,
  };
}

auto RendererInstance::allocate_fsr3_resources(
  this RendererInstance& self, const glm::uvec2 render_size, const glm::uvec2 display_size
) -> void {
  ZoneScoped;

  if (self.fsr3_render_size == render_size && self.fsr3_display_size == display_size) {
    return;
  }

  const auto render_extent = vir::Extent3D{render_size.x, render_size.y, 1};
  const auto display_extent = vir::Extent3D{display_size.x, display_size.y, 1};
  for (auto i = 0_u32; i < 2; i++) {
    self.fsr3_internal_upscaled_color[i] = Texture::create(
      {.format = vir::Format::RGBA16Sfloat,
       .extent = display_extent,
       .usage = vir::ImageUsageFlags::Storage | vir::ImageUsageFlags::Sampled | vir::ImageUsageFlags::TransferDst}
    );
    self.fsr3_accumulation[i] = Texture::create(
      {.format = vir::Format::R8Unorm,
       .extent = render_extent,
       .usage = vir::ImageUsageFlags::Storage | vir::ImageUsageFlags::Sampled | vir::ImageUsageFlags::TransferDst}
    );
    self.fsr3_luma[i] = Texture::create(
      {.format = vir::Format::R16Sfloat,
       .extent = render_extent,
       .usage = vir::ImageUsageFlags::Storage | vir::ImageUsageFlags::Sampled | vir::ImageUsageFlags::TransferDst}
    );
    self.fsr3_luma_history[i] = Texture::create(
      {.format = vir::Format::RGBA16Sfloat,
       .extent = render_extent,
       .usage = vir::ImageUsageFlags::Storage | vir::ImageUsageFlags::Sampled | vir::ImageUsageFlags::TransferDst}
    );
  }

  auto module = vir::Module{};
  const auto black = vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 0.0f);
  for (auto i = 0_u32; i < 2; i++) {
    for (const auto* texture :
         {&self.fsr3_internal_upscaled_color[i],
          &self.fsr3_accumulation[i],
          &self.fsr3_luma[i],
          &self.fsr3_luma_history[i]}) {
      const auto cleared = module.clear(texture->import_discarded(module, vir::Access::None), black);
      module.export_to(cleared, FSR3_HISTORY_REST);
    }
  }
  if (const auto initialized = self.renderer.render_context->execute_blocking(std::move(module)); !initialized) {
    OX_LOG_FATAL("failed to initialize FSR3 history ({})", std::to_underlying(initialized.error()));
  }

  self.fsr3_render_size = render_size;
  self.fsr3_display_size = display_size;
  // the freshly allocated history is undefined, so the next frame treats every pixel as new
  self.fsr3_history_valid = false;
}

auto RendererInstance::apply_fsr3(
  this RendererInstance& self, vir::Module& module, FrameRecording& recording, FSR3Context& context
) -> vir::ValueID {
  ZoneScoped;
  auto& prepared = recording.prepared;

  auto& render_context = *self.renderer.render_context;
  auto& variables = prepared.fsr3;
  variables.constants = module.declare_bytes_var("fsr3 constants", sizeof(GPU::FSR3Constants));
  variables.is_reset = module.declare_bool_var("fsr3 reset", false);
  variables.accumulate_push = module.declare_bytes_var("fsr3 accumulate push", sizeof(FSR3AccumulatePush));
  variables.is_sharpening = module.declare_bool_var("fsr3 sharpening", false);
  variables.sharpness = module.declare_bytes_var("fsr3 sharpness", sizeof(FSR3SharpnessPush));
  variables.is_debugging = module.declare_bool_var("fsr3 debug view", false);
  variables.debug_mode = module.declare_bytes_var("fsr3 debug mode", sizeof(FSR3DebugModePush));
  variables.internal_upscaled_color = module.declare_image_var(
    "fsr3 internal upscaled color",
    vir::Format::RGBA16Sfloat,
    FSR3_HISTORY_REST
  );
  variables.accumulation = module.declare_image_var("fsr3 accumulation", vir::Format::R8Unorm, FSR3_HISTORY_REST);
  variables.current_luma = module.declare_image_var("fsr3 current luma", vir::Format::R16Sfloat, FSR3_HISTORY_REST);
  variables.luma_history = module.declare_image_var("fsr3 luma history", vir::Format::RGBA16Sfloat, FSR3_HISTORY_REST);
  variables.internal_upscaled_color_prev = module.declare_image_var(
    "fsr3 prev color",
    vir::Format::RGBA16Sfloat,
    FSR3_HISTORY_REST
  );
  variables.accumulation_prev = module
                                  .declare_image_var("fsr3 prev accumulation", vir::Format::R8Unorm, FSR3_HISTORY_REST);
  variables.previous_luma = module.declare_image_var("fsr3 prev luma", vir::Format::R16Sfloat, FSR3_HISTORY_REST);
  variables.luma_history_prev = module.declare_image_var(
    "fsr3 prev luma history",
    vir::Format::RGBA16Sfloat,
    FSR3_HISTORY_REST
  );

  const auto& shape = recording.shape;
  const auto render_extent = vir::Extent3D{shape.render_size.x, shape.render_size.y, 1};
  const auto half_extent = vir::Extent3D{
    std::max(shape.render_size.x / 2, 1u),
    std::max(shape.render_size.y / 2, 1u),
    1
  };
  const auto display_extent = vir::Extent3D{shape.display_size.x, shape.display_size.y, 1};
  const auto linear_clamped = render_context.cached_sampler_handle(LinearSamplerClamped);

  auto constants = module.transient_buffer({
    .size = sizeof(GPU::FSR3Constants),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "fsr3 constants",
  });

  constants = module.update_buffer_from(constants, variables.constants);

  auto internal_upscaled_color = variables.internal_upscaled_color;
  auto accumulation = variables.accumulation;
  auto current_luma = variables.current_luma;
  auto luma_history = variables.luma_history;
  auto internal_upscaled_color_prev = variables.internal_upscaled_color_prev;
  auto accumulation_prev = variables.accumulation_prev;
  auto previous_luma = variables.previous_luma;
  auto luma_history_prev = variables.luma_history_prev;

  // on a reset there is no meaningful previous frame, so the far side of the ping-pong is cleared and
  // the frame_index == 0 path in the shaders rejects it anyway
  std::tie(internal_upscaled_color_prev, accumulation_prev, previous_luma, luma_history_prev) = std::tuple_cat(
    module.set_condition(
      variables.is_reset,
      [&](vir::Module& arm) {
        const auto black = vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 1.0f);
        return std::array{
          arm.clear(internal_upscaled_color_prev, black),
          arm.clear(accumulation_prev, black),
          arm.clear(previous_luma, black),
          arm.clear(luma_history_prev, black),
        };
      },
      [&](vir::Module&) {
        return std::array{internal_upscaled_color_prev, accumulation_prev, previous_luma, luma_history_prev};
      }
    )
  );

  auto dilated_motion_vectors = module.transient_image(
    {.extent = render_extent, .format = vir::Format::RG16Sfloat, .name = "fsr3 dilated motion vectors"}
  );
  auto dilated_depth = module.transient_image(
    {.extent = render_extent, .format = vir::Format::R32Sfloat, .name = "fsr3 dilated depth"}
  );
  auto farthest_depth = module.transient_image(
    {.extent = render_extent, .format = vir::Format::R16Sfloat, .name = "fsr3 farthest depth"}
  );
  auto dilated_reactive_masks = module.transient_image(
    {.extent = render_extent, .format = vir::Format::RGBA8Unorm, .name = "fsr3 dilated reactive masks"}
  );
  auto luma_instability = module.transient_image(
    {.extent = render_extent, .format = vir::Format::R16Sfloat, .name = "fsr3 luma instability"}
  );

  // the depth scatter in prepare_inputs is an InterlockedMax, so it has to start from the reversed-z
  // "nothing here" value of 0
  auto reconstructed_prev_nearest_depth = module.transient_image(
    {.extent = render_extent, .format = vir::Format::R32Uint, .name = "fsr3 reconstructed prev depth"}
  );
  reconstructed_prev_nearest_depth = module.clear(
    reconstructed_prev_nearest_depth,
    vir::ClearValue::rgba_u32(0, 0, 0, 1)
  );

  // sprites and particles author the reactive mask while compositing; there is no separate
  // transparency and composition mask yet, so that one stays cleared
  auto input_transparency_mask = module.transient_image(
    {.extent = render_extent, .format = vir::Format::R8Unorm, .name = "fsr3 input transparency mask"}
  );
  input_transparency_mask = module.clear(input_transparency_mask, vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 1.0f));

  auto farthest_depth_mip1 = module.transient_image({
    .extent = half_extent,
    .format = vir::Format::R16Sfloat,
    .name = "fsr3 farthest depth mip1",
  });
  auto shading_change = module.transient_image({
    .extent = half_extent,
    .format = vir::Format::R8Unorm,
    .name = "fsr3 shading change",
  });
  auto spd_mips = module.transient_image({
    .extent = half_extent,
    .format = vir::Format::RG16Sfloat,
    .mip_levels = FSR3_SHADING_CHANGE_MIP_COUNT,
    .name = "fsr3 spd mips",
  });

  // scattered writes from prepare_reactivity only touch thin feature pixels, so the rest has to be
  // cleared rather than left over from last frame
  auto new_locks = module.transient_image(
    {.extent = display_extent, .format = vir::Format::R8Unorm, .name = "fsr3 new locks"}
  );
  new_locks = module.clear(new_locks, vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 1.0f));

  auto upscaled_output = module.transient_image({
    .extent = display_extent,
    .format = vir::Format::RGBA16Sfloat,
    .name = "fsr3 upscaled output",
  });

  // --- prepare inputs: dilate depth and motion, scatter reconstructed depth, extract luma ---
  std::tie(
    constants,
    context.velocity_attachment,
    context.depth_attachment,
    context.color_attachment,
    dilated_motion_vectors,
    dilated_depth,
    reconstructed_prev_nearest_depth,
    farthest_depth,
    current_luma
  ) = std::tuple_cat( //
    module
      .begin_compute(
        {{constants, vir::Access::ComputeUniformRead},
         {context.velocity_attachment, vir::Access::ComputeSampled},
         {context.depth_attachment, vir::Access::ComputeSampled},
         {context.color_attachment, vir::Access::ComputeSampled},
         {dilated_motion_vectors, vir::Access::ComputeRW},
         {dilated_depth, vir::Access::ComputeRW},
         {reconstructed_prev_nearest_depth, vir::Access::ComputeRW},
         {farthest_depth, vir::Access::ComputeRW},
         {current_luma, vir::Access::ComputeRW}}
      )
      .with_name("fsr3 prepare inputs")
      .bind_compute_pipeline(render_context.pipeline("fsr3_prepare_inputs"))
      .bind_buffer(0, 0, constants)
      .bind_image(0, 1, context.velocity_attachment)
      .bind_image(0, 2, context.depth_attachment)
      .bind_image(0, 3, context.color_attachment)
      .bind_image(0, 4, dilated_motion_vectors)
      .bind_image(0, 5, dilated_depth)
      .bind_image(0, 6, reconstructed_prev_nearest_depth)
      .bind_image(0, 7, farthest_depth)
      .bind_image(0, 8, current_luma)
      .dispatch_invocations_per_pixel(dilated_depth)
      .end_compute()
  );

  // --- farthest depth mip 1 ---
  std::tie(constants, farthest_depth, farthest_depth_mip1) = std::tuple_cat(
    module
      .begin_compute(
        {{constants, vir::Access::ComputeUniformRead},
         {farthest_depth, vir::Access::ComputeSampled},
         {farthest_depth_mip1, vir::Access::ComputeRW}}
      )
      .with_name("fsr3 farthest depth mip1")
      .bind_compute_pipeline(render_context.pipeline("fsr3_farthest_depth_mip1"))
      .bind_buffer(0, 0, constants)
      .bind_image(0, 1, farthest_depth)
      .bind_image(0, 2, farthest_depth_mip1)
      .dispatch_invocations_per_pixel(farthest_depth_mip1)
      .end_compute()
  );

  // --- shading change pyramid: mip 0 from the luma pair, then one reduction pass per mip ---
  const auto spd_mip0 = module.mip(spd_mips, 0);
  auto pass = //
    module
      .begin_compute(
        {{constants, vir::Access::ComputeUniformRead},
         {current_luma, vir::Access::ComputeSampled},
         {previous_luma, vir::Access::ComputeSampled},
         {dilated_motion_vectors, vir::Access::ComputeSampled},
         {context.exposure_buffer, vir::Access::ComputeUniformRead},
         {spd_mip0, vir::Access::ComputeRW}}
      )
      .with_name("fsr3 shading change pyramid")
      .bind_compute_pipeline(render_context.pipeline("fsr3_shading_change_pyramid"))
      .bind_buffer(0, 0, constants)
      .bind_image(0, 1, current_luma)
      .bind_image(0, 2, previous_luma)
      .bind_image(0, 3, dilated_motion_vectors)
      .bind_buffer(0, 4, context.exposure_buffer)
      .bind_image(0, 5, spd_mip0)
      .dispatch_invocations(half_extent.width, half_extent.height, 1);
  {
    const auto results = pass.end_compute();
    std::tie(constants, current_luma, previous_luma, dilated_motion_vectors, context.exposure_buffer) = std::tuple(
      results[0],
      results[1],
      results[2],
      results[3],
      results[4]
    );
    spd_mips = module.whole(results[5]);
  }

  for (auto mip = 1_u32; mip < FSR3_SHADING_CHANGE_MIP_COUNT; mip++) {
    const auto dst_size = glm::ivec2(
      std::max(1_u32, half_extent.width >> mip),
      std::max(1_u32, half_extent.height >> mip)
    );
    const auto src_size = glm::ivec2(
      std::max(1_u32, half_extent.width >> (mip - 1)),
      std::max(1_u32, half_extent.height >> (mip - 1))
    );
    const auto src = module.mip(spd_mips, mip - 1);
    const auto dst = module.mip(spd_mips, mip);
    spd_mips = module.whole( //
      module.begin_compute({{src, vir::Access::ComputeSampled}, {dst, vir::Access::ComputeWrite}})
        .with_name("fsr3 downsample")
        .bind_compute_pipeline(render_context.pipeline("fsr3_downsample"))
        .bind_image(0, 0, src)
        .bind_image(0, 1, dst)
        .push_constants(PushConstants(dst_size, src_size))
        .dispatch_invocations(static_cast<u32>(dst_size.x), static_cast<u32>(dst_size.y), 1)
        .end_compute()[1]
    );
  }

  // --- shading change ---
  std::tie(constants, spd_mips, shading_change) = std::tuple_cat(
    module
      .begin_compute(
        {{constants, vir::Access::ComputeUniformRead},
         {spd_mips, vir::Access::ComputeSampled},
         {shading_change, vir::Access::ComputeRW}}
      )
      .with_name("fsr3 shading change")
      .bind_compute_pipeline(render_context.pipeline("fsr3_shading_change"))
      .bind_buffer(0, 0, constants)
      .bind_image(0, 1, spd_mips)
      .bind_sampler(0, 2, linear_clamped)
      .bind_image(0, 3, shading_change)
      .dispatch_invocations_per_pixel(shading_change)
      .end_compute()
  );

  // --- prepare reactivity: disocclusion, motion divergence, accumulation, thin feature locks ---
  std::tie(
    constants,
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
  ) = std::tuple_cat( //
    module
      .begin_compute(
        {{constants, vir::Access::ComputeUniformRead},
         {dilated_motion_vectors, vir::Access::ComputeSampled},
         {dilated_depth, vir::Access::ComputeSampled},
         {reconstructed_prev_nearest_depth, vir::Access::ComputeSampled},
         {current_luma, vir::Access::ComputeSampled},
         {shading_change, vir::Access::ComputeSampled},
         {accumulation_prev, vir::Access::ComputeSampled},
         {context.reactive_mask_attachment, vir::Access::ComputeSampled},
         {input_transparency_mask, vir::Access::ComputeSampled},
         {context.exposure_buffer, vir::Access::ComputeUniformRead},
         {dilated_reactive_masks, vir::Access::ComputeRW},
         {accumulation, vir::Access::ComputeRW},
         {new_locks, vir::Access::ComputeRW}}
      )
      .with_name("fsr3 prepare reactivity")
      .bind_compute_pipeline(render_context.pipeline("fsr3_prepare_reactivity"))
      .bind_buffer(0, 0, constants)
      .bind_image(0, 1, dilated_motion_vectors)
      .bind_image(0, 2, dilated_depth)
      .bind_image(0, 3, reconstructed_prev_nearest_depth)
      .bind_image(0, 4, current_luma)
      .bind_image(0, 5, shading_change)
      .bind_image(0, 6, accumulation_prev)
      .bind_image(0, 7, context.reactive_mask_attachment)
      .bind_image(0, 8, input_transparency_mask)
      .bind_buffer(0, 9, context.exposure_buffer)
      .bind_sampler(0, 10, linear_clamped)
      .bind_image(0, 11, dilated_reactive_masks)
      .bind_image(0, 12, accumulation)
      .bind_image(0, 13, new_locks)
      .dispatch_invocations_per_pixel(dilated_reactive_masks)
      .end_compute()
  );

  // --- luma instability ---
  std::tie(
    constants,
    dilated_motion_vectors,
    dilated_reactive_masks,
    current_luma,
    luma_history_prev,
    farthest_depth_mip1,
    context.exposure_buffer,
    luma_history,
    luma_instability
  ) = std::tuple_cat( //
    module
      .begin_compute(
        {{constants, vir::Access::ComputeUniformRead},
         {dilated_motion_vectors, vir::Access::ComputeSampled},
         {dilated_reactive_masks, vir::Access::ComputeSampled},
         {current_luma, vir::Access::ComputeSampled},
         {luma_history_prev, vir::Access::ComputeSampled},
         {farthest_depth_mip1, vir::Access::ComputeSampled},
         {context.exposure_buffer, vir::Access::ComputeUniformRead},
         {luma_history, vir::Access::ComputeRW},
         {luma_instability, vir::Access::ComputeRW}}
      )
      .with_name("fsr3 luma instability")
      .bind_compute_pipeline(render_context.pipeline("fsr3_luma_instability"))
      .bind_buffer(0, 0, constants)
      .bind_image(0, 1, dilated_motion_vectors)
      .bind_image(0, 2, dilated_reactive_masks)
      .bind_image(0, 3, current_luma)
      .bind_image(0, 4, luma_history_prev)
      .bind_image(0, 5, farthest_depth_mip1)
      .bind_buffer(0, 6, context.exposure_buffer)
      .bind_sampler(0, 7, linear_clamped)
      .bind_image(0, 8, luma_history)
      .bind_image(0, 9, luma_instability)
      .dispatch_invocations_per_pixel(luma_instability)
      .end_compute()
  );

  // --- accumulate ---
  std::tie(
    constants,
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
  ) = std::tuple_cat( //
    module
      .begin_compute(
        {{constants, vir::Access::ComputeUniformRead},
         {context.color_attachment, vir::Access::ComputeSampled},
         {dilated_motion_vectors, vir::Access::ComputeSampled},
         {dilated_reactive_masks, vir::Access::ComputeSampled},
         {luma_instability, vir::Access::ComputeSampled},
         {farthest_depth_mip1, vir::Access::ComputeSampled},
         {internal_upscaled_color_prev, vir::Access::ComputeSampled},
         {context.exposure_buffer, vir::Access::ComputeUniformRead},
         {new_locks, vir::Access::ComputeRW},
         {internal_upscaled_color, vir::Access::ComputeRW},
         {upscaled_output, vir::Access::ComputeRW}}
      )
      .with_name("fsr3 accumulate")
      .bind_compute_pipeline(render_context.pipeline("fsr3_accumulate"))
      .bind_buffer(0, 0, constants)
      .bind_image(0, 1, context.color_attachment)
      .bind_image(0, 2, dilated_motion_vectors)
      .bind_image(0, 3, dilated_reactive_masks)
      .bind_image(0, 4, luma_instability)
      .bind_image(0, 5, farthest_depth_mip1)
      .bind_image(0, 6, internal_upscaled_color_prev)
      .bind_buffer(0, 7, context.exposure_buffer)
      .bind_sampler(0, 8, linear_clamped)
      .bind_image(0, 9, new_locks)
      .bind_image(0, 10, internal_upscaled_color)
      .bind_image(0, 11, upscaled_output)
      .push_constants_from(variables.accumulate_push)
      .dispatch_invocations_per_pixel(internal_upscaled_color)
      .end_compute()
  );

  // --- rcas sharpening, which owns the final write when it runs ---
  // the SDK's sharpness is in stops, exp2(-stops), the cvar is already linear
  std::tie(constants, internal_upscaled_color, context.exposure_buffer, upscaled_output) = std::tuple_cat(sharpen_fsr3(
    module,
    render_context,
    variables.is_sharpening,
    variables.sharpness,
    constants,
    internal_upscaled_color,
    context.exposure_buffer,
    upscaled_output
  ));

  // --- diagnostic view, replaces the output with one of the intermediates ---
  std::tie(
    constants,
    dilated_motion_vectors,
    dilated_reactive_masks,
    luma_instability,
    dilated_depth,
    upscaled_output
  ) = std::tuple_cat( //
    module.set_condition(
      variables.is_debugging,
      [&](vir::Module& arm) {
        return arm
          .begin_compute(
            {{constants, vir::Access::ComputeUniformRead},
             {dilated_motion_vectors, vir::Access::ComputeSampled},
             {dilated_reactive_masks, vir::Access::ComputeSampled},
             {luma_instability, vir::Access::ComputeSampled},
             {dilated_depth, vir::Access::ComputeSampled},
             {upscaled_output, vir::Access::ComputeRW}}
          )
          .with_name("fsr3 debug view")
          .bind_compute_pipeline(render_context.pipeline("fsr3_debug_view"))
          .bind_buffer(0, 0, constants)
          .bind_image(0, 1, dilated_motion_vectors)
          .bind_image(0, 2, dilated_reactive_masks)
          .bind_image(0, 3, luma_instability)
          .bind_image(0, 4, dilated_depth)
          .bind_sampler(0, 5, linear_clamped)
          .bind_image(0, 6, upscaled_output)
          .push_constants_from(variables.debug_mode)
          .dispatch_invocations_per_pixel(upscaled_output)
          .end_compute();
      },
      [&](vir::Module&) {
        return std::array{
          constants,
          dilated_motion_vectors,
          dilated_reactive_masks,
          luma_instability,
          dilated_depth,
          upscaled_output,
        };
      }
    )
  );

  // every history image goes back to rest so the next frame can import it, which also keeps the
  // writes into the current half alive
  for (const auto image : {
         internal_upscaled_color,
         accumulation,
         current_luma,
         luma_history,
         internal_upscaled_color_prev,
         accumulation_prev,
         previous_luma,
         luma_history_prev,
       }) {
    module.export_to(image, FSR3_HISTORY_REST);
  }

  context.output_attachment = upscaled_output;
  return upscaled_output;
}
} // namespace ox
