#include "UI/ImGuiRenderer.hpp"

#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>
#include <algorithm>
#include <cmath>
#include <glm/common.hpp>
#include <imgui_internal.h>
#include <vuk/RenderGraph.hpp>
#include <vuk/Types.hpp>
#include <vuk/runtime/CommandBuffer.hpp>
#include <vuk/runtime/vk/AllocatorHelpers.hpp>
#include <vuk/runtime/vk/Pipeline.hpp>
#include <vuk/vsl/Core.hpp>

#include "Core/App.hpp"
#include "ImGuiSPV_FS.hpp"
#include "ImGuiSPV_VS.hpp"
#include "Render/RenderContext.hpp"
#include "Render/Window.hpp"
#include "Utils/Profiler.hpp"

namespace ox {
// Shadow draws are injected into ImDrawData as command lists whose texture id is an index into
// `shadow_draw_data` biased by this value, so the render loop can tell them apart from real textures.
constexpr auto SHADOW_TEX_ID_BASE = ImTextureID{1} << 40;

static auto window_casts_shadow(const ImGuiWindow* window) -> bool {
  if (window == nullptr || !window->Active || window->Hidden)
    return false;
  if (window->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_NoBackground))
    return false;
  if (window->DockNode != nullptr)
    return false;

  return window->Size.x > 1.0f && window->Size.y > 1.0f;
}

static auto add_shadow_quad(ImDrawList* draw_list, glm::vec2 min, glm::vec2 max, glm::vec2 origin, ImU32 color)
  -> void {
  if (max.x <= min.x || max.y <= min.y)
    return;

  draw_list->PrimReserve(6, 4);

  const auto base = static_cast<ImDrawIdx>(draw_list->_VtxCurrentIdx);
  draw_list->PrimWriteIdx(base);
  draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1));
  draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2));
  draw_list->PrimWriteIdx(base);
  draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2));
  draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(base + 3));

  const auto local_min = min - origin;
  const auto local_max = max - origin;
  draw_list->PrimWriteVtx(ImVec2(min.x, min.y), ImVec2(local_min.x, local_min.y), color);
  draw_list->PrimWriteVtx(ImVec2(max.x, min.y), ImVec2(local_max.x, local_min.y), color);
  draw_list->PrimWriteVtx(ImVec2(max.x, max.y), ImVec2(local_max.x, local_max.y), color);
  draw_list->PrimWriteVtx(ImVec2(min.x, max.y), ImVec2(local_min.x, local_max.y), color);
}

// Emits the shadow as a ring around `hole`, which is the largest rect the caster is guaranteed to cover
// and where the cutout would zero the result anyway. Saves a lot of fill on large windows.
static auto add_shadow_ring(
  ImDrawList* draw_list, //
  glm::vec2 outer_min,
  glm::vec2 outer_max,
  glm::vec2 hole_min,
  glm::vec2 hole_max,
  glm::vec2 origin,
  ImU32 color
) -> void {
  hole_min = glm::clamp(hole_min, outer_min, outer_max);
  hole_max = glm::clamp(hole_max, outer_min, outer_max);

  if (hole_max.x - hole_min.x < 1.0f || hole_max.y - hole_min.y < 1.0f) {
    add_shadow_quad(draw_list, outer_min, outer_max, origin, color);
    return;
  }

  add_shadow_quad(draw_list, outer_min, {outer_max.x, hole_min.y}, origin, color);
  add_shadow_quad(draw_list, {outer_min.x, hole_max.y}, outer_max, origin, color);
  add_shadow_quad(draw_list, {outer_min.x, hole_min.y}, {hole_min.x, hole_max.y}, origin, color);
  add_shadow_quad(draw_list, {hole_max.x, hole_min.y}, {outer_max.x, hole_max.y}, origin, color);
}

ImFont* ImGuiRenderer::load_default_font() {
  ImGuiIO& io = ImGui::GetIO();
  return io.Fonts->AddFontDefault();
}

ImFont* ImGuiRenderer::load_font(const std::filesystem::path& path, f32 font_size, option<ImFontConfig> font_config) {
  ZoneScoped;

  auto path_str = path.string();
  ImGuiIO& io = ImGui::GetIO();
  if (font_config.has_value())
    return io.Fonts->AddFontFromFileTTF(path_str.c_str(), font_size, &*font_config);

  return io.Fonts->AddFontFromFileTTF(path_str.c_str(), font_size);
}

void ImGuiRenderer::build_fonts() {
  ZoneScoped;

  ImGuiIO& io = ImGui::GetIO();
  unsigned char* pixels_data;
  int width, height;
  io.Fonts->Build();
  io.Fonts->GetTexDataAsRGBA32(&pixels_data, &width, &height);
  font_texture = Texture::create({
    .format = vuk::Format::eR8G8B8A8Unorm,
    .extent = vuk::Extent3D{static_cast<u32>(width), static_cast<u32>(height), 1u},
    .usage = vuk::ImageUsageFlagBits::eSampled,
  });

  auto pixels = std::span{pixels_data, static_cast<usize>(width * height * 4)};
  font_texture.upload(pixels, vuk::eFragmentSampled);
}

