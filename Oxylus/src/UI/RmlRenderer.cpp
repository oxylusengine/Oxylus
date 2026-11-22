#include "UI/RmlRenderer.hpp"

#include <RmlUi/Core.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fmt/format.h>
#include <vir/Program.hpp>
#include <vir/RenderGraph.hpp>
#include <vir/Types.hpp>

#include "Asset/AssetFile.hpp"
#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Memory/SlotMap.hpp"
#include "Memory/Stack.hpp"
#include "Render/RenderContext.hpp"
#include "Render/Utils/Samplers.hpp"
#include "Render/Utils/TextureFormat.hpp"
#include "UI/RmlFileInterface.hpp"
#include "Utils/Log.hpp"

namespace ox {
constexpr static auto MAX_RML_PROGRAMS = 8_sz;
constexpr static auto RML_LAYER_SAMPLES = vir::Samples::e4;
// blur taps either side of the centre, a wider blur spaces them out instead
constexpr static auto RML_MAX_BLUR_TAPS = 32.0f;

struct RmlProgram {
  vir::Program program;
  vir::ValueID output = vir::ValueID::Invalid;
  ankerl::svector<vir::ValueID, 4> saved_textures = {};
};

struct RmlDrawPush {
  f32 transform[16] = {};
  u64 vertices = 0;
  u64 indices = 0;
  u64 gradient = 0;
  f32 translation[2] = {};
  u32 image_index = 0;
  u32 sampler_index = 0;
  u32 is_texture_srgb = 0;
  u32 is_texture_premultiplied = 0;
};

struct RmlPassthroughPush {
  glm::vec2 uv_offset = {0.0f, 0.0f};
  glm::vec2 uv_scale = {1.0f, 1.0f};
  f32 factor = 1.0f;
};

struct RmlColorMatrixPush {
  f32 color_matrix[16] = {};
};

struct RmlBlurPush {
  glm::vec2 texel = {};
  glm::vec2 uv_min = {};
  glm::vec2 uv_max = {};
  f32 sigma = 0.0f;
  i32 radius = 0;
  i32 step = 1;
};

struct RmlDropShadowPush {
  glm::vec4 color = {};
  glm::vec2 uv_offset = {};
  glm::vec2 uv_min = {};
  glm::vec2 uv_max = {};
};

// premultiplied alpha
constexpr static auto RML_BLEND = vir::ColorBlendAttachmentState{
  .is_blend_enabled = true,
  .src_color_blend_factor = VK_BLEND_FACTOR_ONE,
  .dst_color_blend_factor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
  .color_blend_op = VK_BLEND_OP_ADD,
  .src_alpha_blend_factor = VK_BLEND_FACTOR_ONE,
  .dst_alpha_blend_factor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
  .alpha_blend_op = VK_BLEND_OP_ADD,
};
constexpr static auto RML_REPLACE = vir::ColorBlendAttachmentState{.is_blend_enabled = false};

static auto stencil_test_state(u8 reference) -> vir::StencilState {
  const auto op = VkStencilOpState{
    .failOp = VK_STENCIL_OP_KEEP,
    .passOp = VK_STENCIL_OP_KEEP,
    .depthFailOp = VK_STENCIL_OP_KEEP,
    .compareOp = VK_COMPARE_OP_EQUAL,
    .compareMask = 0xFF,
    .writeMask = 0,
    .reference = reference,
  };

  return {.is_test_enabled = true, .front = op, .back = op};
}

static auto stencil_write_state(RmlClipWrite write, u8 stencil) -> vir::StencilState {
  const auto is_increment = write == RmlClipWrite::Increment;
  const auto op = VkStencilOpState{
    .failOp = VK_STENCIL_OP_KEEP,
    .passOp = is_increment ? VK_STENCIL_OP_INCREMENT_AND_CLAMP : VK_STENCIL_OP_REPLACE,
    .depthFailOp = VK_STENCIL_OP_KEEP,
    .compareOp = is_increment ? VK_COMPARE_OP_EQUAL : VK_COMPARE_OP_ALWAYS,
    .compareMask = 0xFF,
    .writeMask = 0xFF,
    .reference = stencil,
  };

  return {.is_test_enabled = true, .front = op, .back = op};
}

static auto to_rect(glm::ivec4 scissor) -> VkRect2D {
  return {
    .offset = {scissor.x, scissor.y},
    .extent = {static_cast<u32>(scissor.z), static_cast<u32>(scissor.w)},
  };
}

// half a texel inside the scissor, so bilinear taps never pull in what lies outside it
static auto region_uv(glm::ivec4 scissor, glm::ivec2 extent) -> std::pair<glm::vec2, glm::vec2> {
  const auto size = glm::vec2(extent);
  const auto min = (glm::vec2(scissor.x, scissor.y) + 0.5f) / size;
  const auto max = (glm::vec2(scissor.x + scissor.z, scissor.y + scissor.w) - 0.5f) / size;

  return {min, max};
}

static auto blur_push(f32 sigma, glm::vec2 direction, glm::ivec4 scissor, glm::ivec2 extent) -> RmlBlurPush {
  const auto [uv_min, uv_max] = region_uv(scissor, extent);
  auto push = RmlBlurPush{
    .texel = direction / glm::vec2(extent),
    .uv_min = uv_min,
    .uv_max = uv_max,
    .sigma = sigma,
  };
  if (sigma < 0.1f) {
    return push;
  }

  const auto reach = std::ceil(3.0f * sigma);
  push.step = static_cast<i32>(std::max(1.0f, std::ceil(reach / RML_MAX_BLUR_TAPS)));
  push.radius = static_cast<i32>(std::ceil(reach / static_cast<f32>(push.step)));

  return push;
}

static auto to_color(Rml::ColourbPremultiplied color) -> glm::vec4 {
  return glm::vec4(color.red, color.green, color.blue, color.alpha) / 255.0f;
}

auto rml_projection(const f32 width, const f32 height) -> Rml::Matrix4f {
  // RmlUi's ortho is GL's, Vulkan's clip space has y pointing down and depth in 0..1
  const auto correction = Rml::Matrix4f::FromColumns(
    {1.0f, 0.0f, 0.0f, 0.0f},
    {0.0f, -1.0f, 0.0f, 0.0f},
    {0.0f, 0.0f, 0.5f, 0.0f},
    {0.0f, 0.0f, 0.5f, 1.0f}
  );

  return correction * Rml::Matrix4f::ProjectOrtho(0.0f, width, height, 0.0f, -10000.0f, 10000.0f);
}

auto RmlSignatureHash::operator()(const std::vector<u32>& signature) const noexcept -> u64 {
  return ankerl::unordered_dense::detail::wyhash::hash(signature.data(), signature.size() * sizeof(u32));
}

RmlRenderer::RmlRenderer() {
  ZoneScoped;

  const u8 white_pixel[] = {0xFF, 0xFF, 0xFF, 0xFF};
  this->white_texture = Texture::create({
    .format = vir::Format::RGBA8Unorm,
    .extent = {1, 1, 1},
    .usage = vir::ImageUsageFlags::Sampled,
  });
  this->white_texture.upload(white_pixel, vir::Access::FragmentSampled);

  // the scene binds it every frame, drawn into or not
  this->begin_frame({1, 1});
}

RmlRenderer::~RmlRenderer() {
  // RmlUi releases everything it compiled through `Rml::ReleaseRenderManagers`, this is only what it didn't
  auto& context = App::get_rendercontext();
  this->compiled_geometries.for_each_active_id([&context](RmlGeometryID, RmlCompiledGeometry& geometry) {
    context.destroy_buffer(geometry.buffer);
  });
}

auto RmlRenderer::begin_frame(this RmlRenderer& self, glm::ivec2 extent) -> void {
  ZoneScoped;

  self.builder.begin(extent);
  self.frame_gradients.clear();
  self.current_transform = Rml::Matrix4f::Identity();

  const auto output_extent = vir::Extent3D{static_cast<u32>(extent.x), static_cast<u32>(extent.y), 1};
  if (self.output && self.output.get_extent() == output_extent) {
    return;
  }

  self.output = Texture::create({
    .format = vir::Format::RGBA8Unorm,
    .extent = output_extent,
    .usage = vir::ImageUsageFlags::Sampled | vir::ImageUsageFlags::ColorAttachment | vir::ImageUsageFlags::TransferDst,
  });
  self.output.set_name("rmlui output");
  if (!self.output.clear(vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 0.0f), vir::Access::FragmentSampled)) {
    OX_LOG_ERROR("failed to clear the rmlui output");
  }
}

