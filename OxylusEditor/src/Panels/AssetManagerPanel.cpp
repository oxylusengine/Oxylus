#include "AssetManagerPanel.hpp"

#include <icons/IconsMaterialDesignIcons.h>
#include <imgui.h>
#include <tracy/Tracy.hpp>

namespace ox {
AssetManagerPanel::AssetManagerPanel() : EditorPanelState("Asset Manager", ICON_MDI_FOLDER_SYNC, false) {
  viewer.filter_icon = ICON_MDI_FILTER;
  viewer.search_icon = ICON_MDI_MAGNIFY;
}

void AssetManagerPanel::on_update(this AssetManagerPanel& self) {}

void AssetManagerPanel::on_render(this AssetManagerPanel& self, vuk::ImageAttachment swapchain_attachment) {
  ZoneScoped;

  self.viewer.render(self.id.c_str(), &self.visible);
}
} // namespace ox
