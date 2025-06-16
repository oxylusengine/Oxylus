#pragma once

#include <imgui.h>
#include <imgui_internal.h>
#include <vuk/Value.hpp>

#include "Asset/Texture.hpp"
#include "Core/UUID.hpp"

namespace ox {
class UI {
public:
  static constexpr ImGuiTableFlags default_properties_flags = ImGuiTableFlags_BordersInnerV |
                                                              ImGuiTableFlags_SizingStretchSame;

  static int ui_context_id;
  static int s_counter;
  static char id_buffer[16];

  static void push_id();
  static void pop_id();

  static void push_frame_style(bool on = true);
  static void pop_frame_style();

  static bool
  begin_properties(ImGuiTableFlags flags = default_properties_flags, bool fixed_width = false, float width = 0.4f);
  static void end_properties();

  static void begin_property_grid(const char* label, const char* tooltip, const bool align_text_right = false);
  static void end_property_grid();

  static void tooltip_hover(const char* text);

  static ImVec2 get_icon_button_size(const char8_t* icon, const char* label);

  static void center_next_window();

  static void spacing(const uint32_t count);

  static void align_right(float item_width);

  static void text(std::string_view label, std::string_view value, const char* tooltip = nullptr);

  // --- Properties ---

  // Bool
  static bool property(const char* label, bool* flag, const char* tooltip = nullptr);

  // Dropdown
  static bool
  property(const char* label, int* value, const char** dropdown_strings, int count, const char* tooltip = nullptr);

  // Drag
  template <std::integral T>
  static bool
  property(const char* label, T* value, T min = 0, T max = 0, float speed = 1.0f, const char* tooltip = nullptr) {
    begin_property_grid(label, tooltip);
    bool modified;

    int data_type = ImGuiDataType_S32;
    if constexpr (std::is_signed_v<T>) {
      if constexpr (sizeof(T) == 1)
        data_type = ImGuiDataType_S8;
      else if constexpr (sizeof(T) == 2)
        data_type = ImGuiDataType_S16;
      else if constexpr (sizeof(T) == 4)
        data_type = ImGuiDataType_S32;
      else if constexpr (sizeof(T) == 8)
        data_type = ImGuiDataType_S64;
    } else {
      if constexpr (sizeof(T) == 1)
        data_type = ImGuiDataType_U8;
      else if constexpr (sizeof(T) == 2)
        data_type = ImGuiDataType_U16;
      else if constexpr (sizeof(T) == 4)
        data_type = ImGuiDataType_U32;
      else if constexpr (sizeof(T) == 8)
        data_type = ImGuiDataType_U64;
    }

    if (max > min)
      modified = ImGui::DragScalar(id_buffer, data_type, value, speed, &min, &max);
    else
      modified = ImGui::DragScalar(id_buffer, data_type, value, speed);

    end_property_grid();
    return modified;
  }

  // Drag
  template <std::floating_point T>
  static bool property(const char* label,
                       T* value,
                       T min = 0,
                       T max = 0,
                       const char* tooltip = nullptr,
                       float delta = 0.1f,
                       const char* fmt = "%.3f") {
    begin_property_grid(label, tooltip);
    bool modified;

    int data_type = ImGuiDataType_Float;
    if constexpr (sizeof(T) == 8)
      data_type = ImGuiDataType_Double;

    if (max > min)
      modified = ImGui::DragScalar(id_buffer, data_type, value, delta, &min, &max, fmt);
    else
      modified = ImGui::DragScalar(id_buffer, data_type, value, delta, nullptr, nullptr, fmt);

    end_property_grid();
    return modified;
  }

  // Vec2/3/4
  template <typename T>
  static bool property_vector(const char* label,
                              T& value,
                              bool color = false,
                              const bool show_alpha = true,
                              const char* tooltip = nullptr,
                              float delta = 0.1f,
                              f32 min = 0.f,
                              f32 max = 1.f) {
    begin_property_grid(label, tooltip);
    bool modified;
    const int component_count = value.length();
    if (component_count >= 3 && color) {
      if (show_alpha)
        modified = ImGui::ColorEdit4(id_buffer, glm::value_ptr(value));
      else
        modified = ImGui::ColorEdit3(id_buffer, glm::value_ptr(value));
    } else {
      modified = ImGui::DragScalarN(id_buffer, ImGuiDataType_Float, glm::value_ptr(value), component_count, delta, &min, &max);
    }
    end_property_grid();
    return modified;
  }

  // Texture
  static bool texture_property(const char* label, UUID& texture_uuid, const char* tooltip = nullptr);

  static bool button(const char* label, const ImVec2& size = ImVec2(0, 0), const char* tooltip = nullptr);

  static bool toggle_button(const char* label,
                            bool state,
                            ImVec2 size = {0, 0},
                            float alpha = 1.0f,
                            float pressed_alpha = 1.0f,
                            ImGuiButtonFlags button_flags = ImGuiButtonFlags_None);

  static bool input_text(const char* label,
                         std::string* str,
                         ImGuiInputTextFlags flags = 0,
                         ImGuiInputTextCallback callback = NULL,
                         void* user_data = NULL,
                         const char* tooltip = nullptr);

  // Vec3 with reset button
  static bool
  draw_vec3_control(const char* label, glm::vec3& values, const char* tooltip = nullptr, float reset_value = 0.0f);

  static bool draw_vec2_control(const char* label,
                                glm::vec2& values,
                                const char* tooltip = nullptr,
                                const float reset_value = 0.0f);

  static void image(vuk::Value<vuk::ImageAttachment>&& attch,
                    ImVec2 size,
                    const ImVec2& uv0 = ImVec2(0, 0),
                    const ImVec2& uv1 = ImVec2(1, 1),
                    const ImVec4& tint_col = ImVec4(1, 1, 1, 1),
                    const ImVec4& border_col = ImVec4(0, 0, 0, 0));

  // images
  static void image(const Texture& texture,
                    ImVec2 size,
                    const ImVec2& uv0 = ImVec2(0, 0),
                    const ImVec2& uv1 = ImVec2(1, 1),
                    const ImVec4& tint_col = ImVec4(1, 1, 1, 1),
                    const ImVec4& border_col = ImVec4(0, 0, 0, 0));

  static bool image_button(const char* id,
                           const Texture& view,
                           ImVec2 size,
                           const ImVec2& uv0 = ImVec2(0, 0),
                           const ImVec2& uv1 = ImVec2(1, 1),
                           const ImVec4& tint_col = ImVec4(1, 1, 1, 1),
                           const ImVec4& bg_col = ImVec4(0, 0, 0, 0));

  static bool icon_button(const char8_t* icon, const char* label, ImVec4 icon_color = {0.537f, 0.753f, 0.286f, 1.0f});

  static void clipped_text(const ImVec2& pos_min,
                           const ImVec2& pos_max,
                           const char* text,
                           const char* text_end,
                           const ImVec2* text_size_if_known,
                           const ImVec2& align,
                           const ImRect* clip_rect,
                           float wrap_width);
  static void clipped_text(ImDrawList* draw_list,
                           const ImVec2& pos_min,
                           const ImVec2& pos_max,
                           const char* text,
                           const char* text_display_end,
                           const ImVec2* text_size_if_known,
                           const ImVec2& align,
                           const ImRect* clip_rect,
                           float wrap_width);

  static void
  draw_framerate_overlay(ImVec2 work_pos = {}, ImVec2 work_size = {}, ImVec2 padding = {}, bool* visible = nullptr);
};
} // namespace ox
