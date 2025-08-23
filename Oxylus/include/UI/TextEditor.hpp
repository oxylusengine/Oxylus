#pragma once

#include <ankerl/unordered_dense.h>
#include <functional>
#include <imgui.h>

#include "Core/Types.hpp"

namespace ox {
class TextEditor {
public:
  ImFont* body_font = nullptr;
  u32 font_size = 16;
  std::function<void(const std::string&)> save_file_callback = nullptr;

  TextEditor() = default;
  ~TextEditor() = default;

  auto render(this TextEditor& self, const char* id, bool* visible) -> void;

  auto open_file(this TextEditor& self, const std::string& file_path) -> void;

private:
  struct Document {
    bool open = true;
    std::string name = {};
    bool dirty = false;
    std::string content = {};
    std::string path = {};

    auto force_close(this Document& self) -> void;
    auto save(this Document& self) -> void;
    auto draw_body(this Document& self) -> void;
    auto draw_context_menu(this Document& self, std::vector<Document*>& close_queue) -> void;
  };

  ankerl::unordered_dense::map<std::string, Document> documents = {};
  std::vector<Document*> close_queue = {};

  auto draw_menu_bar(this TextEditor& self, TextEditor::Document& document) -> void;
};
} // namespace ox