auto RmlRenderer::render(this RmlRenderer& self, RenderContext& context) -> void {
  ZoneScoped;

  self.uploaded = std::move(self.builder.plan);
  self.builder.plan = {};
  self.is_visible = false;

  const auto& plan = self.uploaded;
  if (plan.passes.empty() || plan.extent.x <= 0 || plan.extent.y <= 0) {
    return;
  }

  if (!self.frame_gradients.empty()) {
    auto gradients = context.upload_frame_buffer(
      std::span(reinterpret_cast<const u8*>(self.frame_gradients.data()), ox::size_bytes(self.frame_gradients)),
      "rmlui gradients"
    );
    if (!gradients) {
      OX_LOG_ERROR("failed to stage the rmlui gradients");
      return;
    }

    self.gradient_buffer = *gradients;
  }

  auto* program = self.program_for(context);
  if (!program) {
    return;
  }

  program->program.set(program->output, self.output.attachment_info());
  for (usize i = 0; i < program->saved_textures.size(); i++) {
    const auto* texture = self.loaded_textures.slot(static_cast<RmlTextureID>(plan.saved_textures[i]));
    if (!texture) {
      OX_LOG_ERROR("rmlui saved a layer into a texture it already released");
      return;
    }

    program->program.set(program->saved_textures[i], texture->texture.attachment_info());
  }

  const auto ticket = context.execute(program->program);
  if (!ticket) {
    OX_LOG_ERROR("failed to render rmlui ({})", std::to_underlying(ticket.error()));
    return;
  }

  self.is_visible = true;
}

auto RmlRenderer::program_for(this RmlRenderer& self, RenderContext& context) -> RmlProgram* {
  ZoneScoped;

  self.uploaded.signature(self.signature);
  if (const auto it = self.programs.find(self.signature); it != self.programs.end()) {
    const auto order = std::ranges::find(self.program_order, self.signature);
    std::rotate(order, order + 1, self.program_order.end());
    return it->second.get();
  }

  auto program = std::make_unique<RmlProgram>();
  auto module = vir::Module{};
  self.record_program(module, context, *program);
  auto compiled = context.compile(std::move(module));
  if (!compiled) {
    OX_LOG_ERROR("failed to compile the rmlui program ({})", std::to_underlying(compiled.error()));
    return nullptr;
  }

  program->program = std::move(*compiled);
  auto* result = self.programs.emplace(self.signature, std::move(program)).first->second.get();
  self.program_order.push_back(self.signature);
  if (self.program_order.size() > MAX_RML_PROGRAMS) {
    self.programs.erase(self.program_order.front());
    self.program_order.erase(self.program_order.begin());
  }

  return result;
}

auto RmlRenderer::draw_callback(this RmlRenderer& self, const u32 pass_index, const u32 sampler_index)
  -> std::function<void(vir::Recorder&)> {
  return [&self, pass_index, sampler_index](vir::Recorder& recorder) -> void {
    const auto& plan = self.uploaded;
    const auto& pass = plan.passes[pass_index];
    const auto projection = rml_projection(static_cast<f32>(plan.extent.x), static_cast<f32>(plan.extent.y));

    for (const auto& cmd : std::span(plan.draws).subspan(pass.first_draw, pass.draw_count)) {
      recorder.set_scissor_absolute(to_rect(cmd.scissor));

      const auto* texture = cmd.texture ? self.loaded_textures.slot(static_cast<RmlTextureID>(cmd.texture)) : nullptr;
      auto push = RmlDrawPush{
        .vertices = cmd.vertices,
        .indices = cmd.indices,
        .gradient = cmd.gradient ? self.gradient_buffer.device_address + *cmd.gradient * sizeof(GPU::RmlGradient)
                                 : 0_u64,
        .translation = {cmd.translation.x, cmd.translation.y},
        .image_index = (texture ? texture->texture : self.white_texture).get_view_index(),
        .sampler_index = sampler_index,
        .is_texture_srgb = texture && texture->texture.is_srgb() ? 1_u32 : 0_u32,
        .is_texture_premultiplied = texture && texture->is_premultiplied ? 1_u32 : 0_u32,
      };
      const auto transform = projection * cmd.transform;
      std::memcpy(push.transform, transform.data(), sizeof(push.transform));
      // the vertex shader reads the indices itself, so a draw needs no index buffer of its own bound
      recorder.push_constants(push).draw(cmd.index_count, 1);
    }
  };
}

