#pragma once

#include <array>
#include <string_view>
#include <vector>
#include <vuk/ImageAttachment.hpp>

#include "Panels/EditorPanelState.hpp"
#include "Utils/Notification.hpp"

namespace ox {
class ActivityLogPanel : public EditorPanelState {
public:
  ActivityLogPanel();

  auto set_system(this ActivityLogPanel& self, NotificationSystem* system) -> void;

  auto on_update(this ActivityLogPanel& self) -> void {}
  auto on_render(this ActivityLogPanel& self, vuk::ImageAttachment swapchain_attachment) -> void;

private:
  static constexpr u32 TYPE_COUNT = 4;
  static constexpr u32 ALL_TYPES_MASK = (1u << TYPE_COUNT) - 1u;

  auto rebuild_rows(this ActivityLogPanel& self) -> void;
  auto draw_toolbar(this ActivityLogPanel& self) -> void;
  auto draw_rows(this ActivityLogPanel& self, f32 height) -> void;
  auto draw_row(this ActivityLogPanel& self, const Notification& notif, f32 row_height, ImFont* mono_font) -> void;
  auto draw_details(this ActivityLogPanel& self, f32 height) -> void;
  auto draw_context_menu(this ActivityLogPanel& self, const Notification& notif) -> void;
  auto find_selected(this const ActivityLogPanel& self) -> const Notification*;
  auto copy_to_clipboard(this ActivityLogPanel& self, bool only_selected) -> void;
  auto copy_text(this ActivityLogPanel& self, std::string_view text) -> void;
  auto clear_history(this ActivityLogPanel& self) -> void;

  ImGuiTextFilter log_filter = {};
  NotificationSystem* notification_system = nullptr;

  // indices into the history, in display order; rebuilt every frame from the filters
  std::vector<u32> visible_rows = {};
  std::array<u32, TYPE_COUNT> type_counts = {};
  u32 type_mask = ALL_TYPES_MASK;
  u64 last_history_revision = 0;
  u32 last_repeat_count = 0;
  f32 details_height = 160.0f;
  f64 copy_feedback_until = 0.0;

  u64 selected_id = 0; // Notification::id, 0 when nothing is selected
  bool auto_scroll = true;
  bool newest_first = false;
  bool show_timestamps = true;
  bool monospace = true;
  bool show_details = true;
  bool following = true;
  bool jump_to_latest = true;
  bool clear_requested = false;
  bool copy_failed = false;
};
} // namespace ox
