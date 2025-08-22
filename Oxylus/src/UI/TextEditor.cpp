#include "UI/TextEditor.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include "Core/FileSystem.hpp"
#include "UI/UI.hpp"

namespace ox {
auto TextEditor::render(const char* id, bool* visible) -> void {
  ImGui::SetNextWindowSize(ImVec2(ImGui::GetMainViewport()->Size.x / 2, ImGui::GetMainViewport()->Size.y / 2),
                           ImGuiCond_Appearing);
  UI::center_next_window(ImGuiCond_Appearing);
  if (ImGui::Begin(id, visible, ImGuiWindowFlags_MenuBar)) {
    if (ImGui::BeginMenuBar()) {
      if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Save")) {
          save_file();
        }
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("Edit")) {
        UI::begin_properties();
        UI::property("Font size", &font_size);
        UI::end_properties();
        ImGui::EndMenu();
      }
      ImGui::EndMenuBar();
    }

    ImGui::PushFont(nullptr, font_size);
    ImGui::InputTextMultiline("##source", //
                              &current_content,
                              ImGui::GetContentRegionAvail(),
                              ImGuiInputTextFlags_AllowTabInput);
    ImGui::PopFont();
  }
  ImGui::End();
}

auto TextEditor::open_file(const std::string& file_path) -> void {
  ZoneScoped;
  auto file_contents = fs::read_file(file_path);
  if (!file_contents.empty()) {
    current_content = file_contents;
    opened_file_path = file_path;
  }
}

auto TextEditor::save_file() -> void {
  if (save_file_callback)
    save_file_callback(opened_file_path);

  fs::write_file(opened_file_path, current_content);
}
} // namespace ox
