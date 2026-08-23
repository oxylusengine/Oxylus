#include "Notification.hpp"

#include <fmt/format.h>
#include <icons/IconsMaterialDesignIcons.h>
#include <imgui_internal.h>
#include <imspinner.h>
#include <tracy/Tracy.hpp>

#include "Core/App.hpp"
#include "EditorTheme.hpp"

namespace ox {
static constexpr auto notification_window_size = ImVec2(400.f, 60.f);
static constexpr auto root_window_size = ImVec2(420.f, 80.f);
static constexpr f32 padding = 10.f;
static constexpr u32 history_max = 50;

auto NotificationSystem::add(this NotificationSystem& self, Notification&& notif) -> void {
  ZoneScoped;

  if (notif.title.empty()) {
    OX_LOG_WARN("Unnamed activity found!");
    return;
  }

  if (self.active_notifications.contains(notif.title)) {
    if (notif.completed) {
      self.active_notifications.insert_or_assign(notif.title, std::move(notif));
      return;
    }
  }

  self.notification_history.emplace_back(notif);
  self.active_notifications.emplace(notif.title, std::move(notif));

  // cleanup history
  while (self.notification_history.size() >= history_max) {
    self.notification_history.erase(self.notification_history.begin());
  }
}

auto NotificationSystem::get_last_notification(this NotificationSystem& self) -> option<Notification> {
  ZoneScoped;

  if (!self.notification_history.empty())
    return self.notification_history.back();

  return nullopt;
}

auto NotificationSystem::draw(this NotificationSystem& self) -> void {
  ZoneScoped;

  // Bottom right
  ImVec2 root_screen_pos = ImGui::GetMainViewport()->Size;
  root_screen_pos.x -= root_window_size.x + padding;
  root_screen_pos.y -= root_window_size.y + padding + 25;

  if (self.active_notifications.empty())
    return;

  App::get_window().set_cursor_override(WindowCursor::Progress);

  ImGui::SetNextWindowPos({root_screen_pos.x, root_screen_pos.y}, ImGuiCond_Always);
  ImGui::SetNextWindowSize(root_window_size, ImGuiCond_Always);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.f, 0.f, 0.f, 0.f));
  ImGui::Begin(
    "##Notifications",
    nullptr,
    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoNav
  );
  ImGui::PopStyleColor();

  for (auto& [name, notif] : self.active_notifications) {
    self.draw_single(notif);
  }

  ImGui::End();

  // Cleanup
  const auto now = std::chrono::steady_clock::now();
  constexpr auto delay = std::chrono::seconds(2); // so that really fast notifications don't flash
  std::erase_if(self.active_notifications, [&](const auto& notif) {
    return notif.second.completed && now - notif.second.created_at > delay;
  });
}

auto NotificationSystem::draw_single(this NotificationSystem& self, Notification& notif) -> void {
  ZoneScoped;

  ImGui::SetNextWindowBgAlpha(0.8f);
  ImGui::SetNextWindowSize(notification_window_size, ImGuiCond_Always);

  ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 3.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 3.0f);
  ImGui::PushStyleColor(ImGuiCol_Border, Gruvbox::dark0_hard.Value);
  if (
    ImGui::BeginChild(
      notif.title.c_str(),
      {},
      ImGuiChildFlags_Borders | ImGuiChildFlags_FrameStyle,
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs
    )
  ) {
    ImSpinner::detail::SpinnerConfig config{};
    config.setSpinnerType(ImSpinner::e_st_ang);
    config.setSpeed(6.f);
    config.setAngle(4.f);
    config.setThickness(2.f);
    config.setRadius(16.f);
    config.setColor(ImColor(1.f, 1.f, 1.f, 1.f));
    ImGui::PushFont(nullptr, 32.f);
    constexpr auto icon_child_size = 60.f;
    // Center icon vertically
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.f, 0.f));
    ImGui::SetCursorPos(ImVec2(0.f, 0.f));
    ImGui::BeginChild(
      "##notification_icon",
      ImVec2(icon_child_size, notification_window_size.y),
      0,
      ImGuiWindowFlags_NoBackground
    );
    ImGui::SetCursorPos(ImVec2(icon_child_size / 2.f - 16.f, icon_child_size / 2.f - 16.f));
    switch (notif.type) {
      case Notification::Info   : ImGui::TextUnformatted(ICON_MDI_INFORMATION); break;
      case Notification::Warn   : ImGui::TextUnformatted(ICON_MDI_ALERT); break;
      case Notification::Error  : ImGui::TextUnformatted(ICON_MDI_EXCLAMATION); break;
      case Notification::Loading: ImSpinner::Spinner("SpinnerAng270NoBg", config); break;
    }
    ImGui::PopFont();
    ImGui::EndChild();

    // Put in the same line
    ImGui::SetCursorPos(ImVec2(icon_child_size, 0.f));
    ImGui::BeginChild(
      "##notification_text",
      ImVec2(notification_window_size.x - icon_child_size, notification_window_size.y)
    );
    switch (notif.type) {
      case Notification::Info   : ImGui::Text("Info: "); break;
      case Notification::Warn   : ImGui::Text("Warning: "); break;
      case Notification::Error  : ImGui::Text("Error:"); break;
      case Notification::Loading: ImGui::Text("Loading..."); break;
    }
    ImGui::TextUnformatted(fmt::format("{}", notif.title).c_str());
    ImGui::EndChild();
    ImGui::PopStyleVar(2); // WindowPadding, FramePadding
  }
  ImGui::EndChild();

  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
}
} // namespace ox
