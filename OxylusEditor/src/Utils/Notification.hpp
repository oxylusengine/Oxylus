#pragma once
#include <chrono>
#include <imgui.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Core/Option.hpp"
#include "Core/Types.hpp"

namespace ox {
struct Notification {
  std::string title;
  bool completed = false;
  std::chrono::steady_clock::time_point created_at;
  // steady_clock has no epoch to format against, so the log keeps a wall clock copy for display
  std::chrono::system_clock::time_point wall_time;
  // handed out when the entry lands in the history and kept when it collapses, so a panel selection
  // survives both repeats and history trimming
  u64 id = 0;
  // consecutive identical entries collapse into one history row instead of flooding it
  u32 repeat_count = 1;
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
        wall_time(std::chrono::system_clock::now()),
        type(type_) {}
};

struct NotificationSystem {
  std::unordered_map<std::string, Notification> active_notifications;
  std::vector<Notification> notification_history = {};
  u64 next_notification_id = 1;

  // `add` is reachable from any thread: it is what the loguru callback calls, and a project scan
  // logs a registration per asset from its job workers. So it only ever queues here, and
  // `drain_pending` -- main thread, once a frame -- is the single writer of the two containers
  // above, which the panels read without a lock. `clear_history` is the one other writer, and the
  // panel that calls it runs on the main thread too.
  std::mutex pending_mutex = {};
  std::vector<Notification> pending = {};

  auto add(this NotificationSystem& self, Notification&& notif) -> void;
  auto drain_pending(this NotificationSystem& self) -> void;
  auto clear_history(this NotificationSystem& self) -> void;
  auto get_last_notification(this NotificationSystem& self) -> option<Notification>;
  auto draw(this NotificationSystem& self) -> void;
  auto draw_single(this NotificationSystem& self, Notification& notif) -> void;
};

} // namespace ox
