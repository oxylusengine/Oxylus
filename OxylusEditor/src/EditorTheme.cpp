#include "EditorTheme.hpp"

#include <ImGuizmo.h>
#include <icons/IconsMaterialDesignIcons.h>

#include "Core/App.hpp"
#include "Core/VFS.hpp"
#include "Scene/ECSModule/Core.hpp"
#include "UI/ImGuiLayer.hpp"

namespace ox {
static ImVec4 lighten(ImVec4 c, float p) {
  return {glm::max(0.f, c.x + 1.0f * p), glm::max(0.f, c.y + 1.0f * p), glm::max(0.f, c.z + 1.0f * p), c.w};
}

void EditorTheme::init(this EditorTheme& self) {
  const auto* app = App::get();

  auto* vfs = App::get_system<VFS>(EngineSystems::VFS);
  auto regular_font_path = vfs->resolve_physical_dir(VFS::APP_DIR, "Fonts/FiraSans-Regular.ttf");
  auto bold_font_path = vfs->resolve_physical_dir(VFS::APP_DIR, "Fonts/FiraSans-Bold.ttf");

  auto* imguilayer = app->get_imgui_layer();

  ImFontConfig fonts_config;
  fonts_config.SizePixels = self.regular_font_size;

  fonts_config.MergeMode = false;
  self.regular_font = imguilayer->load_font(regular_font_path, self.regular_font_size, fonts_config);
  fonts_config.MergeMode = true;
  imguilayer->add_icon_font(self.regular_font_size, fonts_config);

  fonts_config.MergeMode = false;
  self.bold_font = imguilayer->load_font(bold_font_path, self.regular_font_size, fonts_config);
  fonts_config.MergeMode = true;
  imguilayer->add_icon_font(self.regular_font_size, fonts_config);

  self.component_icon_map[typeid(LightComponent).hash_code()] = ICON_MDI_LIGHTBULB;
  self.component_icon_map[typeid(CameraComponent).hash_code()] = ICON_MDI_CAMERA;
  self.component_icon_map[typeid(AudioSourceComponent).hash_code()] = ICON_MDI_VOLUME_HIGH;
  self.component_icon_map[typeid(TransformComponent).hash_code()] = ICON_MDI_VECTOR_LINE;
  self.component_icon_map[typeid(MeshComponent).hash_code()] = ICON_MDI_VECTOR_SQUARE;
  self.component_icon_map[typeid(LuaScriptComponent).hash_code()] = ICON_MDI_LANGUAGE_LUA;
  self.component_icon_map[typeid(AudioListenerComponent).hash_code()] = ICON_MDI_CIRCLE_SLICE_8;
  self.component_icon_map[typeid(RigidbodyComponent).hash_code()] = ICON_MDI_SOCCER;
  self.component_icon_map[typeid(BoxColliderComponent).hash_code()] = ICON_MDI_CHECKBOX_BLANK_OUTLINE;
  self.component_icon_map[typeid(SphereColliderComponent).hash_code()] = ICON_MDI_CIRCLE_OUTLINE;
  self.component_icon_map[typeid(CapsuleColliderComponent).hash_code()] = ICON_MDI_CIRCLE_OUTLINE;
  self.component_icon_map[typeid(TaperedCapsuleColliderComponent).hash_code()] = ICON_MDI_CIRCLE_OUTLINE;
  self.component_icon_map[typeid(CylinderColliderComponent).hash_code()] = ICON_MDI_CIRCLE_OUTLINE;
  self.component_icon_map[typeid(MeshColliderComponent).hash_code()] = ICON_MDI_CHECKBOX_BLANK_OUTLINE;
  self.component_icon_map[typeid(CharacterControllerComponent).hash_code()] = ICON_MDI_CIRCLE_OUTLINE;
  self.component_icon_map[typeid(ParticleSystemComponent).hash_code()] = ICON_MDI_LAMP;
  self.component_icon_map[typeid(SpriteComponent).hash_code()] = ICON_MDI_SQUARE_OUTLINE;
  self.component_icon_map[typeid(SpriteAnimationComponent).hash_code()] = ICON_MDI_SHAPE_SQUARE_PLUS;
  self.component_icon_map[typeid(AtmosphereComponent).hash_code()] = ICON_MDI_WEATHER_CLOUDY;
  self.component_icon_map[typeid(AutoExposureComponent).hash_code()] = ICON_MDI_CAMERA_ENHANCE;

  self.apply_theme();
  self.set_style();
}

void EditorTheme::apply_theme(bool dark) {
  ImVec4* colors = ImGui::GetStyle().Colors;

  if (dark) {

    colors[ImGuiCol_Text] = ImVec4(1.f, 1.f, 1.f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.42f, 0.42f, 0.42f, 1.00f);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.85f, 0.48f, 0.00f, 0.73f);

    colors[ImGuiCol_WindowBg] = Gruvbox::dark0;
    colors[ImGuiCol_ChildBg] = Gruvbox::dark0;
    colors[ImGuiCol_PopupBg] = Gruvbox::dark0;

    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.20f, 0.20f, 0.20f, 0.35f);

