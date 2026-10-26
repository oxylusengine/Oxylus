#pragma once

#include "Render/Fwd.hpp"


#include "Panels/EditorPanelState.hpp"
#include "UI/TextEditor.hpp"

namespace ox {
class TextEditorPanel : public EditorPanelState {
public:
  TextEditor text_editor = {};

  TextEditorPanel();

  auto on_update(this TextEditorPanel& self) -> void{}
  auto on_render(this TextEditorPanel& self, const SwapchainInfo& swapchain_info) -> void;
};
} // namespace ox
