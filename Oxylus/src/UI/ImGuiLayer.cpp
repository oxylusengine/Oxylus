#include "UI/ImGuiLayer.hpp"

#include <ImGuizmo.h>
#include <SDL3/SDL_mouse.h>
#include <imgui.h>
#include <vuk/RenderGraph.hpp>
#include <vuk/Types.hpp>
#include <vuk/runtime/CommandBuffer.hpp>
#include <vuk/runtime/vk/AllocatorHelpers.hpp>
#include <vuk/runtime/vk/Pipeline.hpp>
#include <vuk/vsl/Core.hpp>

#include "Core/App.hpp"
#include "Render/Slang/Slang.hpp"
#include "Render/Utils/VukCommon.hpp"
#include "Render/Vulkan/VkContext.hpp"
#include "Render/Window.hpp"
#include "Utils/Profiler.hpp"

namespace ox {
ImGuiLayer::ImGuiLayer() : Layer("ImGuiLayer") {}

ImFont* ImGuiLayer::load_default_font() {
  ImGuiIO& io = ImGui::GetIO();
  return io.Fonts->AddFontDefault();
}

ImFont* ImGuiLayer::load_font(const std::string& path, f32 font_size, option<ImFontConfig> font_config) {
  ZoneScoped;

  ImGuiIO& io = ImGui::GetIO();
  if (font_config.has_value())
    return io.Fonts->AddFontFromFileTTF(path.c_str(), font_size, &*font_config);

  return io.Fonts->AddFontFromFileTTF(path.c_str(), font_size);
}

void ImGuiLayer::build_fonts() {
  ZoneScoped;

  ImGuiIO& io = ImGui::GetIO();
  unsigned char* pixels;
  int width, height;
  io.Fonts->Build();
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
  font_texture = std::make_shared<Texture>();
  font_texture->create({},
                       {
                           .preset = Preset::eRTT2DUnmipped,
                           .format = vuk::Format::eR8G8B8A8Srgb,
                           .mime = {},
                           .loaded_data = pixels,
                           .extent = vuk::Extent3D{static_cast<u32>(width), static_cast<u32>(height), 1u},
                       });
}

void ImGuiLayer::on_attach() {
  ZoneScoped;
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard /*| ImGuiConfigFlags_ViewportsEnable*/ |
                    ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_DpiEnableScaleFonts | ImGuiConfigFlags_IsSRGB;
  io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_HasMouseCursors;
  /*io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;*/
  io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
  io.BackendRendererName = "oxylus";

  auto& allocator = *App::get_vkcontext().superframe_allocator;
  auto& ctx = allocator.get_context();

  auto* vfs = App::get_system<VFS>(EngineSystems::VFS);
  auto shaders_dir = vfs->resolve_physical_dir(VFS::APP_DIR, "Shaders");

  Slang slang = {};
  slang.create_session({.root_directory = shaders_dir, .definitions = {}});

  slang.create_pipeline(
      ctx, "imgui", {}, {.path = shaders_dir + "/passes/imgui.slang", .entry_points = {"vs_main", "fs_main"}});
}

void ImGuiLayer::on_detach() { ImGui::DestroyContext(); }

void ImGuiLayer::begin_frame(const f64 delta_time, const vuk::Extent3D extent) {
  ZoneScoped;

  const App* app = App::get();
  auto& imgui = ImGui::GetIO();
  imgui.DeltaTime = static_cast<f32>(delta_time);
  imgui.DisplaySize = ImVec2(static_cast<f32>(extent.width), static_cast<f32>(extent.height));

  rendering_images.clear();
  acquired_images.clear();

  ImGui::NewFrame();
  ImGuizmo::BeginFrame();

  if (imgui.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange) {
    return;
  }

  const auto imgui_cursor = ImGui::GetMouseCursor();
  if (imgui.MouseDrawCursor || imgui_cursor == ImGuiMouseCursor_None) {
    app->get_window().show_cursor(false);
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
      default                         : break;
    }
    app->get_window().show_cursor(true);

    if (app->get_window().get_cursor() != next_cursor) {
      app->get_window().set_cursor(next_cursor);
    }
  }
}

