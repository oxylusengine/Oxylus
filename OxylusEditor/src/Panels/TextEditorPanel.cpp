#include "TextEditorPanel.hpp"

#include <icons/IconsMaterialDesignIcons.h>
#include <imgui.h>
#include <tracy/Tracy.hpp>

#include "Core/App.hpp"
#include "Editor.hpp"

namespace ox {
TextEditorPanel::TextEditorPanel() : EditorPanelState("TextEditor", ICON_MDI_TEXT_BOX_EDIT, false) {
  text_editor.body_font = App::mod<Editor>().editor_theme.mono_font;
}

auto TextEditorPanel::on_render(this TextEditorPanel& self, vuk::ImageAttachment swapchain_attachment) -> void {
  ZoneScoped;

  self.text_editor.render(self.id.c_str(), &self.visible);
}
} // namespace ox
