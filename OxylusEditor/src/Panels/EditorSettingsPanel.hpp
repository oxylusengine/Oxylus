#pragma once

#include <vuk/ImageAttachment.hpp>

#include "EditorPanelState.hpp"

namespace ox {
class EditorSettingsPanel : public EditorPanelState {
public:
  EditorSettingsPanel();

  auto on_update(this EditorSettingsPanel& self) -> void{}
  auto on_render(this EditorSettingsPanel& self, vuk::ImageAttachment swapchain_attachment) -> void;

private:
  enum class OptionRows : u32 {
    General = 0,
    Keybinds,

    Count,
  };
  OptionRows selected_row = OptionRows::General;

  auto option_row_to_sv(OptionRows row) -> std::string_view;

  auto draw_general_tab() -> void;
  auto draw_keybinds_tab() -> void;
};
} // namespace ox
