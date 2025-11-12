#include "AssetManagerPanel.hpp"

#include <tracy/Tracy.hpp>
#include <icons/IconsMaterialDesignIcons.h>
#include <imgui.h>

namespace ox {
AssetManagerPanel::AssetManagerPanel() : EditorPanel("Asset Manager", ICON_MDI_FOLDER_SYNC, false) {
  viewer.filter_icon = ICON_MDI_FILTER;
  viewer.search_icon = ICON_MDI_MAGNIFY;
}

void AssetManagerPanel::on_update() {}

void AssetManagerPanel::on_render(vuk::ImageAttachment swapchain_attachment) {
  ZoneScoped;

  viewer.render(id_.c_str(), &visible);
}
} // namespace ox
