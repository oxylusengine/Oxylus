#include "TextEditorPanel.hpp"

#include <Tracy.hpp>
#include <icons/IconsMaterialDesignIcons.h>
#include <imgui.h>

namespace ox {
TextEditorPanel::TextEditorPanel() : EditorPanel("TextEditor", ICON_MDI_TEXT_BOX_EDIT, false) {}

void TextEditorPanel::on_update() {}

void TextEditorPanel::on_render(vuk::Extent3D extent, vuk::Format format) {
  ZoneScoped;

  text_editor.render(_id.c_str(), &visible);
}
} // namespace ox