auto RmlRenderer::record_program(
  this RmlRenderer& self, vir::Module& module, RenderContext& context, RmlProgram& program
) -> void {
  ZoneScoped;

  const auto& plan = self.uploaded;
  const auto extent = vir::Extent3D{static_cast<u32>(plan.extent.x), static_cast<u32>(plan.extent.y), 1};
  const auto linear_sampler = context.cached_sampler_handle(LinearSamplerClamped);
  const auto sampler_index = SlotMap_decode_id(context.cached_sampler({
                                                 .mag_filter = VK_FILTER_LINEAR,
                                                 .min_filter = VK_FILTER_LINEAR,
                                                 .mipmap_mode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
                                                 .address_mode_u = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                                 .address_mode_v = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                                 .address_mode_w = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                               }))
                               .index;

  program.output = module.declare_image_var("rmlui output", vir::Format::RGBA8Unorm, vir::Access::FragmentSampled);
  auto saved = ankerl::svector<vir::ValueID, 4>{};
  for (usize i = 0; i < plan.saved_textures.size(); i++) {
    const auto name = fmt::format("rmlui saved texture {}", i);
    program.saved_textures.push_back(
      module.declare_image_var(name, vir::Format::RGBA8Unorm, vir::Access::FragmentSampled)
    );
    saved.push_back(program.saved_textures.back());
  }

  auto layers = ankerl::svector<vir::ValueID, 8>(plan.layer_count, vir::ValueID::Invalid);
  layers[0] = module.clear(
    module.transient_image({
      .extent = extent,
      .format = vir::Format::RGBA8Unorm,
      .samples = RML_LAYER_SAMPLES,
      .name = "rmlui layer",
    }),
    vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 0.0f)
  );

  auto stencil = vir::ValueID::Invalid;
  if (plan.uses_stencil) {
    stencil = module.clear(
      module.transient_image({
        .extent = extent,
        .format = vir::Format::D32SfloatS8Uint,
        .samples = RML_LAYER_SAMPLES,
        .name = "rmlui clip mask",
      }),
      vir::ClearValue::depth_stencil(0.0f, 0)
    );
  }

  auto masks = ankerl::svector<vir::ValueID, 2>(plan.mask_count, vir::ValueID::Invalid);

  for (u32 pass_index = 0; pass_index < plan.passes.size(); pass_index++) {
    const auto& pass = plan.passes[pass_index];
    const auto layer = static_cast<usize>(pass.layer);

    switch (pass.kind) {
      case RmlPassKind::Draw: {
        auto accesses = ankerl::svector<vir::ResourceAccess, 8>{{layers[layer], vir::Access::ColorRW}};
        if (pass.stencil != 0) {
          accesses.push_back({stencil, vir::Access::DepthStencilRead});
        }

        for (const auto slot : std::span(plan.texture_reads).subspan(pass.first_read, pass.read_count)) {
          accesses.push_back({saved[static_cast<usize>(slot)], vir::Access::FragmentSampled});
        }

        const auto draws = module.declare_callback_var("rmlui draws", self.draw_callback(pass_index, sampler_index));
        module.begin_rendering(std::span<const vir::ResourceAccess>(accesses.data(), accesses.size()))
          .with_name("rmlui")
          .bind_graphics_pipeline(context.pipeline("rmlui"))
          .set_color_blend(0, RML_BLEND)
          .set_viewport(0, vir::Rect2D::framebuffer())
          .set_scissor(0, vir::Rect2D::framebuffer());
        if (pass.stencil != 0) {
          module.set_stencil(stencil_test_state(pass.stencil));
        }

        const auto results = module.record_from(draws).end_rendering_values();
        layers[layer] = results[0];
        auto next = 1_sz;
        if (pass.stencil != 0) {
          stencil = results[next++];
        }

        for (const auto slot : std::span(plan.texture_reads).subspan(pass.first_read, pass.read_count)) {
          saved[static_cast<usize>(slot)] = results[next++];
        }
      } break;
      case RmlPassKind::ClipClear: {
        stencil = module.clear(stencil, vir::ClearValue::depth_stencil(0.0f, pass.stencil));
      } break;
      case RmlPassKind::ClipDraw: {
        const auto draws = module.declare_callback_var("rmlui clip", self.draw_callback(pass_index, sampler_index));
        stencil = module.begin_rendering({{stencil, vir::Access::DepthStencilRW}})
                    .with_name("rmlui clip mask")
                    .bind_graphics_pipeline(context.pipeline("rmlui_clip"))
                    .set_stencil(stencil_write_state(pass.clip_write, pass.stencil))
                    .set_viewport(0, vir::Rect2D::framebuffer())
                    .set_scissor(0, vir::Rect2D::framebuffer())
                    .record_from(draws)
                    .end_rendering()[0];
      } break;
      case RmlPassKind::PushLayer: {
        layers[layer] = module.clear(
          module.transient_image({
            .extent = extent,
            .format = vir::Format::RGBA8Unorm,
            .samples = RML_LAYER_SAMPLES,
            .name = "rmlui layer",
          }),
          vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 0.0f)
        );
      } break;
      case RmlPassKind::Composite: {
        auto current = module.resolve(
          layers[static_cast<usize>(pass.source)],
          module.transient_image({.extent = extent, .format = vir::Format::RGBA8Unorm, .name = "rmlui filter source"})
        );

        for (u32 filter_index = pass.first_filter; filter_index < pass.first_filter + pass.filter_count;
             filter_index++) {
          const auto& filter = plan.filters[filter_index];
          switch (filter.kind) {
            case RmlFilterKind::Opacity: {
              const auto body = module.declare_callback_var(
                "rmlui opacity",
                [&self, pass_index, filter_index](vir::Recorder& recorder) -> void {
                  const auto& frame = self.uploaded;
                  recorder.set_scissor_absolute(to_rect(frame.passes[pass_index].scissor));
                  recorder.push_constants(RmlPassthroughPush{.factor = frame.filters[filter_index].value}).draw(3, 1);
                }
              );
              current = module
                          .begin_rendering(
                            {{module.transient_image(
                                {.extent = extent, .format = vir::Format::RGBA8Unorm, .name = "rmlui opacity"}
                              ),
                              vir::Access::ColorWrite},
                             {current, vir::Access::FragmentSampled}}
                          )
                          .with_name("rmlui opacity")
                          .bind_graphics_pipeline(context.pipeline("rmlui_passthrough"))
                          .set_color_blend(0, RML_REPLACE)
                          .set_viewport(0, vir::Rect2D::framebuffer())
                          .set_scissor(0, vir::Rect2D::framebuffer())
                          .bind_image(0, 0, current)
                          .bind_sampler(0, 1, linear_sampler)
                          .record_from(body)
                          .end_rendering()[0];
            } break;
            case RmlFilterKind::ColorMatrix: {
              const auto body = module.declare_callback_var(
                "rmlui color matrix",
                [&self, pass_index, filter_index](vir::Recorder& recorder) -> void {
                  const auto& frame = self.uploaded;
                  auto push = RmlColorMatrixPush{};
                  std::memcpy(push.color_matrix, frame.filters[filter_index].color_matrix.data(), sizeof(push));
                  recorder.set_scissor_absolute(to_rect(frame.passes[pass_index].scissor));
                  recorder.push_constants(push).draw(3, 1);
                }
              );
              current = module
                          .begin_rendering(
                            {{module.transient_image(
                                {.extent = extent, .format = vir::Format::RGBA8Unorm, .name = "rmlui color matrix"}
                              ),
                              vir::Access::ColorWrite},
                             {current, vir::Access::FragmentSampled}}
                          )
                          .with_name("rmlui color matrix")
                          .bind_graphics_pipeline(context.pipeline("rmlui_color_matrix"))
                          .set_color_blend(0, RML_REPLACE)
                          .set_viewport(0, vir::Rect2D::framebuffer())
                          .set_scissor(0, vir::Rect2D::framebuffer())
                          .bind_image(0, 0, current)
                          .bind_sampler(0, 1, linear_sampler)
                          .record_from(body)
                          .end_rendering()[0];
            } break;
            case RmlFilterKind::MaskImage: {
              const auto mask = static_cast<usize>(filter.mask);
              if (mask >= masks.size() || masks[mask] == vir::ValueID::Invalid) {
                break;
              }

              const auto body = module.declare_callback_var(
                "rmlui mask",
                [&self, pass_index](vir::Recorder& recorder) -> void {
                  recorder.set_scissor_absolute(to_rect(self.uploaded.passes[pass_index].scissor));
                  recorder.draw(3, 1);
                }
              );
              const auto masked = module
                                    .begin_rendering(
                                      {{module.transient_image(
                                          {.extent = extent, .format = vir::Format::RGBA8Unorm, .name = "rmlui masked"}
                                        ),
                                        vir::Access::ColorWrite},
                                       {current, vir::Access::FragmentSampled},
                                       {masks[mask], vir::Access::FragmentSampled}}
                                    )
                                    .with_name("rmlui mask")
                                    .bind_graphics_pipeline(context.pipeline("rmlui_mask"))
                                    .set_color_blend(0, RML_REPLACE)
                                    .set_viewport(0, vir::Rect2D::framebuffer())
                                    .set_scissor(0, vir::Rect2D::framebuffer())
                                    .bind_image(0, 0, current)
                                    .bind_image(0, 1, masks[mask])
                                    .bind_sampler(0, 2, linear_sampler)
                                    .record_from(body)
                                    .end_rendering();
              current = masked[0];
              masks[mask] = masked[2];
            } break;
            case RmlFilterKind::Blur      :
            case RmlFilterKind::DropShadow: {
              const auto is_shadow = filter.kind == RmlFilterKind::DropShadow;
              const auto source = current;
              auto blurred = source;
              if (is_shadow) {
                const auto body = module.declare_callback_var(
                  "rmlui drop shadow",
                  [&self, pass_index, filter_index](vir::Recorder& recorder) -> void {
                    const auto& frame = self.uploaded;
                    const auto& scissor = frame.passes[pass_index].scissor;
                    const auto& shadow = frame.filters[filter_index];
                    const auto [uv_min, uv_max] = region_uv(scissor, frame.extent);
                    recorder.set_scissor_absolute(to_rect(scissor));
                    recorder
                      .push_constants(
                        RmlDropShadowPush{
                          .color = shadow.color,
                          .uv_offset = shadow.offset / glm::vec2(frame.extent),
                          .uv_min = uv_min,
                          .uv_max = uv_max,
                        }
                      )
                      .draw(3, 1);
                  }
                );
                blurred = module
                            .begin_rendering(
                              {{module.transient_image(
                                  {.extent = extent, .format = vir::Format::RGBA8Unorm, .name = "rmlui drop shadow"}
                                ),
                                vir::Access::ColorWrite},
                               {source, vir::Access::FragmentSampled}}
                            )
                            .with_name("rmlui drop shadow")
                            .bind_graphics_pipeline(context.pipeline("rmlui_drop_shadow"))
                            .set_color_blend(0, RML_REPLACE)
                            .set_viewport(0, vir::Rect2D::framebuffer())
                            .set_scissor(0, vir::Rect2D::framebuffer())
                            .bind_image(0, 0, source)
                            .bind_sampler(0, 1, linear_sampler)
                            .record_from(body)
                            .end_rendering()[0];
              }

              if (filter.is_blurred()) {
                for (const auto direction : {glm::vec2(1.0f, 0.0f), glm::vec2(0.0f, 1.0f)}) {
                  const auto body = module.declare_callback_var(
                    "rmlui blur",
                    [&self, pass_index, filter_index, direction](vir::Recorder& recorder) -> void {
                      const auto& frame = self.uploaded;
                      const auto& scissor = frame.passes[pass_index].scissor;
                      recorder.set_scissor_absolute(to_rect(scissor));
                      recorder
                        .push_constants(blur_push(frame.filters[filter_index].value, direction, scissor, frame.extent))
                        .draw(3, 1);
                    }
                  );
                  blurred = module
                              .begin_rendering(
                                {{module.transient_image(
                                    {.extent = extent, .format = vir::Format::RGBA8Unorm, .name = "rmlui blur"}
                                  ),
                                  vir::Access::ColorWrite},
                                 {blurred, vir::Access::FragmentSampled}}
                              )
                              .with_name("rmlui blur")
                              .bind_graphics_pipeline(context.pipeline("rmlui_blur"))
                              .set_color_blend(0, RML_REPLACE)
                              .set_viewport(0, vir::Rect2D::framebuffer())
                              .set_scissor(0, vir::Rect2D::framebuffer())
                              .bind_image(0, 0, blurred)
                              .bind_sampler(0, 1, linear_sampler)
                              .record_from(body)
                              .end_rendering()[0];
                }
              }

              if (is_shadow) {
                // the element over its own shadow
                const auto body = module.declare_callback_var(
                  "rmlui drop shadow source",
                  [&self, pass_index](vir::Recorder& recorder) -> void {
                    recorder.set_scissor_absolute(to_rect(self.uploaded.passes[pass_index].scissor));
                    recorder.push_constants(RmlPassthroughPush{}).draw(3, 1);
                  }
                );
                blurred = module
                            .begin_rendering({{blurred, vir::Access::ColorRW}, {source, vir::Access::FragmentSampled}})
                            .with_name("rmlui drop shadow source")
                            .bind_graphics_pipeline(context.pipeline("rmlui_passthrough"))
                            .set_color_blend(0, RML_BLEND)
                            .set_viewport(0, vir::Rect2D::framebuffer())
                            .set_scissor(0, vir::Rect2D::framebuffer())
                            .bind_image(0, 0, source)
                            .bind_sampler(0, 1, linear_sampler)
                            .record_from(body)
                            .end_rendering()[0];
              }

              current = blurred;
            } break;
          }
        }

        const auto body = module.declare_callback_var(
          "rmlui composite",
          [&self, pass_index](vir::Recorder& recorder) -> void {
            recorder.set_scissor_absolute(to_rect(self.uploaded.passes[pass_index].scissor));
            recorder.push_constants(RmlPassthroughPush{}).draw(3, 1);
          }
        );
        const auto destination = static_cast<usize>(pass.layer);
        auto accesses = ankerl::svector<vir::ResourceAccess, 3>{
          {layers[destination], vir::Access::ColorRW},
          {current, vir::Access::FragmentSampled},
        };
        if (pass.stencil != 0) {
          accesses.push_back({stencil, vir::Access::DepthStencilRead});
        }

        module.begin_rendering(std::span<const vir::ResourceAccess>(accesses.data(), accesses.size()))
          .with_name("rmlui composite layers")
          .bind_graphics_pipeline(context.pipeline("rmlui_passthrough"))
          .set_color_blend(0, pass.blend_mode == Rml::BlendMode::Replace ? RML_REPLACE : RML_BLEND)
          .set_viewport(0, vir::Rect2D::framebuffer())
          .set_scissor(0, vir::Rect2D::framebuffer())
          .bind_image(0, 0, current)
          .bind_sampler(0, 1, linear_sampler);
        if (pass.stencil != 0) {
          module.set_stencil(stencil_test_state(pass.stencil));
        }

        const auto results = module.record_from(body).end_rendering_values();
        layers[destination] = results[0];
        if (pass.stencil != 0) {
          stencil = results[2];
        }
      } break;
      case RmlPassKind::SaveTexture: {
        const auto resolved = module.resolve(
          layers[layer],
          module.transient_image({.extent = extent, .format = vir::Format::RGBA8Unorm, .name = "rmlui saved layer"})
        );
        const auto body = module.declare_callback_var(
          "rmlui save texture",
          [&self, pass_index](vir::Recorder& recorder) -> void {
            const auto& frame = self.uploaded;
            const auto& scissor = frame.passes[pass_index].scissor;
            const auto size = glm::vec2(frame.extent);
            recorder
              .push_constants(
                RmlPassthroughPush{
                  .uv_offset = glm::vec2(scissor.x, scissor.y) / size,
                  .uv_scale = glm::vec2(scissor.z, scissor.w) / size,
                }
              )
              .draw(3, 1);
          }
        );
        const auto slot = static_cast<usize>(pass.saved_texture);
        saved[slot] = module
                        .begin_rendering(
                          {{saved[slot], vir::Access::ColorWrite}, {resolved, vir::Access::FragmentSampled}}
                        )
                        .with_name("rmlui save texture")
                        .bind_graphics_pipeline(context.pipeline("rmlui_passthrough"))
                        .set_color_blend(0, RML_REPLACE)
                        .set_viewport(0, vir::Rect2D::framebuffer())
                        .set_scissor(0, vir::Rect2D::framebuffer())
                        .bind_image(0, 0, resolved)
                        .bind_sampler(0, 1, linear_sampler)
                        .record_from(body)
                        .end_rendering()[0];
      } break;
      case RmlPassKind::SaveMask: {
        masks[static_cast<usize>(pass.mask)] = module.resolve(
          layers[layer],
          module.transient_image({.extent = extent, .format = vir::Format::RGBA8Unorm, .name = "rmlui mask"})
        );
      } break;
    }
  }

  module.export_to(module.resolve(layers[0], program.output), vir::Access::FragmentSampled);
  for (const auto texture : saved) {
    module.export_to(texture, vir::Access::FragmentSampled);
  }
}

