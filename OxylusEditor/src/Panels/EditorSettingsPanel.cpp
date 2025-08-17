#include "EditorSettingsPanel.hpp"

#include <imgui.h>

#include "EditorLayer.hpp"
#include "UI/UI.hpp"
#include "icons/IconsMaterialDesignIcons.h"

namespace ox {
EditorSettingsPanel::EditorSettingsPanel() : EditorPanel("Editor Settings", ICON_MDI_COGS, false) {}

void EditorSettingsPanel::on_render(vuk::Extent3D, vuk::Format) {
  auto* editor_layer = EditorLayer::get();
  auto& undo_redo_system = editor_layer->undo_redo_system;

  constexpr ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking;
  if (on_begin(window_flags)) {
    UI::begin_properties();
    auto current_history_size = undo_redo_system->get_max_history_size();
    if (UI::property("Undo history size", &current_history_size))
      undo_redo_system->set_max_history_size(current_history_size);
    UI::end_properties();
    on_end();
  }
}
} // namespace ox
