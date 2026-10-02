#include "Panels/ActivityLogPanel.hpp"

#include <algorithm>
#include <fmt/chrono.h>
#include <fmt/format.h>
#include <icons/IconsMaterialDesignIcons.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <ranges>
#include <tracy/Tracy.hpp>

#include "Core/App.hpp"
#include "Editor.hpp"
#include "Memory/Stack.hpp"
#include "UI/UI.hpp"
#include "Utils/EditorTheme.hpp"

namespace ox {
static auto type_icon(const Notification::Type type) -> const c8* {
  switch (type) {
    case Notification::Info   : return ICON_MDI_INFORMATION;
    case Notification::Warn   : return ICON_MDI_ALERT;
    case Notification::Error  : return ICON_MDI_ALERT_CIRCLE;
    case Notification::Loading: return ICON_MDI_PROGRESS_CLOCK;
  }

  return ICON_MDI_INFORMATION;
}

static auto type_color(const Notification::Type type) -> ImColor {
  switch (type) {
    case Notification::Info   : return Gruvbox::bright_blue;
    case Notification::Warn   : return Gruvbox::bright_yellow;
    case Notification::Error  : return Gruvbox::bright_red;
    case Notification::Loading: return Gruvbox::bright_aqua;
  }

  return Gruvbox::bright_blue;
}

static auto type_label(const Notification::Type type) -> const c8* {
  switch (type) {
    case Notification::Info   : return "Info";
    case Notification::Warn   : return "Warning";
    case Notification::Error  : return "Error";
    case Notification::Loading: return "Task";
  }

  return "Info";
}

static auto format_time(memory::ScopedStack& stack, const std::chrono::system_clock::time_point time)
  -> std::string_view {
  const auto as_time_t = std::chrono::system_clock::to_time_t(time);
  std::tm local_time = {};
#if defined(_WIN32)
  if (localtime_s(&local_time, &as_time_t) != 0)
    return "--:--:--";
#else
  if (localtime_r(&as_time_t, &local_time) == nullptr)
    return "--:--:--";
#endif

  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()) % 1000;
  return stack.format("{:%H:%M:%S}.{:03}", local_time, millis.count());
}

// rows are one line tall, so only the first line ever reaches the list; the rest lives in the
// tooltip and the details pane
static auto first_line(const std::string_view text) -> std::string_view {
  return text.substr(0, text.find_first_of("\r\n"));
}

static auto same_line_if_fits(const f32 width) -> void {
  const auto next_x = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x;
  if (next_x + width <= ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x) {
    ImGui::SameLine();
  }
}

ActivityLogPanel::ActivityLogPanel() : EditorPanelState("Activity Log", ICON_MDI_FORUM, false) {
  auto& window = App::get_window();
  auto width = window.get_logical_width() * 0.6f;
  auto height = window.get_logical_height() * 0.75f;
  window_default_size = {width, height};
  window_center_at_appear = true;
}

auto ActivityLogPanel::set_system(this ActivityLogPanel& self, NotificationSystem* system) -> void {
  ZoneScoped;

  self.notification_system = system;
}