vuk::Value<vuk::ImageAttachment> ImGuiLayer::end_frame(VkContext& context, vuk::Value<vuk::ImageAttachment> target) {
  ZoneScoped;

  ImGui::Render();

  ImDrawData* draw_data = ImGui::GetDrawData();

  if (draw_data->Textures != nullptr) {
    for (ImTextureData* tex : *draw_data->Textures) {
      if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates) {
        if (font_texture) {
          font_texture->destroy();
        }

        font_texture = std::make_shared<Texture>();
        font_texture->create(
            {},
            {
                .preset = Preset::eRTT2DUnmipped,
                .format = vuk::Format::eR8G8B8A8Srgb,
                .mime = {},
                .loaded_data = tex->GetPixels(),
                .extent = vuk::Extent3D{static_cast<u32>(tex->Width), static_cast<u32>(tex->Height), 1u},
            });

        tex->SetTexID(1);
        tex->SetStatus(ImTextureStatus_OK);
      }
    }
  }

  auto reset_render_state =
      [this, draw_data](vuk::CommandBuffer& command_buffer, const vuk::Buffer& vertex, const vuk::Buffer& index) {
        command_buffer //
            .bind_sampler(0, 0, vuk::LinearSamplerRepeated)
            .bind_image(0, 1, *font_texture->get_view());
        if (index.size > 0) {
          command_buffer.bind_index_buffer(index,
                                           sizeof(ImDrawIdx) == 2 ? vuk::IndexType::eUint16 : vuk::IndexType::eUint32);
        }
        command_buffer
            .bind_vertex_buffer(
                0,
                vertex,
                0,
                vuk::Packed{vuk::Format::eR32G32Sfloat, vuk::Format::eR32G32Sfloat, vuk::Format::eR8G8B8A8Unorm})
            .bind_graphics_pipeline("imgui")
            .set_viewport(0, vuk::Rect2D::framebuffer());
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

  size_t vertex_size = draw_data->TotalVtxCount * sizeof(ImDrawVert);
  size_t index_size = draw_data->TotalIdxCount * sizeof(ImDrawIdx);
  auto imvert = *context.allocate_buffer(vuk::MemoryUsage::eCPUtoGPU, vertex_size, 1);
  auto imind = *context.allocate_buffer(vuk::MemoryUsage::eCPUtoGPU, index_size, 1);

  size_t vtx_dst = 0, idx_dst = 0;
  for (int n = 0; n < draw_data->CmdListsCount; n++) {
    const ImDrawList* cmd_list = draw_data->CmdLists[n];
    auto imverto = imvert.add_offset(vtx_dst * sizeof(ImDrawVert));
    auto imindo = imind.add_offset(idx_dst * sizeof(ImDrawIdx));

    memcpy(imverto.mapped_ptr, cmd_list->VtxBuffer.Data, cmd_list->VtxBuffer.Size * sizeof(ImDrawVert));
    memcpy(imindo.mapped_ptr, cmd_list->IdxBuffer.Data, cmd_list->IdxBuffer.Size * sizeof(ImDrawIdx));

    vtx_dst += cmd_list->VtxBuffer.Size;
    idx_dst += cmd_list->IdxBuffer.Size;
  }

  auto sampled_images_array = vuk::declare_array("imgui_sampled", std::span(rendering_images));
  auto font_acquired = font_texture->acquire();

  return vuk::make_pass(
      "imgui",
      [verts = imvert, inds = imind, reset_render_state, draw_data](
          vuk::CommandBuffer& command_buffer,
          VUK_IA(vuk::eColorWrite) color_rt,
          VUK_ARG(vuk::ImageAttachment[], vuk::Access::eFragmentSampled) sis,
          VUK_IA(vuk::eFragmentSampled) font) {
        command_buffer.set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
            .set_rasterization(vuk::PipelineRasterizationStateCreateInfo{})
            .set_color_blend(color_rt, vuk::BlendPreset::eAlphaBlend);

        reset_render_state(command_buffer, verts, inds);
        // Will project scissor/clipping rectangles into framebuffer space
        const ImVec2 clip_off = draw_data->DisplayPos;    // (0,0) unless using multi-viewports
        const ImVec2 clip_scale = draw_data
                                      ->FramebufferScale; // (1,1) unless using retina display which are often (2,2)

        // Render command lists
        // (Because we merged all buffers into a single one, we maintain our own offset into them)
        int global_vtx_offset = 0;
        int global_idx_offset = 0;
        for (int n = 0; n < draw_data->CmdListsCount; n++) {
          const ImDrawList* cmd_list = draw_data->CmdLists[n];
          for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++) {
            const ImDrawCmd* im_cmd = &cmd_list->CmdBuffer[cmd_i];
            if (im_cmd->UserCallback != nullptr) {
              // User callback, registered via ImDrawList::AddCallback()
              // (ImDrawCallback_ResetRenderState is a special callback value used by the user to request the renderer
              // to reset render state.)
              if (im_cmd->UserCallback == ImDrawCallback_ResetRenderState)
                reset_render_state(command_buffer, verts, inds);
              else
                im_cmd->UserCallback(cmd_list, im_cmd);
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

                command_buffer.bind_sampler(
                    0, 0, {.magFilter = vuk::Filter::eLinear, .minFilter = vuk::Filter::eLinear});
                if (im_cmd->TexRef._TexData == NULL && im_cmd->TexRef._TexID != 0) {
                  const auto index = im_cmd->GetTexID() - 1;
                  const auto& image = sis[index];
                  command_buffer.bind_image(0, 1, image);
                } else {
                  command_buffer.bind_image(0, 1, font);
                }

                command_buffer.draw_indexed(im_cmd->ElemCount,
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
      })(std::move(target), std::move(sampled_images_array), std::move(font_acquired));
}

ImTextureID ImGuiLayer::add_image(vuk::Value<vuk::ImageAttachment>&& attachment) {
  rendering_images.emplace_back(std::move(attachment));
  return rendering_images.size();
}

ImTextureID ImGuiLayer::add_image(const Texture& texture) {
  if (this->acquired_images.contains(texture.get_view_id())) {
    return this->acquired_images[texture.get_view_id()];
  }

  auto attachment = texture.acquire();
  const auto texture_id = this->add_image(std::move(attachment));
  this->acquired_images.emplace(texture.get_view_id(), texture_id);

  return texture_id;
}

void ImGuiLayer::on_mouse_pos(glm::vec2 pos) {
  ZoneScoped;

  auto& imgui = ImGui::GetIO();
  imgui.AddMousePosEvent(pos.x, pos.y);
}

void ImGuiLayer::on_mouse_button(u8 button, bool down) {
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

void ImGuiLayer::on_mouse_scroll(glm::vec2 offset) {
  ZoneScoped;

  auto& imgui = ImGui::GetIO();
  imgui.AddMouseWheelEvent(offset.x, offset.y);
}

ImGuiKey to_imgui_key(SDL_Keycode keycode, SDL_Scancode scancode);
void ImGuiLayer::on_key(u32 key_code, u32 scan_code, u16 mods, bool down) {
  ZoneScoped;

  auto& imgui = ImGui::GetIO();
  imgui.AddKeyEvent(ImGuiMod_Ctrl, (mods & SDL_KMOD_CTRL) != 0);
  imgui.AddKeyEvent(ImGuiMod_Shift, (mods & SDL_KMOD_SHIFT) != 0);
  imgui.AddKeyEvent(ImGuiMod_Alt, (mods & SDL_KMOD_ALT) != 0);
  imgui.AddKeyEvent(ImGuiMod_Super, (mods & SDL_KMOD_GUI) != 0);

  const auto key = to_imgui_key(static_cast<SDL_Keycode>(key_code), static_cast<SDL_Scancode>(scan_code));
  imgui.AddKeyEvent(key, down);
  imgui.SetKeyEventNativeData(
      key, static_cast<i32>(key_code), static_cast<i32>(scan_code), static_cast<i32>(scan_code));
}

void ImGuiLayer::on_text_input(const c8* text) {
  ZoneScoped;

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
