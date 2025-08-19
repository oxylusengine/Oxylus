#pragma once

#include <imgui.h>
#include <vuk/Value.hpp>
#include <ankerl/unordered_dense.h>
#include <glm/vec2.hpp>

#include "Asset/Texture.hpp"
#include "Core/Layer.hpp"

namespace ox {
class VkContext;
class ImGuiLayer : public Layer {
public:
  std::shared_ptr<Texture> font_texture = nullptr;
  std::vector<vuk::Value<vuk::ImageAttachment>> rendering_images;
  ankerl::unordered_dense::map<ImageViewID, ImTextureID> acquired_images;

  ImGuiLayer();
  ~ImGuiLayer() override = default;

  void on_attach() override;
  void on_detach() override;

  void begin_frame(f64 delta_time, vuk::Extent3D extent);
  [[nodiscard]]
  vuk::Value<vuk::ImageAttachment> end_frame(VkContext& context, vuk::Value<vuk::ImageAttachment> target);

  ImTextureID add_image(vuk::Value<vuk::ImageAttachment>&& attachment);
  ImTextureID add_image(const Texture& texture);

  ImFont* load_default_font();
  ImFont* load_font(const std::string& path, f32 font_size = 0.f, option<ImFontConfig> font_config = nullopt);
  void build_fonts(); // Legacy API

  void on_mouse_pos(glm::vec2 pos);
  void on_mouse_button(u8 button, bool down);
  void on_mouse_scroll(glm::vec2 offset);
  void on_key(u32 key_code, u32 scan_code, u16 mods, bool down);
  void on_text_input(const c8* text);
};
} // namespace ox
