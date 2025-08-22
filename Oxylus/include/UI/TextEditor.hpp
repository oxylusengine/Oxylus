#pragma once

#include <functional>

#include "Core/Types.hpp"

namespace ox {
class TextEditor {
public:
  u32 font_size = 16;
  std::function<void(const std::string&)> save_file_callback = nullptr;

  TextEditor() = default;
  ~TextEditor() = default;

  auto render(const char* id, bool* visible) -> void;

  auto open_file(const std::string& file_path) -> void;
  auto save_file() -> void;

private:
  std::string current_content = {};
  std::string opened_file_path = {};
};
} // namespace ox