auto ImGuiRenderer::init() -> std::expected<void, std::string> {
  ZoneScoped;
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard /*| ImGuiConfigFlags_ViewportsEnable*/ |
                    ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_IsSRGB;
  io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_HasMouseCursors;
  /*io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;*/
  io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
  io.BackendRendererName = "oxylus";
  io.Fonts->TexDesiredFormat = ImTextureFormat_RGBA32;

  io.ConfigDpiScaleFonts = false;
  io.ConfigDpiScaleViewports = false;
  io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
  ImGuiStyle& style = ImGui::GetStyle();
  style.FontScaleDpi = 1.0f;
  this->set_base_style(style);

  auto& runtime = *App::get_rendercontext().runtime;

  auto pipelie_ci = vuk::PipelineBaseCreateInfo{};
  pipelie_ci.add_spirv(
    std::vector<uint32_t>(
      embedded::imgui_vs_main_spirv,
      embedded::imgui_vs_main_spirv + embedded::imgui_vs_main_word_count
    ),
    {},
    "vs_main"
  );
  pipelie_ci.add_spirv(
    std::vector<uint32_t>(
      embedded::imgui_fs_main_spirv,
      embedded::imgui_fs_main_spirv + embedded::imgui_fs_main_word_count
    ),
    {},
    "fs_main"
  );
  runtime.create_named_pipeline("imgui", pipelie_ci);

  this->shadow_settings.layers = {
    ImGuiShadowLayer{.sigma = 22.0f, .spread = -4.0f, .offset = {0.0f, 10.0f}, .color = {0.0f, 0.0f, 0.0f, 0.75f}},
    ImGuiShadowLayer{.sigma = 5.0f, .spread = -1.0f, .offset = {0.0f, 2.0f}, .color = {0.0f, 0.0f, 0.0f, 0.65f}},
  };

  return {};
}

auto ImGuiRenderer::deinit() -> std::expected<void, std::string> {
  // Owned draw lists point into the context's shared draw data.
  shadow_draw_lists.clear();
  shadow_draw_data.clear();

  ImGui::DestroyContext();
  return {};
}

void ImGuiRenderer::begin_frame(const f64 delta_time, glm::vec2 logical_size, glm::vec2 real_size) {
  ZoneScoped;

  auto& imgui = ImGui::GetIO();
  imgui.DeltaTime = static_cast<f32>(delta_time);
  imgui.DisplaySize = ImVec2(logical_size.x, logical_size.y);
  imgui.DisplayFramebufferScale = ImVec2(
    logical_size.x > 0 ? (real_size.x / logical_size.x) : 1.0f,
    logical_size.y > 0 ? (real_size.y / logical_size.y) : 1.0f
  );

  this->apply_ui_scale();

  rendering_images.clear();
  acquired_images.clear();

  const auto routed = wants_keyboard();
  if (keyboard_routed_last_frame && !routed) {
    imgui.ClearInputKeys();
  }
  keyboard_routed_last_frame = routed;

  ImGui::NewFrame();
}

auto ImGuiRenderer::set_base_style(this ImGuiRenderer& self, const ImGuiStyle& style) -> void {
  self.base_style = style;
  self.base_style.FontScaleDpi = 1.0f;
  self.applied_ui_scale = 0.0f;
}

auto ImGuiRenderer::apply_ui_scale(this ImGuiRenderer& self) -> void {
  const auto ui_scale = App::get_ui_scale();
  ImGui::GetMainViewport()->DpiScale = ui_scale;

  if (std::abs(self.applied_ui_scale - ui_scale) <= 0.0001f) {
    return;
  }

  auto& style = ImGui::GetStyle();
  style = self.base_style;
  style.ScaleAllSizes(ui_scale);
  style.FontScaleDpi = ui_scale;
  self.applied_ui_scale = ui_scale;
}

