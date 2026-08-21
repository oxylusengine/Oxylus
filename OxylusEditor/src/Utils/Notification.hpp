#pragma once
#include <chrono>
#include <imgui.h>
#include <string>
#include <unordered_map>

#include "Core/Option.hpp"

namespace ox {
struct Notification {
  std::string title;
  bool completed = false;
  std::chrono::steady_clock::time_point created_at;
  enum Type {
    Info,
    Warn,
    Error,
    Loading,
  } type;

  explicit Notification(std::string_view title_, bool completed_, Type type_)
      : title(title_),
        completed(completed_),
        created_at(std::chrono::steady_clock::now()),
        type(type_) {}
};

struct NotificationSystem {
  std::unordered_map<std::string, Notification> active_notifications;
  std::vector<Notification> notification_history = {};

  auto add(this NotificationSystem& self, Notification&& notif) -> void;
  auto get_last_notification(this NotificationSystem& self) -> option<Notification>;
  auto draw(this NotificationSystem& self) -> void;
  auto draw_single(this NotificationSystem& self, Notification& notif) -> void;
};

} // namespace ox