auto ActivityLogPanel::on_render(this ActivityLogPanel& self, vuk::ImageAttachment swapchain_attachment) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  if (self.on_begin() && self.notification_system != nullptr) {
    const auto& style = ImGui::GetStyle();
    const auto& history = self.notification_system->notification_history;

    self.rebuild_rows();
    self.draw_toolbar();
    if (self.clear_requested) {
      self.clear_history();
    }

    const auto status_height = ImGui::GetTextLineHeightWithSpacing() + style.ItemSpacing.y;
    const auto available_height = ox::max(ImGui::GetContentRegionAvail().y - status_height, 1.0f);
    const auto min_list_height = UI::scale(64.0f);
    const auto min_details_height = UI::scale(80.0f);
    const auto splitter_height = UI::scale(5.0f);
    const auto has_details = self.show_details && available_height >= min_list_height + min_details_height +
                                                                        splitter_height + style.ItemSpacing.y * 2.0f;
    auto details_height = has_details
                            ? std::clamp(
                                UI::scale(self.details_height),
                                min_details_height,
                                available_height - min_list_height - splitter_height - style.ItemSpacing.y * 2.0f
                              )
                            : 0.0f;
    auto list_height = available_height -
                       (has_details ? details_height + splitter_height + style.ItemSpacing.y * 2.0f : 0.0f);

    self.draw_rows(list_height);
    // context menus run inside the row loop, so clearing waits until no row holds a history reference
    if (self.clear_requested) {
      self.clear_history();
    }

    if (has_details) {
      const auto splitter_pos = ImGui::GetCursorScreenPos();
      const auto splitter_rect = ImRect(
        splitter_pos,
        {splitter_pos.x + ImGui::GetContentRegionAvail().x, splitter_pos.y + splitter_height}
      );
      if (
        ImGui::SplitterBehavior(
          splitter_rect,
          ImGui::GetID("###log_details_splitter"),
          ImGuiAxis_Y,
          &list_height,
          &details_height,
          min_list_height,
          min_details_height
        )
      ) {
        self.details_height = details_height / App::get_ui_scale();
      }
      ImGui::Dummy({0.0f, splitter_height});
      self.draw_details(details_height);
    }

    const auto filtered = self.visible_rows.size() != history.size();
    if (ImGui::GetTime() < self.copy_feedback_until) {
      ImGui::TextColored(
        self.copy_failed ? Gruvbox::bright_red.Value : Gruvbox::bright_green.Value,
        "%s",
        self.copy_failed ? "Could not copy to the clipboard" : "Copied to clipboard"
      );
    } else {
      ImGui::TextDisabled(
        "%s",
        stack.format_char(
          "{} of {} entries{}{}",
          self.visible_rows.size(),
          history.size(),
          filtered ? " (filtered)" : "",
          self.auto_scroll && !self.following ? "  |  Follow paused" : ""
        )
      );
    }

    // shortcut routing leaves text-field copy alone and uses cmd on macOS
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C)) {
      self.copy_to_clipboard(true);
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_C)) {
      self.copy_to_clipboard(false);
    }
  }

  self.on_end();
}

auto ActivityLogPanel::rebuild_rows(this ActivityLogPanel& self) -> void {
  ZoneScoped;

  const auto& history = self.notification_system->notification_history;

  self.visible_rows.clear();
  self.type_counts = {};

  auto selection_alive = false;
  for (u32 i = 0; i < static_cast<u32>(history.size()); i++) {
    const auto& notif = history[i];
    // counted before the filters so the toggles keep reading as totals while a filter is on
    self.type_counts[static_cast<usize>(notif.type)] += 1;

    if ((self.type_mask & (1u << static_cast<u32>(notif.type))) == 0) {
      continue;
    }
    if (!self.log_filter.PassFilter(notif.title.c_str())) {
      continue;
    }

    self.visible_rows.emplace_back(i);
    selection_alive |= notif.id == self.selected_id;
  }

  if (!selection_alive) {
    self.selected_id = 0;
  }

  if (self.newest_first) {
    std::ranges::reverse(self.visible_rows);
  }
}

