#include "EditorPanelRegistry.hpp"

namespace ox {
auto EditorPanelRegistry::update_all(this EditorPanelRegistry& self) -> void {
  ZoneScoped;
  for (auto& cb : self.update_callbacks) {
    cb();
  }
}
auto EditorPanelRegistry::render_all(this EditorPanelRegistry& self, vuk::ImageAttachment attachment) -> void {
  ZoneScoped;
  for (auto& cb : self.render_callbacks) {
    cb(attachment);
  }
}
auto EditorPanelRegistry::draw_window_menu(this EditorPanelRegistry& self) -> void {
  ZoneScoped;
  for (auto& meta : self.panel_metadata) {
    // ImGui::MenuItem(meta.name, nullptr, meta.visible);
  }
}

} // namespace ox
