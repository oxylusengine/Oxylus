#include "EditorSettingsPanel.hpp"

#include <SDL3/SDL_scancode.h>
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

  ImGui::BeginChild("right_panel", ImVec2(300, 0), ImGuiChildFlags_Borders);
  {
    auto& editor = App::mod<Editor>();
    auto& undo_redo_system = editor.undo_redo_system;
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

  ImGui::BeginChild("right_panel", ImVec2(300, 0), ImGuiChildFlags_Borders);
  {
    auto& input = App::mod<Input>();
    auto bindings = input.get_bindings();
    static std::string waiting_for_bind = "";

    UI::begin_properties(UI::default_properties_flags, true, 0.5f);

    for (auto& [id, binding] : bindings) {
      if (binding.context == "editor") {
        ImGui::PushID(id.c_str());

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(binding.action_id.c_str());

        ImGui::TableNextColumn();

        if (waiting_for_bind == id) {
          // --- LISTENING STATE ---
          ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.6f, 0.0f, 1.0f));
          ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
          ImGui::Button("Press any key...", ImVec2(-FLT_MIN, 0));
          ImGui::PopStyleColor(2);

          auto [numkeys, key_state] = input.get_keyboard_state();
          auto mod_code = input.get_mod_state();
          auto mod_code_as_scan_code = ScanCode::Unknown;
          if (mod_code & ModCode::AnyAlt) {
            mod_code_as_scan_code = ScanCode::LeftAlt;
          } else if (mod_code & ModCode::AnyControl) {
            mod_code_as_scan_code = ScanCode::LeftControl;
          } else if (mod_code & ModCode::AnyShift) {
            mod_code_as_scan_code = ScanCode::LeftShift;
          }

          if (key_state[static_cast<u32>(ScanCode::Escape)]) {
            waiting_for_bind = "";
          } else {
            for (u32 i = static_cast<u32>(ScanCode::A); i < numkeys; ++i) {
              auto scan_code = static_cast<ScanCode>(i);

              if (key_state[i] && scan_code != mod_code_as_scan_code) {
                std::ignore = input.rebind_action(id, {InputCode(scan_code, mod_code)});

                waiting_for_bind = "";
                break;
              }
            }
          }
        } else {
          if (!binding.primary_inputs.empty()) {
            auto scan_code = binding.primary_inputs.front().scan_code;
            auto mod_code = binding.primary_inputs.front().mod_code;
            auto mod_code_to_scan_code = ScanCode::Unknown;
            if (mod_code & ModCode::AnyAlt) {
              mod_code_to_scan_code = ScanCode::LeftAlt;
            } else if (mod_code & ModCode::AnyControl) {
              mod_code_to_scan_code = ScanCode::LeftControl;
            } else if (mod_code & ModCode::AnyShift) {
              mod_code_to_scan_code = ScanCode::LeftShift;
            }
            std::string_view current_key_name = input.get_key_name(Input::get_key_code_from_scan_code(scan_code));
            std::string_view current_mod_name = input.get_key_name(
              Input::get_key_code_from_scan_code(mod_code_to_scan_code)
            );
            std::string button_name = current_key_name.data();
            if (!current_mod_name.empty()) {
              button_name = fmt::format("{} + {}", current_mod_name, current_key_name);
            }
            if (ImGui::Button(button_name.c_str(), ImVec2(-FLT_MIN, 0))) {
              waiting_for_bind = id;
            }
          }
        }

        ImGui::PopID();
      }
    }

    UI::end_properties();
  }
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
      if (ImGui::BeginTable("options_side", 1, ImGuiTableFlags_BordersH)) {
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
