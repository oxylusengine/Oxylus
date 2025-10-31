#pragma once

#include "Panels/EditorPanel.hpp"
#include "UI/TextEditor.hpp"

namespace ox {
class TextEditorPanel : public EditorPanel {
public:
  TextEditor text_editor = {};

  TextEditorPanel();
  ~TextEditorPanel() override = default;

  void on_update() override;
  void on_render(vuk::ImageAttachment swapchain_attachment) override;

private:
};
} // namespace ox