    colors[ImGuiCol_FrameBg] = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.28f, 0.28f, 0.28f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.26f, 0.26f, 0.26f, 1.00f);

    colors[ImGuiCol_Border] = ImVec4(0.178f, 0.178f, 0.178f, 1.000f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.178f, 0.178f, 0.178f, 1.000f);

    colors[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);

    colors[ImGuiCol_MenuBarBg] = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);

    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.34f, 0.34f, 0.34f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.40f, 0.40f, 0.40f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.43f, 0.43f, 0.43f, 1.00f);

    colors[ImGuiCol_CheckMark] = ImVec4(1.00f, 0.56f, 0.00f, 1.00f);

    colors[ImGuiCol_SliderGrab] = ImVec4(1.00f, 0.56f, 0.00f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(1.00f, 0.56f, 0.00f, 1.00f);

    colors[ImGuiCol_Button] = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.28f, 0.28f, 0.28f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(1.00f, 0.56f, 0.00f, 0.82f);

    colors[ImGuiCol_Header] = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.27f, 0.27f, 0.27f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.27f, 0.27f, 0.27f, 1.00f);

    colors[ImGuiCol_Separator] = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);

    colors[ImGuiCol_ResizeGrip] = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.19f, 0.19f, 0.19f, 1.00f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(0.31f, 0.31f, 0.31f, 1.00f);

    colors[ImGuiCol_Tab] = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.19f, 0.19f, 0.19f, 1.00f);
    colors[ImGuiCol_TabActive] = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    colors[ImGuiCol_TabUnfocused] = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    colors[ImGuiCol_TabSelectedOverline] = ImVec4(0.34f, 0.34f, 0.34f, 1.00f);

    colors[ImGuiCol_DockingPreview] = ImVec4(1.00f, 0.56f, 0.00f, 0.22f);
    colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);

    colors[ImGuiCol_PlotLines] = ImVec4(1.00f, 0.56f, 0.00f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.00f, 0.56f, 0.00f, 1.00f);
    colors[ImGuiCol_PlotHistogram] = ImVec4(1.00f, 0.56f, 0.00f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.56f, 0.00f, 1.00f);

    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
    colors[ImGuiCol_TableBorderStrong] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_TableBorderLight] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_TableRowBg] = ImVec4(1.00f, 1.00f, 1.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.10f);

    colors[ImGuiCol_DragDropTarget] = ImVec4(1.00f, 0.56f, 0.00f, 1.00f);

    colors[ImGuiCol_NavHighlight] = ImVec4(1.00f, 0.56f, 0.00f, 1.00f);
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);

    header_selected_color = ImVec4(1.00f, 0.56f, 0.00f, 0.50f);
    header_hovered_color = lighten(colors[ImGuiCol_HeaderActive], 0.1f);
    window_bg_alternative_color = ImVec4(0.1f, 0.1f, 0.1f, 1.0f);
    asset_icon_color = lighten(header_selected_color, 0.9f);
  }
}

void EditorTheme::set_style() {
  {
    auto& style = ImGuizmo::GetStyle();
    style.TranslationLineThickness *= 1.3f;
    style.TranslationLineArrowSize *= 1.3f;
    style.RotationLineThickness *= 1.3f;
    style.RotationOuterLineThickness *= 1.3f;
    style.ScaleLineThickness *= 1.3f;
    style.ScaleLineCircleSize *= 1.3f;
    style.HatchedAxisLineThickness *= 1.3f;
    style.CenterCircleSize *= 1.3f;

    ImGuizmo::SetGizmoSizeClipSpace(0.2f);
  }

  {
    ImGuiStyle* style = &ImGui::GetStyle();

    style->AntiAliasedFill = true;
    style->AntiAliasedLines = true;
    style->AntiAliasedLinesUseTex = true;

    style->WindowPadding = ImVec2(4.0f, 4.0f);
    style->FramePadding = ImVec2(4.0f, 4.0f);
    style->CellPadding = ImVec2(8.0f, 4.0f);
    style->ItemSpacing = ImVec2(8.0f, 3.0f);
    style->ItemInnerSpacing = ImVec2(2.0f, 4.0f);
    style->TouchExtraPadding = ImVec2(0.0f, 0.0f);
    style->IndentSpacing = 12;
    style->ScrollbarSize = 14;
    style->GrabMinSize = 10;

    style->WindowBorderSize = 0.0f;
    style->ChildBorderSize = 0.0f;
    style->PopupBorderSize = 1.5f;
    style->FrameBorderSize = 0.0f;
    style->TabBorderSize = 1.0f;
    style->DockingSeparatorSize = 3.0f;

    style->WindowRounding = 6.0f;
    style->ChildRounding = 0.0f;
    style->FrameRounding = 2.0f;
    style->PopupRounding = 2.0f;
    style->ScrollbarRounding = 3.0f;
    style->GrabRounding = 2.0f;
    style->LogSliderDeadzone = 4.0f;
    style->TabRounding = 3.0f;

    style->WindowTitleAlign = ImVec2(0.0f, 0.5f);
    style->WindowMenuButtonPosition = ImGuiDir_None;
    style->ColorButtonPosition = ImGuiDir_Left;
    style->ButtonTextAlign = ImVec2(0.5f, 0.5f);
    style->SelectableTextAlign = ImVec2(0.0f, 0.0f);
    style->DisplaySafeAreaPadding = ImVec2(8.0f, 8.0f);

    ui_frame_padding = ImVec2(4.0f, 2.0f);
    popup_item_spacing = ImVec2(6.0f, 8.0f);

    constexpr ImGuiColorEditFlags color_edit_flags = ImGuiColorEditFlags_AlphaBar |
                                                     ImGuiColorEditFlags_AlphaPreviewHalf |
                                                     ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_InputRGB |
                                                     ImGuiColorEditFlags_PickerHueBar | ImGuiColorEditFlags_Uint8;
    ImGui::SetColorEditOptions(color_edit_flags);

    style->ScaleAllSizes(1.0f);
  }
}
} // namespace ox