auto ActivityLogPanel::draw_toolbar(this ActivityLogPanel& self) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  const auto& style = ImGui::GetStyle();
  const auto button_size = ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight());
  const auto search_width = ox::max(
    ImGui::GetContentRegionAvail().x - (button_size.x + style.ItemSpacing.x) * 2.0f,
    1.0f
  );

  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_F)) {
    ImGui::SetKeyboardFocusHere();
  }
  ImGui::SetNextItemWidth(search_width);
  if (
    ImGui::InputTextWithHint(
      "###log_search",
      "Search messages...",
      self.log_filter.InputBuf,
      IM_ARRAYSIZE(self.log_filter.InputBuf)
    )
  ) {
    self.log_filter.Build();
  }
  UI::tooltip_hover("Search the full message. Use commas for alternatives and -word to exclude. (Ctrl/Cmd+F)");

  ImGui::SameLine();
  ImGui::BeginDisabled(!self.log_filter.IsActive());
  if (UI::button(stack.format_char("{}###log_search_clear", ICON_MDI_CLOSE), button_size)) {
    self.log_filter.Clear();
  }
  ImGui::EndDisabled();
  UI::tooltip_hover("Clear search");

  ImGui::SameLine();
  if (UI::button(stack.format_char("{}###log_options", ICON_MDI_COG), button_size)) {
    ImGui::OpenPopup("###log_options_popup");
  }
  UI::tooltip_hover("Display options");

  if (ImGui::BeginPopup("###log_options_popup")) {
    ImGui::MenuItem(stack.format_char("{} Timestamps", ICON_MDI_CLOCK_OUTLINE), nullptr, &self.show_timestamps);
    ImGui::MenuItem(stack.format_char("{} Monospace text", ICON_MDI_FORMAT_FONT), nullptr, &self.monospace);
    ImGui::MenuItem(stack.format_char("{} Details pane", ICON_MDI_TEXT_BOX_OUTLINE), nullptr, &self.show_details);
    if (
      ImGui::MenuItem(stack.format_char("{} Newest first", ICON_MDI_SORT_CLOCK_DESCENDING), nullptr, &self.newest_first)
    ) {
      self.jump_to_latest = true;
    }

    ImGui::Separator();
    if (ImGui::MenuItem(stack.format_char("{} Reset filters", ICON_MDI_FILTER_OFF_OUTLINE))) {
      self.type_mask = ALL_TYPES_MASK;
      self.log_filter.Clear();
    }
    ImGui::EndPopup();
  }

  for (u32 i = 0; i < TYPE_COUNT; i++) {
    const auto type = static_cast<Notification::Type>(i);
    const auto bit = 1u << i;
    const auto enabled = (self.type_mask & bit) != 0;
    const auto
      label = stack.format_char("{} {} {}###log_type_{}", type_icon(type), type_label(type), self.type_counts[i], i);
    const auto width = ImGui::CalcTextSize(label, nullptr, true).x + style.FramePadding.x * 2.0f;
    if (i > 0) {
      same_line_if_fits(width);
    }

    ImGui::PushStyleColor(
      ImGuiCol_Text,
      enabled ? type_color(type).Value : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)
    );
    const auto toggled =
      UI::toggle_button(label, enabled, {width, button_size.y}, 1.0f, 1.0f, ImGuiButtonFlags_None, ImGuiCol_Header);
    ImGui::PopStyleColor();
    UI::tooltip_hover(stack.format_char(
      "{}: {} entries. Click to toggle; Ctrl/Cmd+click to show only this type.",
      type_label(type),
      self.type_counts[i]
    ));
    if (toggled) {
      self.type_mask = ImGui::GetIO().KeyCtrl ? bit : self.type_mask ^ bit;
    }
  }

  // rebuild after editing the filters so both the list and copy use the current results
  self.rebuild_rows();

  const auto copy_label = stack.format_char("{} Copy visible###log_copy_visible", ICON_MDI_CONTENT_COPY);
  same_line_if_fits(ImGui::CalcTextSize(copy_label, nullptr, true).x + style.FramePadding.x * 2.0f);
  ImGui::BeginDisabled(self.visible_rows.empty());
  if (UI::button(copy_label)) {
    self.copy_to_clipboard(false);
  }
  ImGui::EndDisabled();
  UI::tooltip_hover("Copy the filtered entries in display order (Ctrl/Cmd+Shift+C)");

  const auto follow_label = stack.format_char("{} Follow###log_follow", ICON_MDI_ARROW_EXPAND_DOWN);
  same_line_if_fits(ImGui::CalcTextSize(follow_label, nullptr, true).x + style.FramePadding.x * 2.0f);
  if (UI::toggle_button(follow_label, self.auto_scroll)) {
    self.auto_scroll = !self.auto_scroll;
    self.jump_to_latest = self.auto_scroll;
  }
  UI::tooltip_hover("Follow new entries. Scrolling away pauses following; Latest resumes it.");

  same_line_if_fits(button_size.x);
  ImGui::BeginDisabled(self.visible_rows.empty());
  if (UI::button(stack.format_char("{}###log_latest", ICON_MDI_ARROW_COLLAPSE_DOWN), button_size)) {
    self.jump_to_latest = true;
  }
  ImGui::EndDisabled();
  UI::tooltip_hover("Jump to the latest visible entry");

  same_line_if_fits(button_size.x);
  ImGui::BeginDisabled(self.notification_system->notification_history.empty());
  if (UI::button(stack.format_char("{}###log_clear", ICON_MDI_DELETE_SWEEP), button_size)) {
    self.clear_requested = true;
  }
  ImGui::EndDisabled();
  UI::tooltip_hover("Clear the log");
}