auto RmlRenderer::record_composite(
  vir::Module& module,
  RenderContext& render_context,
  vir::ValueID target,
  vir::Extent3D extent,
  RmlCompositeVariables& variables
) -> vir::ValueID {
  ZoneScoped;

  variables.image = module.declare_image_var("rmlui output", vir::Format::RGBA8Unorm, vir::Access::FragmentSampled);
  variables.is_visible = module.declare_bool_var("rmlui visible", false);

  const auto
    target_format = module.image_format(target).value_or(std::pair{vir::Format::RGBA8Srgb, vir::Samples::e1}).first;
  const auto is_target_srgb = to_unorm_format(target_format) != target_format;
  const auto linear_sampler = render_context.cached_sampler_handle(LinearSamplerClamped);

  struct CompositePush {
    u32 is_target_srgb = 0;
  };

  return module.set_condition(
    variables.is_visible,
    [&](vir::Module& arm) -> vir::ValueID {
      auto scene_copy = arm.transient_image({.extent = extent, .format = target_format, .name = "rmlui scene"});
      scene_copy = arm.copy_image(target, scene_copy);

      return arm
        .begin_rendering(
          {{target, vir::Access::ColorWrite},
           {scene_copy, vir::Access::FragmentSampled},
           {variables.image, vir::Access::FragmentSampled}}
        )
        .with_name("rmlui composite")
        .bind_graphics_pipeline(render_context.pipeline("rmlui_composite"))
        .set_viewport(0, vir::Rect2D::framebuffer())
        .set_scissor(0, vir::Rect2D::framebuffer())
        .bind_image(0, 0, scene_copy)
        .bind_image(0, 1, variables.image)
        .bind_sampler(0, 2, linear_sampler)
        .push_constants(CompositePush{.is_target_srgb = is_target_srgb ? 1_u32 : 0_u32})
        .draw(3, 1)
        .end_rendering()[0];
    },
    [&](vir::Module&) -> vir::ValueID { return target; }
  );
}

