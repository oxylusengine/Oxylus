#include "EditorSettingsPanel.hpp"

#include <icons/IconsMaterialDesignIcons.h>
#include <imgui.h>

#include "Core/App.hpp"
#include "Core/Input.hpp"
#include "Editor.hpp"
#include "UI/UI.hpp"

namespace ox {
EditorSettingsPanel::EditorSettingsPanel() : EditorPanel("Editor Settings", ICON_MDI_COGS, false) {
  this->window_default_size = {500, 300};
  this->window_sizing_cond = ImGuiCond_Always;
  this->window_center_at_appear = true;
}

auto EditorSettingsPanel::option_row_to_sv(OptionRows row) -> std::string_view {
  ZoneScoped;

  switch (row) {
    case OptionRows::General : return "General";
    case OptionRows::Keybinds: return "Keybinds";
    default                  : return "Unknown";
  }
}

auto EditorSettingsPanel::draw_general_tab() -> void {
  ZoneScoped;

  auto& editor = App::mod<Editor>();
  auto& undo_redo_system = editor.undo_redo_system;
  ImGui::BeginChild("right_panel", ImVec2(300, 0), ImGuiChildFlags_Borders);
  {
    UI::begin_properties(UI::default_properties_flags, true, 0.3f);
    auto current_history_size = undo_redo_system->get_max_history_size();
    if (UI::property("Undo history size", &current_history_size))
      undo_redo_system->set_max_history_size(current_history_size);
    UI::end_properties();
  }
  ImGui::EndChild();
}

auto EditorSettingsPanel::draw_keybinds_tab() -> void {
  ZoneScoped;

  auto& input = App::mod<Input>();
  ImGui::BeginChild("right_panel", ImVec2(300, 0), ImGuiChildFlags_Borders);
  ImGui::Text("Keybinds");
  ImGui::EndChild();
}

void EditorSettingsPanel::on_render(vuk::ImageAttachment swapchain_attachment) {
  constexpr ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
                                            ImGuiWindowFlags_NoResize;
  if (on_begin(window_flags)) {
    constexpr auto table_flags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersOuter |
                                 ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersH;

    if (ImGui::BeginTable("options_table", 2, table_flags)) {
      ImGui::TableSetupColumn("##side_view", ImGuiTableColumnFlags_WidthFixed, 100);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      if (ImGui::BeginTable("options_side", 1, ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTableFlags_BordersH)) {
        for (u32 opt_index = 0; opt_index < static_cast<u32>(OptionRows::Count); opt_index++) {
          auto opt = static_cast<OptionRows>(opt_index);
          auto opt_sv = option_row_to_sv(opt);
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          if (ImGui::Selectable(opt_sv.data())) {
            this->selected_row = opt;
          }
        }
        ImGui::EndTable();
      }
      ImGui::TableNextColumn();

      switch (this->selected_row) {
        case OptionRows::General: {
          this->draw_general_tab();
          break;
        }
        case OptionRows::Keybinds: {
          this->draw_keybinds_tab();
          break;
        }
        default: break;
      }

      ImGui::EndTable();
    }

    on_end();
  }
}
} // namespace ox