vuk::Value<vuk::ImageAttachment> ImGuiRenderer::end_frame(
  RenderContext& context, vuk::Value<vuk::ImageAttachment> target
) {
  ZoneScoped;

  ImGui::Render();

  auto& window = App::get_window();
  const auto imgui_cursor = ImGui::GetMouseCursor();
  if (ImGui::GetIO().MouseDrawCursor || imgui_cursor == ImGuiMouseCursor_None) {
    window.show_cursor(false);
  } else {
    auto next_cursor = WindowCursor::Arrow;
    switch (imgui_cursor) {
      case ImGuiMouseCursor_Arrow     : next_cursor = WindowCursor::Arrow; break;
      case ImGuiMouseCursor_TextInput : next_cursor = WindowCursor::TextInput; break;
      case ImGuiMouseCursor_ResizeAll : next_cursor = WindowCursor::ResizeAll; break;
      case ImGuiMouseCursor_ResizeNS  : next_cursor = WindowCursor::ResizeNS; break;
      case ImGuiMouseCursor_ResizeEW  : next_cursor = WindowCursor::ResizeEW; break;
      case ImGuiMouseCursor_ResizeNESW: next_cursor = WindowCursor::ResizeNESW; break;
      case ImGuiMouseCursor_ResizeNWSE: next_cursor = WindowCursor::ResizeNWSE; break;
      case ImGuiMouseCursor_Hand      : next_cursor = WindowCursor::Hand; break;
      case ImGuiMouseCursor_NotAllowed: next_cursor = WindowCursor::NotAllowed; break;
      case ImGuiMouseCursor_Progress  : next_cursor = WindowCursor::Progress; break;
      case ImGuiMouseCursor_Wait      : next_cursor = WindowCursor::Wait; break;
      default                         : break;
    }
    window.show_cursor(true);

    if (window.get_cursor() != next_cursor) {
      window.set_cursor(next_cursor);
    }
  }

  ImDrawData* draw_data = ImGui::GetDrawData();

  build_window_shadows(draw_data);

  if (draw_data->Textures) {
    for (auto* texture : *draw_data->Textures) {
      auto acquired_image = vuk::Value<vuk::ImageAttachment>{};

#if 1
      if (texture->Status == ImTextureStatus_WantCreate || texture->Status == ImTextureStatus_WantUpdates) {
        if (font_texture) {
          font_texture.destroy();
        }

        font_texture = Texture::create({
          .format = vuk::Format::eR8G8B8A8Unorm,
          .extent = vuk::Extent3D{static_cast<u32>(texture->Width), static_cast<u32>(texture->Height), 1u},
          .usage = vuk::ImageUsageFlagBits::eSampled,
        });
        font_texture.set_name("imgui font texture");
        font_texture.upload(
          {static_cast<u8*>(texture->GetPixels()), static_cast<usize>(texture->GetSizeInBytes())},
          vuk::eFragmentSampled
        );

        texture->SetStatus(ImTextureStatus_OK);
      }

      auto texture_id = this->add_image(font_texture.view());
      OX_ASSERT(texture_id > 0);
      texture->SetTexID(texture_id);
#else
      auto acquired = false;
      auto upload_offset = vuk::Offset3D(texture->UpdateRect.x, texture->UpdateRect.y, 0);
      auto upload_extent = vuk::Extent3D(texture->UpdateRect.w, texture->UpdateRect.h, 1);

      switch (texture->Status) {
        case ImTextureStatus_WantCreate: {
          font_texture = std::make_shared<Texture>();
          font_texture->create(
            {},
            {.preset = Preset::eRTT2DUnmipped,
             .format = vuk::Format::eR8G8B8A8Unorm,
             .mime = {},
             .extent = vuk::Extent3D{static_cast<u32>(texture->Width), static_cast<u32>(texture->Height), 1u}}
          );
          font_texture->set_name("font_texture");

          acquired_image = font_texture->acquire({}, vuk::eNone);
          acquired = true;

          upload_offset = {};
          upload_extent = font_texture->get_extent();

          [[fallthrough]];
        }
        case ImTextureStatus_WantUpdates: {
          auto upload_pitch = upload_extent.width * texture->BytesPerPixel;
          auto upload_buffer = context.alloc_image_buffer(font_texture->get_format(), upload_extent);
          auto* buffer_ptr = reinterpret_cast<u8*>(upload_buffer->mapped_ptr);
          for (auto y = 0_u32; y < upload_extent.height; y++) {
            auto* pixels = static_cast<u8*>(
              texture->GetPixelsAt(upload_offset.x, upload_offset.y + static_cast<i32>(y))
            );
            std::memcpy(buffer_ptr + upload_pitch * y, pixels, upload_pitch);
          }

          auto upload_pass = vuk::make_pass(
            "upload",
            [upload_offset, upload_extent](
              vuk::CommandBuffer& cmd_list, //
              VUK_BA(vuk::eTransferRead) src,
              VUK_IA(vuk::eTransferWrite) dst
            ) {
              auto buffer_copy_region = vuk::BufferImageCopy{
                .bufferOffset = src->offset,
                .imageSubresource = {.aspectMask = vuk::ImageAspectFlagBits::eColor, .layerCount = 1},
                .imageOffset = upload_offset,
                .imageExtent = upload_extent,
              };
              cmd_list.copy_buffer_to_image(src, dst, buffer_copy_region);
              return dst;
            }
          );

          if (!acquired) {
            acquired_image = font_texture->acquire({}, vuk::eNone);
          }

          // Freed into this frame's list, which is recycled only after its submission completes.
          acquired_image = upload_pass(
            vuk::acquire_buf("imgui font staging", *upload_buffer, vuk::Access::eNone),
            std::move(acquired_image)
          );
          auto texture_id = this->add_image(std::move(acquired_image));
          texture->SetTexID(texture_id);
          texture->SetStatus(ImTextureStatus_OK);
        } break;
        case ImTextureStatus_OK: {
          acquired_image = font_texture->acquire({}, vuk::eFragmentSampled);
          auto texture_id = this->add_image(std::move(acquired_image));
          texture->SetTexID(texture_id);
        } break;
        case ImTextureStatus_WantDestroy: {
          font_texture->destroy();
        } break;
        case ImTextureStatus_Destroyed:;
      }
#endif
    }
  }

  auto sampled_images_array = vuk::declare_array("imgui_sampled", std::span(rendering_images));
  size_t vertex_size = draw_data->TotalVtxCount * sizeof(ImDrawVert);
  size_t index_size = draw_data->TotalIdxCount * sizeof(ImDrawIdx);
  if (!draw_data || vertex_size == 0) {
    if (!rendering_images.empty()) {
      context.wait_on(std::move(sampled_images_array));
    }

    return target;
  }

  auto imvert = context.alloc_transient_buffer(vuk::MemoryUsage::eCPUtoGPU, vertex_size, 1);
  auto imind = context.alloc_transient_buffer(vuk::MemoryUsage::eCPUtoGPU, index_size, 1);

  size_t vtx_dst = 0, idx_dst = 0;
  for (int n = 0; n < draw_data->CmdListsCount; n++) {
    const ImDrawList* cmd_list = draw_data->CmdLists[n];
    auto imverto = imvert->add_offset(vtx_dst * sizeof(ImDrawVert));
    auto imindo = imind->add_offset(idx_dst * sizeof(ImDrawIdx));

    memcpy(imverto.mapped_ptr, cmd_list->VtxBuffer.Data, cmd_list->VtxBuffer.Size * sizeof(ImDrawVert));
    memcpy(imindo.mapped_ptr, cmd_list->IdxBuffer.Data, cmd_list->IdxBuffer.Size * sizeof(ImDrawIdx));

    vtx_dst += cmd_list->VtxBuffer.Size;
    idx_dst += cmd_list->IdxBuffer.Size;
  }

  auto reset_render_state =
    [](vuk::CommandBuffer& command_buffer, const vuk::Buffer& vertex, const vuk::Buffer& index) -> void {
    if (index.size > 0) {
      command_buffer.bind_index_buffer(
        index,
        sizeof(ImDrawIdx) == 2 ? vuk::IndexType::eUint16 : vuk::IndexType::eUint32
      );
    }
    command_buffer
      .bind_vertex_buffer(
        0,
        vertex,
        0,
        vuk::Packed{vuk::Format::eR32G32Sfloat, vuk::Format::eR32G32Sfloat, vuk::Format::eR8G8B8A8Unorm}
      )
      .set_viewport(0, vuk::Rect2D::framebuffer());
  };

  auto bind_base_pipeline = [draw_data](vuk::CommandBuffer& command_buffer) -> void {
    command_buffer.bind_graphics_pipeline("imgui");
    struct PC {
      float translate[2];
      float scale[2];
    } pc;
    pc.scale[0] = 2.0f / draw_data->DisplaySize.x;
    pc.scale[1] = 2.0f / draw_data->DisplaySize.y;
    pc.translate[0] = -1.0f - draw_data->DisplayPos.x * pc.scale[0];
    pc.translate[1] = -1.0f - draw_data->DisplayPos.y * pc.scale[1];
    command_buffer.push_constants(vuk::ShaderStageFlagBits::eVertex, 0, pc);
  };

  auto shadow_params_copy = shadow_draw_data;

  return vuk::make_pass(
      "imgui",
      [reset_render_state, bind_base_pipeline, draw_data, shadow_draw_params = std::move(shadow_params_copy)]( //
          vuk::CommandBuffer& command_buffer,
          VUK_BA(vuk::Access::eVertexRead) vertex_buf,
          VUK_BA(vuk::Access::eIndexRead) index_buf,
          VUK_IA(vuk::eColorWrite) color_rt,
          VUK_ARG(vuk::ImageAttachment[], vuk::Access::eFragmentSampled) sis) {
        command_buffer.set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
            .set_rasterization(vuk::PipelineRasterizationStateCreateInfo{})
            .set_color_blend(color_rt, vuk::BlendPreset::eAlphaBlend);

        reset_render_state(command_buffer, vertex_buf, index_buf);
        // Will project scissor/clipping rectangles into framebuffer space
        const ImVec2 clip_off = draw_data->DisplayPos;    // (0,0) unless using multi-viewports
        const ImVec2 clip_scale = draw_data
                                      ->FramebufferScale; // (1,1) unless using retina display which are often (2,2)

        // Render command lists
        // (Because we merged all buffers into a single one, we maintain our own offset into them)
        int global_vtx_offset = 0;
        int global_idx_offset = 0;
        bool pipeline_dirty = true;
        for (int n = 0; n < draw_data->CmdListsCount; n++) {
          const ImDrawList* cmd_list = draw_data->CmdLists[n];
          for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++) {
            const ImDrawCmd* im_cmd = &cmd_list->CmdBuffer[cmd_i];
            if (im_cmd->UserCallback != nullptr) {
              // User callback, registered via ImDrawList::AddCallback()
              // (ImDrawCallback_ResetRenderState is a special callback value used by the user to request the renderer
              // to reset render state.)
              if (im_cmd->UserCallback == ImDrawCallback_ResetRenderState) {
                reset_render_state(command_buffer, vertex_buf, index_buf);
                pipeline_dirty = true;
              } else {
                im_cmd->UserCallback(cmd_list, im_cmd);
              }
            } else {
              // Project scissor/clipping rectangles into framebuffer space
              ImVec4 clip_rect;
              clip_rect.x = (im_cmd->ClipRect.x - clip_off.x) * clip_scale.x;
              clip_rect.y = (im_cmd->ClipRect.y - clip_off.y) * clip_scale.y;
              clip_rect.z = (im_cmd->ClipRect.z - clip_off.x) * clip_scale.x;
              clip_rect.w = (im_cmd->ClipRect.w - clip_off.y) * clip_scale.y;

              const auto fb_width = command_buffer.get_ongoing_render_pass().extent.width;
              const auto fb_height = command_buffer.get_ongoing_render_pass().extent.height;
              if (clip_rect.x < fb_width && clip_rect.y < fb_height && clip_rect.z >= 0.0f && clip_rect.w >= 0.0f) {
                // Negative offsets are illegal for vkCmdSetScissor
                clip_rect.x = std::max(clip_rect.x, 0.0f);
                clip_rect.y = std::max(clip_rect.y, 0.0f);

                // Apply scissor/clipping rectangle
                vuk::Rect2D scissor;
                scissor.offset.x = static_cast<int32_t>(clip_rect.x);
                scissor.offset.y = static_cast<int32_t>(clip_rect.y);
                scissor.extent.width = static_cast<uint32_t>(clip_rect.z - clip_rect.x);
                scissor.extent.height = static_cast<uint32_t>(clip_rect.w - clip_rect.y);
                command_buffer.set_scissor(0, scissor);

                const auto texture_id = im_cmd->GetTexID();
                if (texture_id >= SHADOW_TEX_ID_BASE) {
                  struct ShadowPC {
                    float translate[2];
                    float scale[2];
                    ImGuiShadowDrawData shadow;
                  } shadow_pc;
                  static_assert(sizeof(ShadowPC) == 56, "imgui_shadow.slang push constant layout mismatch");
                  shadow_pc.scale[0] = 2.0f / draw_data->DisplaySize.x;
                  shadow_pc.scale[1] = 2.0f / draw_data->DisplaySize.y;
                  shadow_pc.translate[0] = -1.0f - draw_data->DisplayPos.x * shadow_pc.scale[0];
                  shadow_pc.translate[1] = -1.0f - draw_data->DisplayPos.y * shadow_pc.scale[1];
                  shadow_pc.shadow = shadow_draw_params[texture_id - SHADOW_TEX_ID_BASE];

                  command_buffer.bind_graphics_pipeline("imgui_shadow")
                                .push_constants(vuk::ShaderStageFlagBits::eVertex |
                                                    vuk::ShaderStageFlagBits::eFragment,
                                                0,
                                                shadow_pc)
                                .draw_indexed(im_cmd->ElemCount,
                                              1,
                                              im_cmd->IdxOffset + global_idx_offset,
                                              im_cmd->VtxOffset + global_vtx_offset,
                                              0);
                  pipeline_dirty = true;
                  continue;
                }

                if (pipeline_dirty) {
                  bind_base_pipeline(command_buffer);
                  pipeline_dirty = false;
                }

                // NOTE: Dear ImGui assumes id 0 for textures means they are invalid textures.
                // So we use indices for textures starting from 1 thus this -1 is required.
                const auto& image = sis[texture_id - 1];

                command_buffer.bind_image(0, 1, image)
                              .bind_sampler(0, 0, {.magFilter = vuk::Filter::eLinear, .minFilter = vuk::Filter::eLinear})
                              .draw_indexed(im_cmd->ElemCount,
                                            1,
                                            im_cmd->IdxOffset + global_idx_offset,
                                            im_cmd->VtxOffset + global_vtx_offset,
                                            0);
              }
            }
          }
          global_idx_offset += cmd_list->IdxBuffer.Size;
          global_vtx_offset += cmd_list->VtxBuffer.Size;
        }

        return color_rt;
      })(std::move(imvert),
         std::move(imind),
         std::move(target),
         std::move(sampled_images_array));
}

