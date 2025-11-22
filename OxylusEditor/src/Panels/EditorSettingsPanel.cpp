#include "EditorSettingsPanel.hpp"

#include <icons/IconsMaterialDesignIcons.h>
#include <imgui.h>

#include "Core/App.hpp"
#include "Editor.hpp"
#include "UI/UI.hpp"

namespace ox {
EditorSettingsPanel::EditorSettingsPanel() : EditorPanel("Editor Settings", ICON_MDI_COGS, false) {}

void EditorSettingsPanel::on_render(vuk::ImageAttachment swapchain_attachment) {
  auto& editor = App::mod<Editor>();
  auto& undo_redo_system = editor.undo_redo_system;

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