auto ActivityLogPanel::draw_rows(this ActivityLogPanel& self, const f32 height) -> void {
  ZoneScoped;

  constexpr auto TABLE_FLAGS = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV |
                               ImGuiTableFlags_NoBordersInBody | ImGuiTableFlags_Resizable;

  if (!ImGui::BeginTable("###log_table", 4, TABLE_FLAGS, {0.0f, height})) {
    return;
  }

  const auto& style = ImGui::GetStyle();
  const auto& history = self.notification_system->notification_history;
  const auto timestamps_fit = self.show_timestamps && ImGui::GetContentRegionAvail().x >= UI::scale(480.0f);

  // a fixed column width covers its own cell padding, so the glyph needs it added or it clips
  ImGui::TableSetupColumn(
    "###log_severity",
    ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize,
    ImGui::CalcTextSize(ICON_MDI_ALERT_CIRCLE).x + style.CellPadding.x * 2.0f
  );
  ImGui::TableSetupColumn(
    "Time",
    ImGuiTableColumnFlags_WidthFixed | (timestamps_fit ? 0 : ImGuiTableColumnFlags_Disabled),
    ImGui::CalcTextSize("00:00:00.000").x + style.CellPadding.x * 2.0f
  );
  ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableSetupColumn(
    "Count",
    ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize,
    ImGui::CalcTextSize("Count").x + style.CellPadding.x * 2.0f
  );
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableHeadersRow();

  const auto row_height = ImGui::GetTextLineHeight() + style.CellPadding.y * 2.0f;
  const auto at_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
  const auto at_top = ImGui::GetScrollY() <= 1.0f;
  self.following = self.newest_first ? at_top : at_bottom;
  const auto latest_repeat_count = history.empty() ? 0 : history.back().repeat_count;
  const auto history_changed = self.last_history_revision != self.notification_system->next_notification_id ||
                               self.last_repeat_count != latest_repeat_count;
  const auto scroll_to_latest = self.jump_to_latest || (self.auto_scroll && self.following && history_changed);

  // looked up once instead of per row: the module registry lookup is a hash probe and a profiler zone
  auto* mono_font = self.monospace ? App::mod<Editor>().editor_theme.mono_font : nullptr;

  auto clipper = ImGuiListClipper();
  clipper.Begin(static_cast<i32>(self.visible_rows.size()), row_height);
  if (scroll_to_latest && !self.newest_first && !self.visible_rows.empty()) {
    clipper.IncludeItemByIndex(static_cast<i32>(self.visible_rows.size()) - 1);
  }
  while (clipper.Step()) {
    for (auto row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
      self.draw_row(history[self.visible_rows[static_cast<usize>(row)]], row_height, mono_font);
    }
  }

  // following the newest entry is only welcome while the user has not scrolled away from it
  if (scroll_to_latest) {
    if (self.newest_first) {
      ImGui::SetScrollY(0.0f);
    } else if (!self.visible_rows.empty()) {
      ImGui::SetScrollHereY(1.0f);
    }
    self.following = true;
  }
  self.jump_to_latest = false;
  self.last_history_revision = self.notification_system->next_notification_id;
  self.last_repeat_count = latest_repeat_count;

  if (self.visible_rows.empty()) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(2);
    ImGui::TextDisabled("%s", history.empty() ? "No activity yet" : "No matching entries");
    if (!history.empty() && ImGui::SmallButton("Reset filters")) {
      self.log_filter.Clear();
      self.type_mask = ALL_TYPES_MASK;
    }
  }

  ImGui::EndTable();
}

