#pragma once
#include <chrono>
#include <imgui.h>
#include <string>
#include <unordered_map>

#include "Core/Types.hpp"

namespace ox {
struct Notification {
  std::string title;
  bool completed = false;
  std::chrono::steady_clock::time_point created_at;

  explicit Notification(std::string_view title_, bool completed_)
      : title(title_),
        completed(completed_),
        created_at(std::chrono::steady_clock::now()) {}
};

struct NotificationSystem {
  std::unordered_map<std::string, Notification> active_notifications;

  auto add(Notification&& notif) -> void;
  auto draw() -> void;
  auto draw_single(Notification& notif, auto current_time, const ImVec2& screen_pos, f32 y_offset) -> void;
};

} // namespace ox