auto ImGuiRenderer::build_window_shadows(this ImGuiRenderer& self, ImDrawData* draw_data) -> void {
  ZoneScoped;

  self.shadow_draw_data.clear();

  if (!self.shadow_settings.enabled || self.shadow_settings.layers.empty() || draw_data->CmdLists.Size == 0)
    return;

  auto& ctx = *ImGui::GetCurrentContext();

  auto casters = ankerl::svector<std::pair<const ImDrawList*, const ImGuiWindow*>, 32>{};
  for (const auto* window : ctx.Windows) {
    if (window_casts_shadow(window))
      casters.emplace_back(window->DrawList, window);
  }
  if (casters.empty())
    return;

  const auto display_min = glm::vec2(draw_data->DisplayPos.x, draw_data->DisplayPos.y);
  const auto display_max = display_min + glm::vec2(draw_data->DisplaySize.x, draw_data->DisplaySize.y);
  const auto ui_scale = App::get_ui_scale();

  auto used_lists = 0_sz;
  for (auto index = 0; index < draw_data->CmdLists.Size; index++) {
    const auto* window_list = draw_data->CmdLists[index];
    const auto caster = std::ranges::find(casters, window_list, &decltype(casters)::value_type::first);
    if (caster == casters.end())
      continue;

    const auto* window = caster->second;
    const auto rect_min = glm::vec2(window->Pos.x, window->Pos.y);
    const auto rect_max = rect_min + glm::vec2(window->Size.x, window->Size.y);
    const auto rect_center = (rect_min + rect_max) * 0.5f;
    const auto rect_half = (rect_max - rect_min) * 0.5f;
    const auto rounding = window->WindowRounding;

    if (self.shadow_draw_lists.size() <= used_lists)
      self.shadow_draw_lists.emplace_back(std::make_unique<ImDrawList>(ImGui::GetDrawListSharedData()));

    auto* shadow_list = self.shadow_draw_lists[used_lists].get();
    shadow_list->_OwnerName = "##WindowShadow";
    shadow_list->_ResetForNewFrame();
    shadow_list->PushClipRect(ImVec2(display_min.x, display_min.y), ImVec2(display_max.x, display_max.y), false);

    for (const auto& layer : self.shadow_settings.layers) {
      const auto sigma = std::max(layer.sigma * ui_scale, 0.01f);
      const auto spread = layer.spread * ui_scale;
      const auto shadow_center = rect_center + layer.offset * ui_scale;
      const auto shadow_half = glm::max(rect_half + spread, glm::vec2(0.5f));
      const auto extent = 3.0f * sigma;

      const auto outer_min = glm::max(shadow_center - shadow_half - extent, display_min);
      const auto outer_max = glm::min(shadow_center + shadow_half + extent, display_max);
      if (outer_max.x <= outer_min.x || outer_max.y <= outer_min.y)
        continue;

      const auto texture_id = SHADOW_TEX_ID_BASE + static_cast<ImTextureID>(self.shadow_draw_data.size());
      self.shadow_draw_data.emplace_back(
        ImGuiShadowDrawData{
          .half_size = shadow_half,
          .corner_radius = std::max(rounding + spread, 0.0f),
          .sigma = sigma,
          .cutout_center = rect_center - shadow_center,
          .cutout_half_size = rect_half,
          .cutout_radius = rounding,
          .cutout_enabled = 1.0f,
        }
      );

      const auto color = ImGui::ColorConvertFloat4ToU32(
        ImVec4(layer.color.r, layer.color.g, layer.color.b, layer.color.a)
      );

      // The caster's rounded rect always covers this inset, so the cutout zeroes the shadow there.
      const auto inset = rounding * 0.2929f;
      shadow_list->PushTexture(ImTextureRef(texture_id));
      add_shadow_ring(shadow_list, outer_min, outer_max, rect_min + inset, rect_max - inset, shadow_center, color);
      shadow_list->PopTexture();
    }

    shadow_list->PopClipRect();
    shadow_list->_PopUnusedDrawCmd();

    if (shadow_list->VtxBuffer.Size == 0)
      continue;

    used_lists += 1;
    draw_data->CmdLists.insert(draw_data->CmdLists.Data + index, shadow_list);
    index += 1;

    draw_data->TotalVtxCount += shadow_list->VtxBuffer.Size;
    draw_data->TotalIdxCount += shadow_list->IdxBuffer.Size;
  }

  draw_data->CmdListsCount = draw_data->CmdLists.Size;
}

