#include "Panels/ActivityLogPanel.hpp"

#include <fmt/format.h>
#include <icons/IconsMaterialDesignIcons.h>
#include <ranges>
#include <tracy/Tracy.hpp>

namespace ox {
ActivityLogPanel::ActivityLogPanel() : EditorPanelState("Activity Log", ICON_MDI_FORUM, false) {}

auto ActivityLogPanel::set_system(this ActivityLogPanel& self, NotificationSystem* system) -> void {
  ZoneScoped;

  self.notification_system = system;
}

auto ActivityLogPanel::on_render(this ActivityLogPanel& self, vuk::ImageAttachment swapchain_attachment) -> void {
  ZoneScoped;

  if (self.on_begin()) {
    float filter_cursor_pos_x = ImGui::GetCursorPosX();

    self.log_filter.Draw("##log_filter", ImGui::GetContentRegionAvail().x);
    if (!self.log_filter.IsActive()) {
      ImGui::SameLine();
      ImGui::SetCursorPosX(filter_cursor_pos_x + ImGui::GetFontSize() * 0.5f);
      ImGui::TextUnformatted(ICON_MDI_MAGNIFY "Search logs...");
    }

    if (ImGui::BeginTable("table_row_height", 2, ImGuiTableFlags_Borders)) {
      ImGui::TableSetupColumn("##", ImGuiTableColumnFlags_WidthFixed, 12.f);

      for (auto& notification : std::views::reverse(self.notification_system->notification_history)) {
        if (!self.log_filter.PassFilter(notification.title.c_str())) {
          continue;
        }
        ImGui::TableNextRow();

        std::string icon_text = {};
        ImVec4 text_color = {};
        switch (notification.type) {
          case Notification::Info: {
            icon_text = ICON_MDI_INFORMATION;
            text_color = ImVec4{0, 1, 0, 1};
            break;
          }
          case Notification::Warn: {
            icon_text = ICON_MDI_ALERT;
            text_color = ImVec4{0.9f, 0.6f, 0.2f, 1};
            break;
          }
          case Notification::Error: {
            icon_text = ICON_MDI_EXCLAMATION;
            text_color = ImVec4{1, 0, 0, 1};
            break;
          }
          case Notification::Loading: {
            icon_text = ICON_MDI_CHECK_BOLD;
            text_color = ImVec4{1, 1, 1, 1};
            break;
          }
        }

        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text, text_color);
        ImGui::TextUnformatted(icon_text.c_str());
        ImGui::PopStyleColor();

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(notification.title.c_str());
      }
    }
    ImGui::EndTable();
  }

  self.on_end();
}
} // namespace ox
