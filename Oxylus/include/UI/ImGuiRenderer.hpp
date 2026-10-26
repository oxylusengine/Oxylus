#pragma once

#include <ankerl/svector.h>
#include <ankerl/unordered_dense.h>
#include <expected>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <imgui.h>
#include <memory>
#include <vir/Fwd.hpp>

#include "Asset/Texture.hpp"

namespace ox {
class Renderer;
class Input;

struct ImGuiVariables {
  vir::ValueID vertices = vir::ValueID::Invalid;
  vir::ValueID indices = vir::ValueID::Invalid;
};

struct ImGuiShadowLayer {
  f32 sigma = 12.0f;
  f32 spread = 0.0f;
  glm::vec2 offset = {0.0f, 0.0f};
  glm::vec4 color = {0.0f, 0.0f, 0.0f, 0.28f};
};

struct ImGuiShadowSettings {
  bool enabled = true;
  ankerl::svector<ImGuiShadowLayer, 4> layers = {};
};

// Mirrors the shadow half of `imgui.slang`'s push constant block.
struct ImGuiShadowDrawData {
  glm::vec2 half_size = {};
  f32 corner_radius = 0.0f;
  f32 sigma = 0.0f;
  glm::vec2 cutout_center = {};
  glm::vec2 cutout_half_size = {};
  f32 cutout_radius = 0.0f;
  f32 cutout_enabled = 0.0f;
};

class ImGuiRenderer {
public:
  constexpr static auto MODULE_NAME = "ImGuiRenderer";
  using module_dependencies = std::tuple<Input, Renderer>;

  Texture font_texture = {};

  bool keyboard_input_enabled = true;

  ImGuiShadowSettings shadow_settings = {};
  std::vector<std::unique_ptr<ImDrawList>> shadow_draw_lists = {};
  std::vector<ImGuiShadowDrawData> shadow_draw_data = {};

  auto init(this ImGuiRenderer& self) -> std::expected<void, std::string>;
  auto deinit(this ImGuiRenderer& self) -> std::expected<void, std::string>;

  auto wants_keyboard(this const ImGuiRenderer& self) -> bool;

  void begin_frame(f64 delta_time, glm::vec2 logical_size, glm::vec2 real_size);
  auto set_base_style(this ImGuiRenderer& self, const ImGuiStyle& style) -> void;
  // finishes the frame's UI, before the program uploads and draws its geometry
  auto end_frame(this ImGuiRenderer& self) -> void;
  // draws the current UI over `target`, recorded once with the geometry going through `variables`
  auto record(this ImGuiRenderer& self, vir::Module& module, vir::ValueID target, ImGuiVariables& variables)
    -> vir::ValueID;
  // uploads this frame's geometry into the variables `record` declared, before each execution
  auto bind(this const ImGuiRenderer& self, vir::Program& program, const ImGuiVariables& variables) -> void;

  // images are drawn by bindless index, so whatever is shown has to rest at `vir::Access::FragmentSampled`
  auto add_image(this ImGuiRenderer& self, const TextureView& texture_view) -> ImTextureID;

  ImFont* load_default_font();
  ImFont* load_font(const std::filesystem::path& path, f32 font_size = 0.f, option<ImFontConfig> font_config = nullopt);
  void build_fonts(); // Legacy API

  void on_mouse_pos(glm::vec2 pos);
  void on_mouse_button(u8 button, bool down);
  void on_mouse_scroll(glm::vec2 offset);
  void on_key(u32 key_code, u32 scan_code, u16 mods, bool down);
  void on_text_input(const c8* text);

  auto build_window_shadows(this ImGuiRenderer& self, ImDrawData* draw_data) -> void;

private:
  c8* clipboard_text = nullptr;
  ImGuiStyle base_style = {};
  // the draw data finalized by the last `end_frame`
  ImDrawData* staged_draw_data = nullptr;
  f32 applied_ui_scale = 0.0f;
  bool keyboard_routed_last_frame = true;

  auto init_clipboard(this ImGuiRenderer& self) -> void;
  auto apply_ui_scale(this ImGuiRenderer& self) -> void;
};
} // namespace ox