auto ActivityLogPanel::draw_row(
  this ActivityLogPanel& self, const Notification& notif, const f32 row_height, ImFont* mono_font
) -> void {
  memory::ScopedStack stack;

  const auto color = type_color(notif.type);

  const auto text_height = ImGui::GetTextLineHeight();

  ImGui::TableNextRow(ImGuiTableRowFlags_None, row_height);

  // a tinted row reads as a warning or an error before the glyph does
  if (notif.type == Notification::Warn || notif.type == Notification::Error) {
    auto tint = color.Value;
    tint.w = 0.09f;
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(tint));
  }

  ImGui::TableSetColumnIndex(0);
  ImGui::PushID(static_cast<i32>(notif.id));

  constexpr auto SELECTABLE_FLAGS = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap |
                                    ImGuiSelectableFlags_AllowDoubleClick;
  const auto cursor = ImGui::GetCursorPos();
  // the row height is forced on TableNextRow, so the selectable takes the plain text height or it
  // would add its own padding on top and drift out of the clipper's fixed pitch
  if (ImGui::Selectable("###log_row", notif.id == self.selected_id, SELECTABLE_FLAGS, {0.0f, text_height})) {
    self.selected_id = notif.id;
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      self.show_details = true;
    }
  }
  if (ImGui::IsItemFocused() && ImGui::GetIO().NavActive) {
    self.selected_id = notif.id;
  }
  const auto hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);

  if (ImGui::BeginPopupContextItem("###log_row_context")) {
    self.selected_id = notif.id;
    self.draw_context_menu(notif);
    ImGui::EndPopup();
  }

  // the selectable owns the cell, so the glyph is drawn back over it
  ImGui::SetCursorPos(cursor);
  ImGui::PushStyleColor(ImGuiCol_Text, color.Value);
  ImGui::TextUnformatted(type_icon(notif.type));
  ImGui::PopStyleColor();

  if (ImGui::TableSetColumnIndex(1)) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    const auto time = format_time(stack, notif.wall_time);
    ImGui::TextUnformatted(time.data(), time.data() + time.size());
    ImGui::PopStyleColor();
  }

  auto truncated = false;
  if (ImGui::TableSetColumnIndex(2)) {
    if (mono_font != nullptr) {
      ImGui::PushFont(mono_font, 0.0f);
    }

    const auto line = first_line(notif.title);
    const auto pos = ImGui::GetCursorScreenPos();
    const auto max_x = pos.x + ImGui::GetContentRegionAvail().x;
    // ellipsis rather than the table's hard clip, so a cut off line still reads as cut off
    ImGui::RenderTextEllipsis(
      ImGui::GetWindowDrawList(),
      pos,
      {max_x, pos.y + ImGui::GetTextLineHeight()},
      max_x,
      line.data(),
      line.data() + line.size(),
      nullptr
    );

    truncated = line.size() != notif.title.size() ||
                ImGui::CalcTextSize(line.data(), line.data() + line.size()).x > max_x - pos.x;

    if (mono_font != nullptr) {
      ImGui::PopFont();
    }
  }

  if (notif.repeat_count > 1 && ImGui::TableSetColumnIndex(3)) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted(stack.format_char("x{}", notif.repeat_count));
    ImGui::PopStyleColor();
    UI::tooltip_hover(stack.format_char("Repeated {} times", notif.repeat_count));
  }

  if (hovered && truncated) {
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(UI::scale(600.0f));
    ImGui::TextUnformatted(notif.title.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }

  ImGui::PopID();
}

auto ActivityLogPanel::draw_details(this ActivityLogPanel& self, const f32 height) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  ImGui::BeginChild(
    "###log_details",
    {0.0f, height},
    ImGuiChildFlags_Borders,
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
  );

  const auto* notif = self.find_selected();
  if (notif == nullptr) {
    ImGui::TextDisabled("Select an entry to read it in full.");
  } else {
    ImGui::PushStyleColor(ImGuiCol_Text, type_color(notif->type).Value);
    ImGui::TextUnformatted(stack.format_char("{} {}", type_icon(notif->type), type_label(notif->type)));
    ImGui::PopStyleColor();

    ImGui::SameLine();
    ImGui::TextDisabled(
      "%s",
      stack.format_char(
        "{}{}",
        format_time(stack, notif->wall_time),
        notif->repeat_count > 1 ? stack.format("  repeated {} times", notif->repeat_count) : std::string_view()
      )
    );

    const auto copy_label = stack.format_char("{} Copy", ICON_MDI_CONTENT_COPY);
    same_line_if_fits(ImGui::CalcTextSize(copy_label).x + ImGui::GetStyle().FramePadding.x * 2.0f);
    if (UI::button(copy_label)) {
      self.copy_text(notif->title);
    }
    UI::tooltip_hover("Copy the full message (Ctrl/Cmd+C)");

    ImGui::Separator();

    ImGui::PushID(static_cast<i32>(notif->id));
    ImGui::BeginChild("###log_message_body", {0.0f, 0.0f});
    auto* mono_font = self.monospace ? App::mod<Editor>().editor_theme.mono_font : nullptr;
    if (mono_font != nullptr) {
      ImGui::PushFont(mono_font, 0.0f);
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(notif->title.c_str());
    ImGui::PopTextWrapPos();
    if (mono_font != nullptr) {
      ImGui::PopFont();
    }
    ImGui::EndChild();
    ImGui::PopID();
  }

  ImGui::EndChild();
}

