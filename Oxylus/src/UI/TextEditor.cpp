#include "UI/TextEditor.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include "OS/File.hpp"
#include "UI/UI.hpp"

namespace ox {
auto TextEditor::render(this TextEditor& self, const char* id, bool* visible) -> void {
  ZoneScoped;

  ImGui::SetNextWindowSize(
    ImVec2(ImGui::GetMainViewport()->Size.x / 2, ImGui::GetMainViewport()->Size.y / 2),
    ImGuiCond_Appearing
  );
  UI::center_next_window(ImGuiCond_Appearing);
  if (ImGui::Begin(id, visible)) {
    ImGuiTabBarFlags tab_bar_flags = ImGuiTabBarFlags_FittingPolicyDefault_ | ImGuiTabBarFlags_Reorderable;
    tab_bar_flags |= ImGuiTabBarFlags_DrawSelectedOverline;
    if (ImGui::BeginTabBar("##tabs", tab_bar_flags)) {
      for (auto& [name, document] : self.documents) {
        ImGuiTabItemFlags tab_flags = (document.dirty ? ImGuiTabItemFlags_UnsavedDocument : 0);
        bool is_visible = ImGui::BeginTabItem(name.c_str(), &document.open, tab_flags);

        // Cancel attempt to close when unsaved add to save queue so we can display a popup.
        if (!document.open && document.dirty) {
          document.open = true;
          self.close_queue.push_back(&document);
        }

        document.draw_context_menu(self.close_queue);
        if (is_visible) {
          if (ImGui::BeginChild("##body_window", {}, 0, ImGuiWindowFlags_MenuBar)) {
            self.draw_menu_bar(document);

            ImGui::PushFont(self.body_font, self.font_size);
            document.draw_body();
            ImGui::PopFont();
          }
          ImGui::EndChild();
          ImGui::EndTabItem();
        }
      }

      ImGui::EndTabBar();
    }
  }

  // Display closing confirmation UI
  if (!self.close_queue.empty()) {
    int close_queue_unsaved_documents = 0;
    for (usize n = 0; n < self.close_queue.size(); n++)
      if (self.close_queue[n]->dirty)
        close_queue_unsaved_documents++;

    if (close_queue_unsaved_documents == 0) {
      // Close documents when all are unsaved
      for (usize n = 0; n < self.close_queue.size(); n++)
        self.close_queue[n]->force_close();
      self.close_queue.clear();
    } else {
      if (!ImGui::IsPopupOpen("Save?"))
        ImGui::OpenPopup("Save?");
      if (ImGui::BeginPopupModal("Save?", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Save change to the following items?");
        float item_height = ImGui::GetTextLineHeightWithSpacing();
        if (ImGui::BeginChild(ImGui::GetID("frame"), ImVec2(-FLT_MIN, 6.25f * item_height), ImGuiChildFlags_FrameStyle))
          for (Document* doc : self.close_queue)
            if (doc->dirty)
              ImGui::TextUnformatted(doc->name.c_str());
        ImGui::EndChild();

        ImVec2 button_size(ImGui::GetFontSize() * 7.0f, 0.0f);
        if (ImGui::Button("Yes", button_size)) {
          for (Document* doc : self.close_queue) {
            if (doc->dirty)
              doc->save();
            doc->force_close();
          }
          self.close_queue.clear();
          ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("No", button_size)) {
          for (Document* doc : self.close_queue)
            doc->force_close();
          self.close_queue.clear();
          ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", button_size)) {
          self.close_queue.clear();
          ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
      }
    }
  }

  ImGui::End();
}

auto TextEditor::open_file(this TextEditor& self, const std::filesystem::path& file_path) -> void {
  ZoneScoped;

  auto name = file_path.filename().string();
  auto file_contents = File::to_string(file_path);
  if (!file_contents.empty()) {
    auto document = Document{
      .open = true,
      .name = name,
      .content = file_contents,
      .path = file_path,
    };
    self.documents.insert_or_assign(name, document);
  }
}

auto TextEditor::Document::force_close(this Document& self) -> void {
  self.open = false;
  self.dirty = false;
}

auto TextEditor::Document::save(this TextEditor::Document& self) -> void {
  ZoneScoped;

  if (!self.dirty) {
    return;
  }

  auto file = File(self.path, FileAccess::Write);
  file.write(self.content);
  file.close();

  self.dirty = false;
}

auto TextEditor::Document::draw_body(this TextEditor::Document& self) -> void {
  ZoneScoped;

  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.1, 0.1, 0.1, 1.0));

  if (ImGui::InputTextMultiline(
        "##source", //
        &self.content,
        ImGui::GetContentRegionAvail(),
        ImGuiInputTextFlags_AllowTabInput
      )) {
    self.dirty = true;
  }

  ImGui::PopStyleColor();
}

auto TextEditor::Document::draw_context_menu(this TextEditor::Document& self, std::vector<Document*>& close_queue)
  -> void {
  ZoneScoped;

  if (!ImGui::BeginPopupContextItem())
    return;

  ImGui::SetNextItemShortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_Tooltip);
  if (ImGui::MenuItem("Save", "Ctrl+S", false, self.open))
    self.save();
  ImGui::SetNextItemShortcut(ImGuiMod_Ctrl | ImGuiKey_W, ImGuiInputFlags_Tooltip);
  if (ImGui::MenuItem("Close", "Ctrl+W", false, self.open))
    close_queue.push_back(&self);
  ImGui::EndPopup();
}

auto TextEditor::draw_menu_bar(this TextEditor& self, TextEditor::Document& document) -> void {
  ZoneScoped;

  if (ImGui::BeginMenuBar()) {
    if (ImGui::BeginMenu("File")) {
      if (ImGui::MenuItem("Close all")) {
        for (auto& [n, d] : self.documents) {
          self.close_queue.emplace_back(&d);
        }
      }
      if (ImGui::MenuItem("Save")) {
        document.save();
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
      UI::begin_properties();
      UI::property("Font size", &self.font_size);
      UI::end_properties();
      ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
  }
}
} // namespace ox
