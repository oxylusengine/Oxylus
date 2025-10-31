#pragma once

#include "EditorPanel.hpp"

namespace ox {
class EditorSettingsPanel : public EditorPanel {
public:
  EditorSettingsPanel();
  ~EditorSettingsPanel() override = default;
  void on_render(vuk::ImageAttachment swapchain_attachment) override;
};
} // namespace ox
