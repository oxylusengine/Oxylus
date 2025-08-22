#include "TextEditorPanel.hpp"

#include <Tracy.hpp>
#include <icons/IconsMaterialDesignIcons.h>
#include <imgui.h>

#include "EditorLayer.hpp"

namespace ox {
TextEditorPanel::TextEditorPanel() : EditorPanel("TextEditor", ICON_MDI_TEXT_BOX_EDIT, false) {
  auto* editor_layer = EditorLayer::get();
  text_editor.body_font = editor_layer->editor_theme.mono_font;
}

void TextEditorPanel::on_update() {}

void TextEditorPanel::on_render(vuk::Extent3D extent, vuk::Format format) {
  ZoneScoped;

  text_editor.render(_id.c_str(), &visible);
}
} // namespace ox