ImTextureID ImGuiRenderer::add_image(vuk::Value<vuk::ImageAttachment>&& attachment) {
  rendering_images.emplace_back(std::move(attachment));
  return rendering_images.size();
}

ImTextureID ImGuiRenderer::add_image(const TextureView& texture_view) {
  if (this->acquired_images.contains(texture_view.image_view_id)) {
    return this->acquired_images[texture_view.image_view_id];
  }

  auto attachment = texture_view.acquire({}, vuk::eFragmentSampled);
  const auto texture_id = this->add_image(std::move(attachment));
  this->acquired_images.emplace(texture_view.get_view_id(), texture_id);

  return texture_id;
}

void ImGuiRenderer::on_mouse_pos(glm::vec2 pos) {
  ZoneScoped;

  auto& imgui = ImGui::GetIO();
  imgui.AddMousePosEvent(pos.x, pos.y);
}

void ImGuiRenderer::on_mouse_button(u8 button, bool down) {
  ZoneScoped;

  i32 imgui_button = 0;
  switch (button) {
    case SDL_BUTTON_LEFT  : imgui_button = 0; break;
    case SDL_BUTTON_RIGHT : imgui_button = 1; break;
    case SDL_BUTTON_MIDDLE: imgui_button = 2; break;
    case SDL_BUTTON_X1    : imgui_button = 3; break;
    case SDL_BUTTON_X2    : imgui_button = 4; break;
    default               : return;
  }

  auto& imgui = ImGui::GetIO();
  imgui.AddMouseButtonEvent(imgui_button, down);
  imgui.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
}

