#include <vir/RenderGraph.hpp>
#include <vir/Types.hpp>

#include "Render/RenderContext.hpp"
#include "Render/Renderer.hpp"
#include "Render/RendererInstance.hpp"
#include "Render/Utils/PushConstants.hpp"
#include "Utils/Log.hpp"

namespace ox {
using RTGIGBufferPush = PushConstants<glm::uvec2>;

static auto rtgi_half_extent(const glm::uvec2 render_size) -> vir::Extent3D {
  return {(render_size.x + 1_u32) / 2_u32, (render_size.y + 1_u32) / 2_u32, 1};
}

auto RendererInstance::bind_rtgi_variables(
  this const RendererInstance& self, vir::Program& program, const RTGIVariables& variables
) -> void {
  ZoneScoped;

  if (!program.has_variable(variables.is_reset)) {
    return;
  }

  const auto& params = self.frame_params;
  const auto& cvar = *params.cvar;
  program.set(variables.is_reset, params.rtgi_reset);

  const auto half_extent = rtgi_half_extent(params.render_size);

  program.set_bytes(
    variables.settings,
    GPU::RTGISettings{
      .half_size = {half_extent.width, half_extent.height},
      .max_temporal_samples = static_cast<f32>(std::clamp(cvar.cvar_rtgi_max_temporal_samples.get(), 1, 1024)),
      .fast_convergence_samples = std::max(cvar.cvar_rtgi_fast_convergence_samples.get(), 0.0f),
      .temporal_fast_history_frames = static_cast<u32>(
        std::clamp(cvar.cvar_rtgi_temporal_fast_history_frames.get(), 1, 15)
      ),
      .post_blur_max_width = static_cast<u32>(std::clamp(cvar.cvar_rtgi_post_blur_max_width.get(), 1, 16)),
      .is_eye_adapted = static_cast<u32>(params.shape.has_eye_adaptation),
      .fixed_exposure = cvar.cvar_exposure.get(),
      .ray_percentage = std::clamp(cvar.cvar_rtgi_ray_percentage.get(), 0.05f, 4.0f),
      .min_ray_budget = std::clamp(cvar.cvar_rtgi_min_ray_budget.get(), 0.0f, 1.0f),
      .max_rays_per_pixel = static_cast<u32>(std::clamp(cvar.cvar_rtgi_max_rays_per_pixel.get(), 1, 12)),
      .frame_index = params.frame_index & 0xFFF_u32,
      .firefly_filter_enabled = static_cast<u32>(cvar.cvar_rtgi_firefly_filter.as_bool()),
      .firefly_perceptual_tolerance = std::clamp(cvar.cvar_rtgi_firefly_tolerance.get(), 0.0f, 16.0f),
      .firefly_clamp_mode = 0,
      .pre_blur_base_width = std::clamp(cvar.cvar_rtgi_pre_blur_width.get(), 1.0f, 64.0f),
    }
  );

  const auto current_index = params.rtgi_history_ping ? 1 : 0;
  const auto previous_index = 1 - current_index;
  program.set(variables.half_res_depth, self.rtgi_half_res_depth[current_index].attachment_info());
  program.set(variables.previous_half_res_depth, self.rtgi_half_res_depth[previous_index].attachment_info());
  program.set(variables.half_res_face_normal, self.rtgi_half_res_face_normal[current_index].attachment_info());
  program.set(
    variables.previous_half_res_face_normal,
    self.rtgi_half_res_face_normal[previous_index].attachment_info()
  );
  program.set(
    variables.half_res_normal_roughness,
    self.rtgi_half_res_normal_roughness[current_index].attachment_info()
  );
  program.set(
    variables.previous_half_res_normal_roughness,
    self.rtgi_half_res_normal_roughness[previous_index].attachment_info()
  );
}

auto RendererInstance::allocate_rtgi_resources(this RendererInstance& self, const glm::uvec2 render_size) -> void {
  ZoneScoped;

  if (self.rtgi_render_size == render_size) {
    return;
  }

  constexpr auto history_usage = vir::ImageUsageFlags::Storage | vir::ImageUsageFlags::Sampled |
                                 vir::ImageUsageFlags::TransferDst;
  const auto half_extent = rtgi_half_extent(render_size);
  for (auto i = 0_u32; i < 2; i++) {
    self.rtgi_half_res_depth[i] = Texture::create(
      {.format = vir::Format::R32Sfloat, .extent = half_extent, .usage = history_usage}
    );
    self.rtgi_half_res_face_normal[i] = Texture::create(
      {.format = vir::Format::R32Uint, .extent = half_extent, .usage = history_usage}
    );
    self.rtgi_half_res_normal_roughness[i] = Texture::create(
      {.format = vir::Format::R32Uint, .extent = half_extent, .usage = history_usage}
    );
  }

  auto module = vir::Module{};
  const auto black = vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 0.0f);
  const auto zero = vir::ClearValue::rgba_u32(0, 0, 0, 0);
  for (auto i = 0_u32; i < 2; i++) {
    const auto cleared_depth = module.clear(
      self.rtgi_half_res_depth[i].import_discarded(module, vir::Access::None),
      black
    );
    module.export_to(cleared_depth, RTGIContext::HISTORY_REST);

    for (const auto* texture : {&self.rtgi_half_res_face_normal[i], &self.rtgi_half_res_normal_roughness[i]}) {
      const auto cleared = module.clear(texture->import_discarded(module, vir::Access::None), zero);
      module.export_to(cleared, RTGIContext::HISTORY_REST);
    }
  }

