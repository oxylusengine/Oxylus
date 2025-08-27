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

void AssetManagerPanel::on_render(vuk::Extent3D extent, vuk::Format format) {
  ZoneScoped;

  viewer.render(_id.c_str(), &visible);
}
} // namespace ox
