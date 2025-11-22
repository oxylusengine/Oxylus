#pragma once

#include "Panels/EditorPanel.hpp"
#include "UI/AssetManagerViewer.hpp"

namespace ox {
class AssetManagerPanel : public EditorPanel {
public:
  AssetManagerPanel();

  ~AssetManagerPanel() override = default;

  AssetManagerPanel(const AssetManagerPanel& other) = delete;
  AssetManagerPanel(AssetManagerPanel&& other) = delete;
  AssetManagerPanel& operator=(const AssetManagerPanel& other) = delete;
  AssetManagerPanel& operator=(AssetManagerPanel&& other) = delete;

  void on_update() override;
  void on_render(vuk::ImageAttachment swapchain_attachment) override;

private:
  AssetManagerViewer viewer = {};
};
} // namespace ox
