#pragma once
#include <chrono>
#include <imgui.h>
#include <mutex>
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

  // `add` is reachable from any thread: it is what the loguru callback calls, and a project scan
  // logs a registration per asset from its job workers. So it only ever queues here, and
  // `drain_pending` -- main thread, once a frame -- is the single writer of the two containers
  // above, which the panels read without a lock.
  std::mutex pending_mutex = {};
  std::vector<Notification> pending = {};

  auto add(this NotificationSystem& self, Notification&& notif) -> void;
  auto drain_pending(this NotificationSystem& self) -> void;
  auto get_last_notification(this NotificationSystem& self) -> option<Notification>;
  auto draw(this NotificationSystem& self) -> void;
  auto draw_single(this NotificationSystem& self, Notification& notif) -> void;
};

} // namespace ox