void ImGuiRenderer::on_mouse_scroll(glm::vec2 offset) {
  ZoneScoped;

  auto& imgui = ImGui::GetIO();
  imgui.AddMouseWheelEvent(offset.x, offset.y);
}

auto ImGuiRenderer::wants_keyboard(this const ImGuiRenderer& self) -> bool {
  return self.keyboard_input_enabled || ImGui::GetIO().WantTextInput;
}

ImGuiKey to_imgui_key(SDL_Keycode keycode, SDL_Scancode scancode);
void ImGuiRenderer::on_key(u32 key_code, u32 scan_code, u16 mods, bool down) {
  ZoneScoped;

  if (!wants_keyboard()) {
    return;
  }

  auto& imgui = ImGui::GetIO();
  imgui.AddKeyEvent(ImGuiMod_Ctrl, (mods & SDL_KMOD_CTRL) != 0);
  imgui.AddKeyEvent(ImGuiMod_Shift, (mods & SDL_KMOD_SHIFT) != 0);
  imgui.AddKeyEvent(ImGuiMod_Alt, (mods & SDL_KMOD_ALT) != 0);
  imgui.AddKeyEvent(ImGuiMod_Super, (mods & SDL_KMOD_GUI) != 0);

  const auto key = to_imgui_key(static_cast<SDL_Keycode>(key_code), static_cast<SDL_Scancode>(scan_code));
  imgui.AddKeyEvent(key, down);
  imgui
    .SetKeyEventNativeData(key, static_cast<i32>(key_code), static_cast<i32>(scan_code), static_cast<i32>(scan_code));
}

