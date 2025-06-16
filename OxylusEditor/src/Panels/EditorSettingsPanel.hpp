#pragma once

#include "EditorPanel.hpp"

namespace ox {
class EditorSettingsPanel : public EditorPanel {
public:
  EditorSettingsPanel();
  ~EditorSettingsPanel() override = default;
  void on_render(vuk::Extent3D extent, vuk::Format format) override;
};
} // namespace ox
