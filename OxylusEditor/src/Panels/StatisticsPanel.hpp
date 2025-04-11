﻿#pragma once
#include <vector>

#include "EditorPanel.hpp"

namespace ox {
class StatisticsPanel : public EditorPanel {
public:
  StatisticsPanel();
  void on_render(vuk::Extent3D extent, vuk::Format format) override;

private:
  float fps_values[50] = {};
  std::vector<float> frame_times{};

  void memory_tab() const;
  void renderer_tab();
};
} // namespace ox