auto RmlRenderer::bind_composite(
  this const RmlRenderer& self, vir::Program& program, const RmlCompositeVariables& variables
) -> void {
  program.set(variables.image, self.output.attachment_info());
  program.set(variables.is_visible, self.is_visible);
}

auto RmlRenderer::stage_geometry(
  this RmlRenderer& self, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture
) -> option<RmlDrawCmd> {
  const auto* compiled = self.compiled_geometries.slot(static_cast<RmlGeometryID>(geometry));
  if (!compiled) {
    return nullopt;
  }

  return RmlDrawCmd{
    .vertices = compiled->vertices,
    .indices = compiled->indices,
    .index_count = compiled->index_count,
    .texture = texture,
    .translation = translation,
    .transform = self.current_transform,
  };
}

auto RmlRenderer::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices)
  -> Rml::CompiledGeometryHandle {
  ZoneScoped;

  if (vertices.empty() || indices.empty()) {
    return 0;
  }

  // RmlUi compiles a geometry once and draws it every frame until it changes, so it is written here and only here
  auto& context = App::get_rendercontext();
  const auto vertex_bytes = vertices.size() * sizeof(Rml::Vertex);
  const auto index_bytes = indices.size() * sizeof(i32);
  const auto buffer_id = context.allocate_buffer({
    .size = vertex_bytes + index_bytes,
    .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    .location = vir::MemoryLocation::CpuToGpu,
    .alignment = 16,
    .name = "rmlui geometry",
  });
  if (!buffer_id) {
    OX_LOG_ERROR("failed to allocate rmlui geometry ({})", std::to_underlying(buffer_id.error()));
    return 0;
  }

  const auto buffer = context.buffer(*buffer_id);
  const auto is_written =
    buffer.write_bytes(0, std::span(reinterpret_cast<const u8*>(vertices.data()), vertex_bytes)) &&
    buffer.write_bytes(vertex_bytes, std::span(reinterpret_cast<const u8*>(indices.data()), index_bytes)) &&
    context.persistent->flush_buffer(buffer);
  if (!is_written) {
    OX_LOG_ERROR("failed to write rmlui geometry");
    context.destroy_buffer(*buffer_id);
    return 0;
  }

  const auto id = this->compiled_geometries.create_slot({
    .buffer = *buffer_id,
    .vertices = buffer.device_address,
    .indices = buffer.device_address + vertex_bytes,
    .index_count = static_cast<u32>(indices.size()),
  });
  return static_cast<Rml::CompiledGeometryHandle>(id);
}

