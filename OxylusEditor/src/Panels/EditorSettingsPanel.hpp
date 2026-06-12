#pragma once

#include "EditorPanel.hpp"

namespace ox {
class EditorSettingsPanel : public EditorPanel {
public:
  EditorSettingsPanel();
  ~EditorSettingsPanel() override = default;
  void on_render(vuk::ImageAttachment swapchain_attachment) override;

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
