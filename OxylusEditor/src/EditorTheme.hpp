#pragma once
#include <ankerl/unordered_dense.h>
#include <imgui.h>

struct ImFont;

namespace ox {
namespace Gruvbox {
constexpr auto dark0_hard = ImColor(29, 32, 33, 255);
constexpr auto dark0 = ImColor(40, 40, 40, 255);
constexpr auto dark0_soft = ImColor(50, 48, 47, 255);
constexpr auto dark1 = ImColor(60, 56, 54, 255);
constexpr auto dark2 = ImColor(80, 73, 69, 255);
constexpr auto dark3 = ImColor(102, 92, 84, 255);
constexpr auto dark4 = ImColor(124, 111, 100, 255);
constexpr auto dark4_256 = ImColor(124, 111, 100, 255);

constexpr auto bright_red = ImColor(251, 73, 52, 255);
constexpr auto bright_green = ImColor(184, 187, 38, 255);
constexpr auto bright_yellow = ImColor(250, 189, 47, 255);
constexpr auto bright_blue = ImColor(131, 165, 152, 255);
constexpr auto bright_purple = ImColor(211, 134, 155, 255);
constexpr auto bright_aqua = ImColor(142, 192, 124, 255);
constexpr auto bright_orange = ImColor(254, 128, 25, 255);

constexpr auto neutral_red = ImColor(204, 36, 29, 255);
constexpr auto neutral_green = ImColor(152, 151, 26, 255);
constexpr auto neutral_yellow = ImColor(215, 153, 33, 255);
constexpr auto neutral_blue = ImColor(69, 133, 136, 255);
constexpr auto neutral_purple = ImColor(177, 98, 134, 255);
constexpr auto neutral_aqua = ImColor(104, 157, 106, 255);
constexpr auto neutral_orange = ImColor(214, 93, 14, 255);
} // namespace Gruvbox

class EditorTheme {
public:
  f32 regular_font_size = 16.0f;
  f32 small_font_size = 12.0f;
  ImFont* regular_font = nullptr;
  ImFont* bold_font = nullptr;

  inline static ImVec4 header_selected_color;
  inline static ImVec4 header_hovered_color;
  inline static ImVec4 window_bg_alternative_color;
  inline static ImVec4 asset_icon_color;

  inline static ImVec2 ui_frame_padding;
  inline static ImVec2 popup_item_spacing;

  ankerl::unordered_dense::map<size_t, const char8_t*> component_icon_map = {};

  void init(this EditorTheme& self);

  void apply_theme(bool dark = true);
  void set_style();
};
} // namespace ox