auto RmlRenderer::RenderGeometry(
  Rml::CompiledGeometryHandle geometry_handle, Rml::Vector2f translation, Rml::TextureHandle texture_handle
) -> void {
  ZoneScoped;

  if (auto cmd = this->stage_geometry(geometry_handle, translation, texture_handle)) {
    this->builder.draw(*cmd);
  }
}

auto RmlRenderer::ReleaseGeometry(Rml::CompiledGeometryHandle geometry) -> void {
  ZoneScoped;

  const auto id = static_cast<RmlGeometryID>(geometry);
  if (const auto* compiled = this->compiled_geometries.slot(id)) {
    // retired, so a draw still in flight keeps reading it
    App::get_rendercontext().destroy_buffer(compiled->buffer);
    this->compiled_geometries.destroy_slot(id);
  }
}

auto RmlRenderer::LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) -> Rml::TextureHandle {
  ZoneScoped;

  // joined against the document's own path by RmlUi, so as virtual as the document was. The source image while it is
  // there, its cooked pack once it isn't, as `RmlFileInterface` reads documents: a shipped game strips the image
  const auto& vfs = App::get_vfs();
  const auto path = resolve_rml_path(&vfs, source);
  auto texture = Texture{};
  if (std::filesystem::exists(path)) {
    texture = Texture::create({.source = path});
  } else if (App::has_mod<AssetManager>()) {
    const auto pack_path = find_rml_pack(App::mod<AssetManager>(), &vfs, source, {AssetType::Texture});
    if (auto pack = pack_path.empty() ? nullopt : AssetFile::unpack(pack_path)) {
      for (const auto& entry : pack->entries) {
        if (const auto* texture_data = std::get_if<TextureData>(&entry.data)) {
          texture = Texture::create(*texture_data, TextureLoadInfo{});
          break;
        }
      }
    }
  }

  if (!texture) {
    OX_LOG_WARN("RmlUi couldn't load the image '{}' ({}).", source, path);
    return 0;
  }

  texture_dimensions.x = texture.get_extent().width;
  texture_dimensions.y = texture.get_extent().height;

  auto id = this->loaded_textures.create_slot({.texture = std::move(texture)});
  return static_cast<Rml::TextureHandle>(id);
}

