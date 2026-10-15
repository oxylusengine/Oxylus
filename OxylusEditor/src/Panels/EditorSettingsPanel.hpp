#pragma once

#include "Render/Fwd.hpp"


#include "EditorPanelState.hpp"

namespace ox {
class EditorSettingsPanel : public EditorPanelState {
public:
  EditorSettingsPanel();

  auto on_update(this EditorSettingsPanel& self) -> void {}
  auto on_render(this EditorSettingsPanel& self, const SwapchainInfo& swapchain_info) -> void;

private:
  enum class OptionRows : u32 {
    General = 0,
    Keybinds,

    Count,
  };
  OptionRows selected_row = OptionRows::General;
  i32 pending_ui_scale_percent = 100;
  bool ui_scale_edit_initialized = false;
  bool ui_scale_dirty = false;

  auto option_row_to_sv(OptionRows row) -> std::string_view;

  auto draw_general_tab(this EditorSettingsPanel& self) -> void;
  auto draw_keybinds_tab() -> void;
};
} // namespace ox
