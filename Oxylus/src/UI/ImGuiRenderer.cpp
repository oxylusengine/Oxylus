#include "UI/ImGuiRenderer.hpp"

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_stdinc.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <glm/common.hpp>
#include <imgui_internal.h>
#include <span>

#include "Core/App.hpp"
#include "Memory/SlotMap.hpp"
#include "Memory/Stack.hpp"
#include "Render/Graph/RenderGraph.hpp"
#include "Render/Graph/Types.hpp"
#include "Render/RenderContext.hpp"
#include "Render/Window.hpp"
#include "Utils/Log.hpp"
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
    .format = vir::Format::RGBA8Unorm,
    .extent = {static_cast<u32>(width), static_cast<u32>(height), 1_u32},
    .usage = vir::ImageUsageFlags::Sampled,
  });

  auto pixels = std::span{pixels_data, static_cast<usize>(width * height * 4)};
  font_texture.upload(pixels, vir::Access::FragmentSampled);
}

auto ImGuiRenderer::init(this ImGuiRenderer& self) -> std::expected<void, std::string> {
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

  self.init_clipboard();

  io.ConfigDpiScaleFonts = false;
  io.ConfigDpiScaleViewports = false;
  io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
  ImGuiStyle& style = ImGui::GetStyle();
  style.FontScaleDpi = 1.0f;
  self.set_base_style(style);

  self.shadow_settings.layers = {
    ImGuiShadowLayer{.sigma = 22.0f, .spread = -4.0f, .offset = {0.0f, 10.0f}, .color = {0.0f, 0.0f, 0.0f, 0.75f}},
    ImGuiShadowLayer{.sigma = 5.0f, .spread = -1.0f, .offset = {0.0f, 2.0f}, .color = {0.0f, 0.0f, 0.0f, 0.65f}},
  };

  return {};
}

auto ImGuiRenderer::deinit(this ImGuiRenderer& self) -> std::expected<void, std::string> {
  ZoneScoped;

  // owned draw lists point into the context's shared draw data
  self.shadow_draw_lists.clear();
  self.shadow_draw_data.clear();

  SDL_free(self.clipboard_text);
  self.clipboard_text = nullptr;

  ImGui::DestroyContext();
  return {};
}