auto RmlRenderer::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions)
  -> Rml::TextureHandle {
  ZoneScoped;

  // already premultiplied, uploaded as it is rather than divided back out and multiplied again in the shader
  auto texture = Texture::create({
    .format = vir::Format::RGBA8Unorm,
    .extent = {static_cast<u32>(source_dimensions.x), static_cast<u32>(source_dimensions.y), 1_u32},
    .usage = vir::ImageUsageFlags::Sampled,
  });
  texture.upload(std::span(source.data(), source.size()), vir::Access::FragmentSampled);

  auto id = this->loaded_textures.create_slot({.texture = std::move(texture), .is_premultiplied = true});
  return static_cast<Rml::TextureHandle>(id);
}

auto RmlRenderer::ReleaseTexture(Rml::TextureHandle texture_handle) -> void {
  ZoneScoped;

  auto id = static_cast<RmlTextureID>(texture_handle);
  this->loaded_textures.destroy_slot(id);
}

auto RmlRenderer::EnableScissorRegion(bool enable) -> void { this->builder.set_scissor_enabled(enable); }

auto RmlRenderer::SetScissorRegion(Rml::Rectanglei region) -> void {
  this->builder.set_scissor({region.Left(), region.Top(), region.Width(), region.Height()});
}

auto RmlRenderer::SetTransform(const Rml::Matrix4f* transform) -> void {
  this->current_transform = transform ? *transform : Rml::Matrix4f::Identity();
}

auto RmlRenderer::EnableClipMask(bool enable) -> void { this->builder.set_clip_mask_enabled(enable); }

auto RmlRenderer::RenderToClipMask(
  Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation
) -> void {
  ZoneScoped;

  if (auto cmd = this->stage_geometry(geometry, translation, 0)) {
    this->builder.clip(operation, *cmd);
  }
}

auto RmlRenderer::PushLayer() -> Rml::LayerHandle { return static_cast<Rml::LayerHandle>(this->builder.push_layer()); }

auto RmlRenderer::CompositeLayers(
  Rml::LayerHandle source,
  Rml::LayerHandle destination,
  Rml::BlendMode blend_mode,
  Rml::Span<const Rml::CompiledFilterHandle> filters
) -> void {
  ZoneScoped;

  memory::ScopedStack stack;
  auto resolved = stack.alloc<RmlFilter>(filters.size());
  auto count = 0_sz;
  for (const auto handle : filters) {
    if (const auto* filter = this->compiled_filters.slot(static_cast<RmlFilterID>(handle))) {
      resolved[count++] = *filter;
    }
  }

  this->builder.composite(
    static_cast<RmlLayerID>(source),
    static_cast<RmlLayerID>(destination),
    blend_mode,
    resolved.first(count)
  );
}

auto RmlRenderer::PopLayer() -> void { this->builder.pop_layer(); }

auto RmlRenderer::SaveLayerAsTexture() -> Rml::TextureHandle {
  ZoneScoped;

  const auto scissor = this->builder.scissor();
  if (scissor.z <= 0 || scissor.w <= 0) {
    return 0;
  }

  auto texture = Texture::create({
    .format = vir::Format::RGBA8Unorm,
    .extent = {static_cast<u32>(scissor.z), static_cast<u32>(scissor.w), 1},
    .usage = vir::ImageUsageFlags::Sampled | vir::ImageUsageFlags::ColorAttachment | vir::ImageUsageFlags::TransferDst,
  });
  // the program that draws into it expects it at rest
  if (!texture.clear(vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 0.0f), vir::Access::FragmentSampled)) {
    OX_LOG_ERROR("failed to clear a texture for rmlui to save a layer into");
    return 0;
  }

  const auto handle = static_cast<Rml::TextureHandle>(
    this->loaded_textures.create_slot({.texture = std::move(texture), .is_premultiplied = true})
  );
  this->builder.save_texture(handle);

  return handle;
}

auto RmlRenderer::SaveLayerAsMaskImage() -> Rml::CompiledFilterHandle {
  ZoneScoped;

  const auto mask = this->builder.save_mask();
  const auto id = this->compiled_filters.create_slot(RmlFilter{.kind = RmlFilterKind::MaskImage, .mask = mask});

  return static_cast<Rml::CompiledFilterHandle>(id);
}