auto ActivityLogPanel::draw_context_menu(this ActivityLogPanel& self, const Notification& notif) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  if (ImGui::MenuItem(stack.format_char("{} Copy message", ICON_MDI_CONTENT_COPY))) {
    self.copy_text(notif.title);
  }
  if (ImGui::MenuItem(stack.format_char("{} Copy visible entries", ICON_MDI_CONTENT_COPY))) {
    self.copy_to_clipboard(false);
  }

  ImGui::Separator();

  if (ImGui::MenuItem(stack.format_char("{} Show only {}", ICON_MDI_FILTER_OUTLINE, type_label(notif.type)))) {
    self.type_mask = 1u << static_cast<u32>(notif.type);
  }
  if (ImGui::MenuItem(stack.format_char("{} Show all severities", ICON_MDI_FILTER_OFF_OUTLINE))) {
    self.type_mask = ALL_TYPES_MASK;
  }

  ImGui::Separator();

  if (ImGui::MenuItem(stack.format_char("{} Clear the log", ICON_MDI_DELETE_SWEEP))) {
    self.clear_requested = true;
  }
}

auto ActivityLogPanel::find_selected(this const ActivityLogPanel& self) -> const Notification* {
  ZoneScoped;

  if (self.selected_id == 0 || self.notification_system == nullptr) {
    return nullptr;
  }

  const auto& history = self.notification_system->notification_history;
  const auto it = std::ranges::find(history, self.selected_id, &Notification::id);
  return it == history.end() ? nullptr : &*it;
}

auto ActivityLogPanel::copy_to_clipboard(this ActivityLogPanel& self, const bool only_selected) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  if (only_selected) {
    if (const auto* notif = self.find_selected()) {
      self.copy_text(notif->title);
    }
    return;
  }
  if (self.notification_system == nullptr || self.visible_rows.empty()) {
    return;
  }

  const auto& history = self.notification_system->notification_history;
  const auto entry_text = [](memory::ScopedStack& entry_stack, const Notification& notif) -> std::string_view {
    return entry_stack.format(
      "[{}] [{}] {}{}\n",
      format_time(entry_stack, notif.wall_time),
      type_label(notif.type),
      notif.title,
      notif.repeat_count > 1 ? entry_stack.format(" (x{})", notif.repeat_count) : std::string_view()
    );
  };

  // size the scratch buffer exactly, without building a temporary heap string for every entry
  usize length = 0;
  for (const auto row : self.visible_rows) {
    memory::ScopedStack entry_stack;
    length += entry_text(entry_stack, history[row]).size();
  }
  const auto buffer = stack.alloc<c8>(length + 1);
  auto* cursor = buffer.data();
  for (const auto row : self.visible_rows) {
    memory::ScopedStack entry_stack;
    const auto entry = entry_text(entry_stack, history[row]);
    cursor = std::ranges::copy(entry, cursor).out;
  }
  *cursor = '\0';
  self.copy_text({buffer.data(), length});
}

auto ActivityLogPanel::copy_text(this ActivityLogPanel& self, const std::string_view text) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  ImGui::SetClipboardText(stack.null_terminate_cstr(text));
  const auto* clipboard = ImGui::GetClipboardText();
  self.copy_failed = clipboard == nullptr || std::string_view(clipboard) != text;
  self.copy_feedback_until = ImGui::GetTime() + 3.0;
}

auto ActivityLogPanel::clear_history(this ActivityLogPanel& self) -> void {
  ZoneScoped;

  self.notification_system->clear_history();
  self.selected_id = 0;
  self.visible_rows.clear();
  self.type_counts = {};
  self.clear_requested = false;
  self.jump_to_latest = true;
}
} // namespace ox
