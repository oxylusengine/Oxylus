#include "Notification.hpp"

#include <tracy/Tracy.hpp>
#include <fmt/format.h>
#include <icons/IconsMaterialDesignIcons.h>
#include <imgui_internal.h>
#include <imspinner.h>

namespace ox {
static constexpr auto notification_window_size = ImVec2(400.f, 50.f);
static constexpr auto root_window_size = ImVec2(420.f, 300.f);
static constexpr f32 padding = 40.f;

auto NotificationSystem::add(Notification&& notif) -> void {
  ZoneScoped;

  if (active_notifications.contains(notif.title)) {
    if (notif.completed) {
      active_notifications.insert_or_assign(notif.title, std::move(notif));
      return;
    }
  }

  active_notifications.emplace(notif.title, std::move(notif));
}

auto NotificationSystem::draw() -> void {
  ZoneScoped;
  const auto now = std::chrono::steady_clock::now();
  // Bottom right
  ImVec2 notification_screen_pos = ImGui::GetMainViewport()->Size;
  notification_screen_pos.x -= notification_window_size.x + padding;
  notification_screen_pos.y -= notification_window_size.y + padding;

  ImVec2 root_screen_pos = ImGui::GetMainViewport()->Size;
  root_screen_pos.x -= root_window_size.x + padding;
  root_screen_pos.y -= root_window_size.y + padding;

  float y_offset = 0.0f;

  if (active_notifications.empty())
    return;

  ImGui::SetNextWindowPos({root_screen_pos.x, root_screen_pos.y}, ImGuiCond_Always);
  ImGui::SetNextWindowSize(root_window_size, ImGuiCond_Always);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.f, 0.f, 0.f, 0.f));
  ImGui::Begin("##Notifications", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar);
  ImGui::PopStyleColor();

  for (auto& [name, notif] : active_notifications) {
    draw_single(notif, now, notification_screen_pos, y_offset);
    y_offset -= notification_window_size.y + 10.f;
  }

  ImGui::End();

  // Cleanup
  const auto delay = std::chrono::seconds(2); // so that really fast notifications don't flash
  std::erase_if(active_notifications,
                [&](const auto& n) { return n.second.completed && (now - n.second.created_at) > delay; });
}

auto NotificationSystem::draw_single(Notification& notif, auto current_time, const ImVec2& screen_pos, f32 y_offset)
    -> void {
  ZoneScoped;

  ImGui::SetNextWindowBgAlpha(0.8f);
  ImGui::SetNextWindowSize(notification_window_size, ImGuiCond_Always);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 3.0f);
  if (ImGui::BeginChild(notif.title.c_str(),
                        {},
                        ImGuiChildFlags_Borders | ImGuiChildFlags_FrameStyle,
                        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
    ImSpinner::detail::SpinnerConfig config{};
    config.setSpinnerType(ImSpinner::e_st_ang);
    config.setSpeed(6.f);
    config.setAngle(4.f);
    config.setThickness(2.f);
    config.setRadius(16.f);
    config.setColor(ImColor(1.f, 1.f, 1.f, 1.f));
    ImSpinner::Spinner("SpinnerAng270NoBg", config);
    ImGui::SameLine();
    ImGui::BeginChild("##load_text");
    ImGui::Text("Loading...");
    ImGui::TextUnformatted(fmt::format("{}", notif.title).c_str());
    ImGui::EndChild();
  }
  ImGui::EndChild();

  ImGui::PopStyleVar();
}
} // namespace ox