auto RmlRenderer::CompileFilter(const Rml::String& name, const Rml::Dictionary& parameters)
  -> Rml::CompiledFilterHandle {
  ZoneScoped;

  // the colour matrices are RmlUi's GL3 backend's, which take them from the filter effects spec
  auto filter = RmlFilter{};
  const auto value = Rml::Get(parameters, "value", 1.0f);
  if (name == "opacity") {
    filter.kind = RmlFilterKind::Opacity;
    filter.value = value;
  } else if (name == "blur") {
    filter.kind = RmlFilterKind::Blur;
    filter.value = Rml::Get(parameters, "sigma", 1.0f);
  } else if (name == "drop-shadow") {
    filter.kind = RmlFilterKind::DropShadow;
    filter.value = Rml::Get(parameters, "sigma", 0.0f);
    filter.color = to_color(Rml::Get(parameters, "color", Rml::Colourb()).ToPremultiplied());
    const auto offset = Rml::Get(parameters, "offset", Rml::Vector2f(0.0f));
    filter.offset = {offset.x, offset.y};
  } else if (name == "brightness") {
    filter.kind = RmlFilterKind::ColorMatrix;
    filter.color_matrix = Rml::Matrix4f::Diag(value, value, value, 1.0f);
  } else if (name == "contrast") {
    filter.kind = RmlFilterKind::ColorMatrix;
    const auto grayness = 0.5f - 0.5f * value;
    filter.color_matrix = Rml::Matrix4f::Diag(value, value, value, 1.0f);
    filter.color_matrix.SetColumn(3, Rml::Vector4f(grayness, grayness, grayness, 1.0f));
  } else if (name == "invert") {
    filter.kind = RmlFilterKind::ColorMatrix;
    const auto amount = std::clamp(value, 0.0f, 1.0f);
    const auto inverted = 1.0f - 2.0f * amount;
    filter.color_matrix = Rml::Matrix4f::Diag(inverted, inverted, inverted, 1.0f);
    filter.color_matrix.SetColumn(3, Rml::Vector4f(amount, amount, amount, 1.0f));
  } else if (name == "grayscale") {
    filter.kind = RmlFilterKind::ColorMatrix;
    const auto rest = 1.0f - value;
    const auto gray = value * Rml::Vector3f(0.2126f, 0.7152f, 0.0722f);
    filter.color_matrix = Rml::Matrix4f::FromRows(
      {gray.x + rest, gray.y, gray.z, 0.0f},
      {gray.x, gray.y + rest, gray.z, 0.0f},
      {gray.x, gray.y, gray.z + rest, 0.0f},
      {0.0f, 0.0f, 0.0f, 1.0f}
    );
  } else if (name == "sepia") {
    filter.kind = RmlFilterKind::ColorMatrix;
    const auto rest = 1.0f - value;
    const auto r = value * Rml::Vector3f(0.393f, 0.769f, 0.189f);
    const auto g = value * Rml::Vector3f(0.349f, 0.686f, 0.168f);
    const auto b = value * Rml::Vector3f(0.272f, 0.534f, 0.131f);
    filter.color_matrix = Rml::Matrix4f::FromRows(
      {r.x + rest, r.y, r.z, 0.0f},
      {g.x, g.y + rest, g.z, 0.0f},
      {b.x, b.y, b.z + rest, 0.0f},
      {0.0f, 0.0f, 0.0f, 1.0f}
    );
  } else if (name == "hue-rotate") {
    filter.kind = RmlFilterKind::ColorMatrix;
    const auto s = std::sin(value);
    const auto c = std::cos(value);
    filter.color_matrix = Rml::Matrix4f::FromRows(
      {0.213f + 0.787f * c - 0.213f * s, 0.715f - 0.715f * c - 0.715f * s, 0.072f - 0.072f * c + 0.928f * s, 0.0f},
      {0.213f - 0.213f * c + 0.143f * s, 0.715f + 0.285f * c + 0.140f * s, 0.072f - 0.072f * c - 0.283f * s, 0.0f},
      {0.213f - 0.213f * c - 0.787f * s, 0.715f - 0.715f * c + 0.715f * s, 0.072f + 0.928f * c + 0.072f * s, 0.0f},
      {0.0f, 0.0f, 0.0f, 1.0f}
    );
  } else if (name == "saturate") {
    filter.kind = RmlFilterKind::ColorMatrix;
    filter.color_matrix = Rml::Matrix4f::FromRows(
      {0.213f + 0.787f * value, 0.715f - 0.715f * value, 0.072f - 0.072f * value, 0.0f},
      {0.213f - 0.213f * value, 0.715f + 0.285f * value, 0.072f - 0.072f * value, 0.0f},
      {0.213f - 0.213f * value, 0.715f - 0.715f * value, 0.072f + 0.928f * value, 0.0f},
      {0.0f, 0.0f, 0.0f, 1.0f}
    );
  } else {
    OX_LOG_WARN("RmlUi filter '{}' isn't supported.", name);
    return 0;
  }

  return static_cast<Rml::CompiledFilterHandle>(this->compiled_filters.create_slot(std::move(filter)));
}

auto RmlRenderer::ReleaseFilter(Rml::CompiledFilterHandle filter) -> void {
  this->compiled_filters.destroy_slot(static_cast<RmlFilterID>(filter));
}

auto RmlRenderer::CompileShader(const Rml::String& name, const Rml::Dictionary& parameters)
  -> Rml::CompiledShaderHandle {
  ZoneScoped;

  const auto is_repeating = Rml::Get(parameters, "repeating", false);
  auto gradient = GPU::RmlGradient{};
  if (name == "linear-gradient") {
    gradient.function = is_repeating ? GPU::RmlGradientFunction::RepeatingLinear : GPU::RmlGradientFunction::Linear;
    const auto p0 = Rml::Get(parameters, "p0", Rml::Vector2f(0.0f));
    const auto p1 = Rml::Get(parameters, "p1", Rml::Vector2f(0.0f));
    gradient.p = {p0.x, p0.y};
    gradient.v = {p1.x - p0.x, p1.y - p0.y};
  } else if (name == "radial-gradient") {
    gradient.function = is_repeating ? GPU::RmlGradientFunction::RepeatingRadial : GPU::RmlGradientFunction::Radial;
    const auto center = Rml::Get(parameters, "center", Rml::Vector2f(0.0f));
    const auto radius = Rml::Get(parameters, "radius", Rml::Vector2f(1.0f));
    gradient.p = {center.x, center.y};
    gradient.v = {1.0f / radius.x, 1.0f / radius.y};
  } else if (name == "conic-gradient") {
    gradient.function = is_repeating ? GPU::RmlGradientFunction::RepeatingConic : GPU::RmlGradientFunction::Conic;
    const auto center = Rml::Get(parameters, "center", Rml::Vector2f(0.0f));
    const auto angle = Rml::Get(parameters, "angle", 0.0f);
    gradient.p = {center.x, center.y};
    gradient.v = {std::cos(angle), std::sin(angle)};
  } else {
    OX_LOG_WARN("RmlUi shader '{}' isn't supported.", name);
    return 0;
  }

  const auto it = parameters.find("color_stop_list");
  if (it == parameters.end() || it->second.GetType() != Rml::Variant::COLORSTOPLIST) {
    return 0;
  }

  const auto& stops = it->second.GetReference<Rml::ColorStopList>();
  gradient.stop_count = static_cast<u32>(std::min<usize>(stops.size(), GPU::RML_GRADIENT_MAX_STOPS));
  if (gradient.stop_count == 0) {
    return 0;
  }

  for (u32 i = 0; i < gradient.stop_count; i++) {
    gradient.stop_positions[i] = stops[i].position.number;
    gradient.stop_colors[i] = to_color(stops[i].color);
  }

  return static_cast<Rml::CompiledShaderHandle>(this->compiled_shaders.create_slot(std::move(gradient)));
}

auto RmlRenderer::RenderShader(
  Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle
) -> void {
  ZoneScoped;

  const auto* gradient = this->compiled_shaders.slot(static_cast<RmlShaderID>(shader));
  if (!gradient) {
    return;
  }

  if (auto cmd = this->stage_geometry(geometry, translation, 0)) {
    cmd->gradient = static_cast<u32>(this->frame_gradients.size());
    this->frame_gradients.push_back(*gradient);
    this->builder.draw(*cmd);
  }
}

auto RmlRenderer::ReleaseShader(Rml::CompiledShaderHandle shader) -> void {
  this->compiled_shaders.destroy_slot(static_cast<RmlShaderID>(shader));
}
} // namespace ox
