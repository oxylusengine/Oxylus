#pragma once

#include <ankerl/unordered_dense.h>
#include <expected>
#include <glm/vec2.hpp>
#include <imgui.h>
#include <vuk/Value.hpp>

#include "Asset/Texture.hpp"

namespace ox {
class RenderContext;
class Renderer;
class Input;
class ImGuiRenderer {
public:
  constexpr static auto MODULE_NAME = "ImGuiRenderer";
  using module_dependencies = std::tuple<Input, Renderer>;

  option<Texture> font_texture = nullopt;
  std::vector<vuk::Value<vuk::ImageAttachment>> rendering_images;
  ankerl::unordered_dense::map<ImageViewID, ImTextureID> acquired_images;

  auto init() -> std::expected<void, std::string>;
  auto deinit() -> std::expected<void, std::string>;

  void begin_frame(f64 delta_time, glm::vec2 logical_size, glm::vec2 real_size);
  [[nodiscard]]
  vuk::Value<vuk::ImageAttachment> end_frame(RenderContext& context, vuk::Value<vuk::ImageAttachment> target);

  ImTextureID add_image(vuk::Value<vuk::ImageAttachment>&& attachment);
  ImTextureID add_image(const TextureView& texture_view);

  ImFont* load_default_font();
  ImFont* load_font(const std::filesystem::path& path, f32 font_size = 0.f, option<ImFontConfig> font_config = nullopt);
  void build_fonts(); // Legacy API

  void on_mouse_pos(glm::vec2 pos);
  void on_mouse_button(u8 button, bool down);
  void on_mouse_scroll(glm::vec2 offset);
  void on_key(u32 key_code, u32 scan_code, u16 mods, bool down);
  void on_text_input(const c8* text);
};
} // namespace ox
