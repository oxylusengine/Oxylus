#include "EditorSettingsPanel.hpp"

#include <imgui.h>

#include "EditorLayer.hpp"
#include "icons/IconsMaterialDesignIcons.h"

namespace ox {
EditorSettingsPanel::EditorSettingsPanel() : EditorPanel("Editor Settings", ICON_MDI_COGS, false) {}

void EditorSettingsPanel::on_render(vuk::Extent3D, vuk::Format) {
  auto& editor_theme = EditorLayer::get()->editor_theme;

  constexpr ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking;
  if (on_begin(window_flags)) {
    // Theme
    const char* themes[] = {"Dark", "White"};
    int themeIndex = 0;
    if (ImGui::Combo("Theme", &themeIndex, themes, std::size(themes))) {

      editor_theme.apply_theme(!(bool)themeIndex);
    }
    on_end();
  }
}
} // namespace ox
