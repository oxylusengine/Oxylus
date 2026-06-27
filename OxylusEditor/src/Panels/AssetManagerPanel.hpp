#pragma once

#include "Panels/EditorPanelState.hpp"
#include "UI/AssetManagerViewer.hpp"

namespace ox {
class AssetManagerPanel : public EditorPanelState {
public:
  AssetManagerPanel();

  auto on_update(this AssetManagerPanel& self) -> void;
  auto on_render(this AssetManagerPanel& self, vuk::ImageAttachment swapchain_attachment) -> void;

private:
  AssetManagerViewer viewer = {};
};
} // namespace ox
