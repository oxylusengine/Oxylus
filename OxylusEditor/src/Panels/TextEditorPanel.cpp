#include "TextEditorPanel.hpp"

#include <icons/IconsMaterialDesignIcons.h>
#include <imgui.h>
#include <tracy/Tracy.hpp>

#include "Core/App.hpp"
#include "Editor.hpp"

namespace ox {
TextEditorPanel::TextEditorPanel() : EditorPanel("TextEditor", ICON_MDI_TEXT_BOX_EDIT, false) {
  text_editor.body_font = App::mod<Editor>().editor_theme.mono_font;
}

void TextEditorPanel::on_update() {}

void TextEditorPanel::on_render(vuk::Extent3D extent, vuk::Format format) {
  ZoneScoped;

  text_editor.render(_id.c_str(), &visible);
}
} // namespace ox