void ImGuiRenderer::on_text_input(const c8* text) {
  ZoneScoped;

  if (!wants_keyboard()) {
    return;
  }

  auto& imgui = ImGui::GetIO();
  imgui.AddInputCharactersUTF8(text);
}

ImGuiKey to_imgui_key(SDL_Keycode keycode, SDL_Scancode scancode) {
  ZoneScoped;

  switch (scancode) {
    case SDL_SCANCODE_KP_0       : return ImGuiKey_Keypad0;
    case SDL_SCANCODE_KP_1       : return ImGuiKey_Keypad1;
    case SDL_SCANCODE_KP_2       : return ImGuiKey_Keypad2;
    case SDL_SCANCODE_KP_3       : return ImGuiKey_Keypad3;
    case SDL_SCANCODE_KP_4       : return ImGuiKey_Keypad4;
    case SDL_SCANCODE_KP_5       : return ImGuiKey_Keypad5;
    case SDL_SCANCODE_KP_6       : return ImGuiKey_Keypad6;
    case SDL_SCANCODE_KP_7       : return ImGuiKey_Keypad7;
    case SDL_SCANCODE_KP_8       : return ImGuiKey_Keypad8;
    case SDL_SCANCODE_KP_9       : return ImGuiKey_Keypad9;
    case SDL_SCANCODE_KP_PERIOD  : return ImGuiKey_KeypadDecimal;
    case SDL_SCANCODE_KP_DIVIDE  : return ImGuiKey_KeypadDivide;
    case SDL_SCANCODE_KP_MULTIPLY: return ImGuiKey_KeypadMultiply;
    case SDL_SCANCODE_KP_MINUS   : return ImGuiKey_KeypadSubtract;
    case SDL_SCANCODE_KP_PLUS    : return ImGuiKey_KeypadAdd;
    case SDL_SCANCODE_KP_ENTER   : return ImGuiKey_KeypadEnter;
    case SDL_SCANCODE_KP_EQUALS  : return ImGuiKey_KeypadEqual;
    default                      : break;
  }

  switch (keycode) {
    case SDLK_TAB         : return ImGuiKey_Tab;
    case SDLK_LEFT        : return ImGuiKey_LeftArrow;
    case SDLK_RIGHT       : return ImGuiKey_RightArrow;
    case SDLK_UP          : return ImGuiKey_UpArrow;
    case SDLK_DOWN        : return ImGuiKey_DownArrow;
    case SDLK_PAGEUP      : return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN    : return ImGuiKey_PageDown;
    case SDLK_HOME        : return ImGuiKey_Home;
    case SDLK_END         : return ImGuiKey_End;
    case SDLK_INSERT      : return ImGuiKey_Insert;
    case SDLK_DELETE      : return ImGuiKey_Delete;
    case SDLK_BACKSPACE   : return ImGuiKey_Backspace;
    case SDLK_SPACE       : return ImGuiKey_Space;
    case SDLK_RETURN      : return ImGuiKey_Enter;
    case SDLK_ESCAPE      : return ImGuiKey_Escape;
    case SDLK_APOSTROPHE  : return ImGuiKey_Apostrophe;
    case SDLK_COMMA       : return ImGuiKey_Comma;
    case SDLK_MINUS       : return ImGuiKey_Minus;
    case SDLK_PERIOD      : return ImGuiKey_Period;
    case SDLK_SLASH       : return ImGuiKey_Slash;
    case SDLK_SEMICOLON   : return ImGuiKey_Semicolon;
    case SDLK_EQUALS      : return ImGuiKey_Equal;
    case SDLK_LEFTBRACKET : return ImGuiKey_LeftBracket;
    case SDLK_BACKSLASH   : return ImGuiKey_Backslash;
    case SDLK_RIGHTBRACKET: return ImGuiKey_RightBracket;
    case SDLK_GRAVE       : return ImGuiKey_GraveAccent;
    case SDLK_CAPSLOCK    : return ImGuiKey_CapsLock;
    case SDLK_SCROLLLOCK  : return ImGuiKey_ScrollLock;
    case SDLK_NUMLOCKCLEAR: return ImGuiKey_NumLock;
    case SDLK_PRINTSCREEN : return ImGuiKey_PrintScreen;
    case SDLK_PAUSE       : return ImGuiKey_Pause;
    case SDLK_LCTRL       : return ImGuiKey_LeftCtrl;
    case SDLK_LSHIFT      : return ImGuiKey_LeftShift;
    case SDLK_LALT        : return ImGuiKey_LeftAlt;
    case SDLK_LGUI        : return ImGuiKey_LeftSuper;
    case SDLK_RCTRL       : return ImGuiKey_RightCtrl;
    case SDLK_RSHIFT      : return ImGuiKey_RightShift;
    case SDLK_RALT        : return ImGuiKey_RightAlt;
    case SDLK_RGUI        : return ImGuiKey_RightSuper;
    case SDLK_APPLICATION : return ImGuiKey_Menu;
    case SDLK_0           : return ImGuiKey_0;
    case SDLK_1           : return ImGuiKey_1;
    case SDLK_2           : return ImGuiKey_2;
    case SDLK_3           : return ImGuiKey_3;
    case SDLK_4           : return ImGuiKey_4;
    case SDLK_5           : return ImGuiKey_5;
    case SDLK_6           : return ImGuiKey_6;
    case SDLK_7           : return ImGuiKey_7;
    case SDLK_8           : return ImGuiKey_8;
    case SDLK_9           : return ImGuiKey_9;
    case SDLK_A           : return ImGuiKey_A;
    case SDLK_B           : return ImGuiKey_B;
    case SDLK_C           : return ImGuiKey_C;
    case SDLK_D           : return ImGuiKey_D;
    case SDLK_E           : return ImGuiKey_E;
    case SDLK_F           : return ImGuiKey_F;
    case SDLK_G           : return ImGuiKey_G;
    case SDLK_H           : return ImGuiKey_H;
    case SDLK_I           : return ImGuiKey_I;
    case SDLK_J           : return ImGuiKey_J;
    case SDLK_K           : return ImGuiKey_K;
    case SDLK_L           : return ImGuiKey_L;
    case SDLK_M           : return ImGuiKey_M;
    case SDLK_N           : return ImGuiKey_N;
    case SDLK_O           : return ImGuiKey_O;
    case SDLK_P           : return ImGuiKey_P;
    case SDLK_Q           : return ImGuiKey_Q;
    case SDLK_R           : return ImGuiKey_R;
    case SDLK_S           : return ImGuiKey_S;
    case SDLK_T           : return ImGuiKey_T;
    case SDLK_U           : return ImGuiKey_U;
    case SDLK_V           : return ImGuiKey_V;
    case SDLK_W           : return ImGuiKey_W;
    case SDLK_X           : return ImGuiKey_X;
    case SDLK_Y           : return ImGuiKey_Y;
    case SDLK_Z           : return ImGuiKey_Z;
    case SDLK_F1          : return ImGuiKey_F1;
    case SDLK_F2          : return ImGuiKey_F2;
    case SDLK_F3          : return ImGuiKey_F3;
    case SDLK_F4          : return ImGuiKey_F4;
    case SDLK_F5          : return ImGuiKey_F5;
    case SDLK_F6          : return ImGuiKey_F6;
    case SDLK_F7          : return ImGuiKey_F7;
    case SDLK_F8          : return ImGuiKey_F8;
    case SDLK_F9          : return ImGuiKey_F9;
    case SDLK_F10         : return ImGuiKey_F10;
    case SDLK_F11         : return ImGuiKey_F11;
    case SDLK_F12         : return ImGuiKey_F12;
    case SDLK_F13         : return ImGuiKey_F13;
    case SDLK_F14         : return ImGuiKey_F14;
    case SDLK_F15         : return ImGuiKey_F15;
    case SDLK_F16         : return ImGuiKey_F16;
    case SDLK_F17         : return ImGuiKey_F17;
    case SDLK_F18         : return ImGuiKey_F18;
    case SDLK_F19         : return ImGuiKey_F19;
    case SDLK_F20         : return ImGuiKey_F20;
    case SDLK_F21         : return ImGuiKey_F21;
    case SDLK_F22         : return ImGuiKey_F22;
    case SDLK_F23         : return ImGuiKey_F23;
    case SDLK_F24         : return ImGuiKey_F24;
    case SDLK_AC_BACK     : return ImGuiKey_AppBack;
    case SDLK_AC_FORWARD  : return ImGuiKey_AppForward;
    default               : break;
  }
  return ImGuiKey_None;
}
} // namespace ox