auto ImGuiRenderer::init_clipboard(this ImGuiRenderer& self) -> void {
  ZoneScoped;

  // the custom backend needs SDL callbacks to reach the system clipboard on every platform
  auto& platform = ImGui::GetPlatformIO();
  platform.Platform_ClipboardUserData = &self;
  platform.Platform_GetClipboardTextFn = [](ImGuiContext* context) -> const c8* {
    ZoneScopedN("get clipboard text");

    auto& renderer = *static_cast<ImGuiRenderer*>(context->PlatformIO.Platform_ClipboardUserData);
    SDL_free(renderer.clipboard_text);
    renderer.clipboard_text = SDL_GetClipboardText();
    return renderer.clipboard_text;
  };
  platform.Platform_SetClipboardTextFn = [](ImGuiContext*, const c8* text) -> void {
    ZoneScopedN("set clipboard text");

    if (!SDL_SetClipboardText(text)) {
      OX_LOG_ERROR("Could not copy text to the clipboard: {}", SDL_GetError());
    }
  };
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

auto ImGuiRenderer::end_frame(this ImGuiRenderer& self) -> void {
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

  self.staged_draw_data = nullptr;
  ImDrawData* draw_data = ImGui::GetDrawData();
  if (!draw_data) {
    return;
  }

  self.build_window_shadows(draw_data);

  if (draw_data->Textures) {
    for (auto* texture : *draw_data->Textures) {
      if (texture->Status == ImTextureStatus_WantCreate || texture->Status == ImTextureStatus_WantUpdates) {
        self.font_texture = Texture::create({
          .format = vir::Format::RGBA8Unorm,
          .extent = {static_cast<u32>(texture->Width), static_cast<u32>(texture->Height), 1_u32},
          .usage = vir::ImageUsageFlags::Sampled,
        });
        self.font_texture.set_name("imgui font texture");
        self.font_texture.upload(
          {static_cast<u8*>(texture->GetPixels()), static_cast<usize>(texture->GetSizeInBytes())},
          vir::Access::FragmentSampled
        );

        texture->SetStatus(ImTextureStatus_OK);
      }

      texture->SetTexID(self.add_image(self.font_texture.view()));
    }
  }

  self.staged_draw_data = draw_data;
}

auto ImGuiRenderer::bind(this const ImGuiRenderer& self, vir::Program& program, const ImGuiVariables& variables)
  -> void {
  ZoneScopedN("upload imgui geometry");

  if (!program.has_variable(variables.vertices) || !program.has_variable(variables.indices)) {
    return;
  }

  program.set(variables.vertices, vir::Buffer{});
  program.set(variables.indices, vir::Buffer{});
  const auto* draw_data = self.staged_draw_data;
  if (
    !draw_data || draw_data->TotalVtxCount <= 0 || draw_data->TotalIdxCount <= 0 || draw_data->DisplaySize.x <= 0.0f ||
    draw_data->DisplaySize.y <= 0.0f
  ) {
    return;
  }

  auto& context = App::get_rendercontext();
  memory::ScopedStack stack;
  auto vertex_data = stack.alloc<u8>(static_cast<usize>(draw_data->TotalVtxCount) * sizeof(ImDrawVert));
  auto index_data = stack.alloc<u8>(static_cast<usize>(draw_data->TotalIdxCount) * sizeof(ImDrawIdx));
  auto vertex_offset = 0_sz;
  auto index_offset = 0_sz;
  for (const auto* cmd_list : draw_data->CmdLists) {
    const auto vertex_size = static_cast<usize>(cmd_list->VtxBuffer.Size) * sizeof(ImDrawVert);
    const auto index_size = static_cast<usize>(cmd_list->IdxBuffer.Size) * sizeof(ImDrawIdx);
    if (vertex_size != 0) {
      std::memcpy(vertex_data.data() + vertex_offset, cmd_list->VtxBuffer.Data, vertex_size);
    }
    if (index_size != 0) {
      std::memcpy(index_data.data() + index_offset, cmd_list->IdxBuffer.Data, index_size);
    }
    vertex_offset += vertex_size;
    index_offset += index_size;
  }

  const auto vertex_buffer = context.upload_frame_buffer(vertex_data, "imgui vertices");
  const auto index_buffer = context.upload_frame_buffer(index_data, "imgui indices");
  if (!vertex_buffer || !index_buffer) {
    OX_LOG_ERROR("failed to upload the imgui geometry");
    return;
  }

  program.set(variables.vertices, *vertex_buffer);
  program.set(variables.indices, *index_buffer);
}

auto ImGuiRenderer::record(
  this ImGuiRenderer& self, vir::Module& module, vir::ValueID target, ImGuiVariables& variables
) -> vir::ValueID {
  ZoneScoped;

  auto& render_context = App::get_rendercontext();

  struct PushConstants {
    u64 vertices = 0;
    glm::vec2 translate = {};
    glm::vec2 scale = {};
    u32 image_index = 0;
    u32 sampler_index = 0;
    u32 is_shadow = 0;
    f32 corner_radius = 0.0f;
    glm::vec2 half_size = {};
    f32 sigma = 0.0f;
    f32 cutout_radius = 0.0f;
    glm::vec2 cutout_center = {};
    glm::vec2 cutout_half_size = {};
    f32 cutout_enabled = 0.0f;
  };
  static_assert(offsetof(PushConstants, half_size) == 40, "imgui.slang push constant layout mismatch");
  static_assert(offsetof(PushConstants, cutout_enabled) == 72, "imgui.slang push constant layout mismatch");

  static constexpr auto sampler_info = vir::SamplerInfo{
    .mag_filter = VK_FILTER_LINEAR,
    .min_filter = VK_FILTER_LINEAR,
    .mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
    .address_mode_u = VK_SAMPLER_ADDRESS_MODE_REPEAT,
    .address_mode_v = VK_SAMPLER_ADDRESS_MODE_REPEAT,
    .address_mode_w = VK_SAMPLER_ADDRESS_MODE_REPEAT,
  };
  const auto sampler_index = SlotMap_decode_id(render_context.cached_sampler(sampler_info)).index;

  const auto vertices = module.declare_buffer_var("imgui vertices", vir::Access::None);
  const auto indices = module.declare_buffer_var("imgui indices", vir::Access::None);
  variables = {.vertices = vertices, .indices = indices};

  // the draw lists are whatever the frame built, so they are walked when the program runs
  const auto draws = module.declare_callback_var(
    "imgui",
    [&self, sampler_index, vertices, indices](vir::Recorder& recorder) -> void {
      const auto* draw_data = self.staged_draw_data;
      if (!draw_data) {
        return;
      }

      const auto vertex_buffer = recorder.buffer(vertices);
      const auto index_buffer = recorder.buffer(indices);
      if (vertex_buffer.is_null() || index_buffer.is_null()) {
        return;
      }

      const auto area = recorder.render_area();
      const auto scale = glm::vec2(2.0f / draw_data->DisplaySize.x, 2.0f / draw_data->DisplaySize.y);
      const auto translate = glm::vec2(
        -1.0f - draw_data->DisplayPos.x * scale.x,
        -1.0f - draw_data->DisplayPos.y * scale.y
      );
      // projects clip rectangles into framebuffer space
      const auto clip_off = draw_data->DisplayPos;
      const auto clip_scale = draw_data->FramebufferScale;

      recorder.bind_index_buffer(index_buffer, sizeof(ImDrawIdx) == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
      // the lists share one buffer of each, so every draw is offset by the lists before it
      auto global_vtx_offset = 0_u32;
      auto global_idx_offset = 0_u32;
      for (const auto* cmd_list : draw_data->CmdLists) {
        for (const auto& im_cmd : cmd_list->CmdBuffer) {
          if (im_cmd.UserCallback != nullptr) {
            if (im_cmd.UserCallback != ImDrawCallback_ResetRenderState) {
              im_cmd.UserCallback(cmd_list, &im_cmd);
            }

            continue;
          }

          auto clip_min = glm::vec2(
            (im_cmd.ClipRect.x - clip_off.x) * clip_scale.x,
            (im_cmd.ClipRect.y - clip_off.y) * clip_scale.y
          );
          const auto clip_max = glm::vec2(
            (im_cmd.ClipRect.z - clip_off.x) * clip_scale.x,
            (im_cmd.ClipRect.w - clip_off.y) * clip_scale.y
          );
          if (
            clip_min.x >= static_cast<f32>(area.extent.width) || clip_min.y >= static_cast<f32>(area.extent.height) ||
            clip_max.x < 0.0f || clip_max.y < 0.0f
          ) {
            continue;
          }

          // negative offsets are illegal for vkCmdSetScissor
          clip_min = glm::max(clip_min, glm::vec2(0.0f));
          recorder.set_scissor_absolute({
            .offset = {static_cast<i32>(clip_min.x), static_cast<i32>(clip_min.y)},
            .extent = {static_cast<u32>(clip_max.x - clip_min.x), static_cast<u32>(clip_max.y - clip_min.y)},
          });

          auto push = PushConstants{
            .vertices = vertex_buffer.device_address + (im_cmd.VtxOffset + global_vtx_offset) * sizeof(ImDrawVert),
            .translate = translate,
            .scale = scale,
            .sampler_index = sampler_index,
          };
          const auto texture_id = im_cmd.GetTexID();
          if (texture_id >= SHADOW_TEX_ID_BASE) {
            const auto& shadow = self.shadow_draw_data[texture_id - SHADOW_TEX_ID_BASE];
            push.is_shadow = 1;
            push.corner_radius = shadow.corner_radius;
            push.half_size = shadow.half_size;
            push.sigma = shadow.sigma;
            push.cutout_radius = shadow.cutout_radius;
            push.cutout_center = shadow.cutout_center;
            push.cutout_half_size = shadow.cutout_half_size;
            push.cutout_enabled = shadow.cutout_enabled;
          } else {
            // Dear ImGui treats id 0 as no texture, so ids are bindless indices plus one
            push.image_index = static_cast<u32>(texture_id - 1);
          }

          // the shader block ends at the last field, before C++ tail padding
          recorder
            .push_constant_bytes(
              0,
              std::span{
                reinterpret_cast<const u8*>(&push),
                offsetof(PushConstants, cutout_enabled) + sizeof(push.cutout_enabled)
              }
            )
            .draw_indexed(im_cmd.ElemCount, 1, im_cmd.IdxOffset + global_idx_offset, 0, 0);
        }

        global_idx_offset += static_cast<u32>(cmd_list->IdxBuffer.Size);
        global_vtx_offset += static_cast<u32>(cmd_list->VtxBuffer.Size);
      }
    }
  );

  module
    .begin_rendering(
      {{target, vir::Access::ColorRW}, {vertices, vir::Access::VertexRead}, {indices, vir::Access::IndexRead}}
    )
    .with_name("imgui")
    .bind_graphics_pipeline(render_context.pipeline("imgui"))
    .set_color_blend(0, vir::BlendPreset::AlphaBlend)
    .set_viewport(0, vir::Rect2D::framebuffer())
    .set_scissor(0, vir::Rect2D::framebuffer())
    .record_from(draws);
  return module.end_rendering<3>()[0];
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

auto ImGuiRenderer::add_image(this ImGuiRenderer&, const TextureView& texture_view) -> ImTextureID {
  return static_cast<ImTextureID>(SlotMap_decode_id(texture_view.get_view_id()).index) + 1;
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
