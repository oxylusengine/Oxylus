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
  enum Type {
    Info,
    Warn,
    Error,
    Loading,
  } type;

  explicit Notification(std::string_view title_, bool completed_, Type type)
      : title(title_),
        completed(completed_),
        created_at(std::chrono::steady_clock::now()),
        type(type) {}
};

struct NotificationSystem {
  std::unordered_map<std::string, Notification> active_notifications;

  auto add(Notification&& notif) -> void;
  auto draw() -> void;
  auto draw_single(Notification& notif) -> void;
};

} // namespace ox