  if (const auto initialized = self.renderer.render_context->execute_blocking(std::move(module)); !initialized) {
    OX_LOG_FATAL("failed to initialize the RTGI history ({})", std::to_underlying(initialized.error()));
  }

  self.rtgi_render_size = render_size;
  self.rtgi_history_valid = false;
}

auto RendererInstance::record_rtgi(
  this RendererInstance& self,
  vir::Module& module,
  RenderContext& render_context,
  FrameRecording& recording,
  RTGIContext& context
) -> void {
  ZoneScoped;
  auto& prepared = recording.prepared;
  auto& values = recording.values;

  auto& variables = prepared.rtgi;
  variables.is_reset = module.declare_bool_var("rtgi reset", false);
  variables.half_res_depth = module.declare_image_var(
    "rtgi half res depth",
    vir::Format::R32Sfloat,
    RTGIContext::HISTORY_REST
  );
  variables.previous_half_res_depth = module.declare_image_var(
    "rtgi previous half res depth",
    vir::Format::R32Sfloat,
    RTGIContext::HISTORY_REST
  );
  variables.half_res_face_normal = module.declare_image_var(
    "rtgi half res face normal",
    vir::Format::R32Uint,
    RTGIContext::HISTORY_REST
  );
  variables.previous_half_res_face_normal = module.declare_image_var(
    "rtgi previous half res face normal",
    vir::Format::R32Uint,
    RTGIContext::HISTORY_REST
  );
  variables.half_res_normal_roughness = module.declare_image_var(
    "rtgi half res normal roughness",
    vir::Format::R32Uint,
    RTGIContext::HISTORY_REST
  );
  variables.previous_half_res_normal_roughness = module.declare_image_var(
    "rtgi previous half res normal roughness",
    vir::Format::R32Uint,
    RTGIContext::HISTORY_REST
  );
  variables.settings = module.declare_bytes_var("rtgi settings", sizeof(GPU::RTGISettings));

  context.half_res_depth = variables.half_res_depth;
  context.half_res_face_normal = variables.half_res_face_normal;
  context.half_res_normal_roughness = variables.half_res_normal_roughness;

  // after a reset a depth of 0 (sky) and a count of 0 make every pixel look disoccluded
  std::tie(
    context.previous_half_res_depth,
    context.previous_half_res_face_normal,
    context.previous_half_res_normal_roughness
  ) = std::tuple_cat( //
    module.set_condition(
      variables.is_reset,
      [&](vir::Module& arm) {
        const auto black = vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 0.0f);
        const auto zero = vir::ClearValue::rgba_u32(0, 0, 0, 0);
        return std::array{
          arm.clear(variables.previous_half_res_depth, black),
          arm.clear(variables.previous_half_res_face_normal, zero),
          arm.clear(variables.previous_half_res_normal_roughness, zero),
        };
      },
      [&](vir::Module&) {
        return std::array{
          variables.previous_half_res_depth,
          variables.previous_half_res_face_normal,
          variables.previous_half_res_normal_roughness,
        };
      }
    )
  );

  const auto& shape = recording.shape;
  const auto half_extent = rtgi_half_extent(shape.render_size);
  context.half_res_albedo_metalness = module.transient_image(
    {.extent = half_extent, .format = vir::Format::RGBA8Unorm, .name = "rtgi half res albedo metalness"}
  );

  std::tie(
    values.camera,
    context.depth_attachment,
    context.albedo_attachment,
    context.normal_attachment,
    context.metallic_roughness_occlusion_attachment,
    context.half_res_depth,
    context.half_res_face_normal,
    context.half_res_normal_roughness,
    context.half_res_albedo_metalness
  ) = std::tuple_cat( //
    module
      .begin_compute(
        {{values.camera, vir::Access::ComputeUniformRead},
         {context.depth_attachment, vir::Access::ComputeSampled},
         {context.albedo_attachment, vir::Access::ComputeSampled},
         {context.normal_attachment, vir::Access::ComputeSampled},
         {context.metallic_roughness_occlusion_attachment, vir::Access::ComputeSampled},
         {context.half_res_depth, vir::Access::ComputeWrite},
         {context.half_res_face_normal, vir::Access::ComputeWrite},
         {context.half_res_normal_roughness, vir::Access::ComputeWrite},
         {context.half_res_albedo_metalness, vir::Access::ComputeWrite}}
      )
      .with_name("rtgi gbuffer")
      .bind_compute_pipeline(render_context.pipeline("rtgi_gbuffer"))
      .bind_buffer(0, 0, values.camera)
      .bind_image(0, 1, context.depth_attachment)
      .bind_image(0, 2, context.albedo_attachment)
      .bind_image(0, 3, context.normal_attachment)
      .bind_image(0, 4, context.metallic_roughness_occlusion_attachment)
      .bind_image(0, 5, context.half_res_depth)
      .bind_image(0, 6, context.half_res_face_normal)
      .bind_image(0, 7, context.half_res_normal_roughness)
      .bind_image(0, 8, context.half_res_albedo_metalness)
      .push_constants(RTGIGBufferPush(shape.render_size))
      .dispatch_invocations(half_extent.width, half_extent.height, 1)
      .end_compute()
  );
}
} // namespace ox
