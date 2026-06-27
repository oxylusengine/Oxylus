#pragma once

#include <vuk/ImageAttachment.hpp>

#include "Panels/EditorPanelState.hpp"
#include "UI/TextEditor.hpp"

namespace ox {
class TextEditorPanel : public EditorPanelState {
public:
  TextEditor text_editor = {};

  TextEditorPanel();

  auto on_update(this TextEditorPanel& self) -> void{}
  auto on_render(this TextEditorPanel& self, vuk::ImageAttachment swapchain_attachment) -> void;
};
} // namespace ox
