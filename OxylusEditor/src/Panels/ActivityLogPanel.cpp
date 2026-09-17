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

static auto format_time(const std::chrono::system_clock::time_point time) -> std::string {
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
  return fmt::format("{:%H:%M:%S}.{:03}", local_time, millis.count());
}

// rows are one line tall, so only the first line ever reaches the list; the rest lives in the
// tooltip and the details pane
static auto first_line(const std::string& text) -> std::string_view {
  const auto view = std::string_view(text);
  const auto end = view.find('\n');
  return end == std::string_view::npos ? view : view.substr(0, end);
}

ActivityLogPanel::ActivityLogPanel() : EditorPanelState("Activity Log", ICON_MDI_FORUM, false) {
  // log lines are wide and short, the default portrait panel size wastes both dimensions
  window_default_size = {760.0f, 540.0f};
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

    const auto status_height = ImGui::GetTextLineHeightWithSpacing() + style.ItemSpacing.y;
    const auto details_height = self.show_details ? UI::scale(120.0f) + style.ItemSpacing.y : 0.0f;
    const auto list_height = ox::max(
      ImGui::GetContentRegionAvail().y - status_height - details_height,
      UI::scale(64.0f)
    );

    self.draw_rows(list_height);

    if (self.show_details) {
      self.draw_details(UI::scale(120.0f));
    }

    const auto filtered = self.visible_rows.size() != history.size();
    ImGui::TextDisabled(
      "%s",
      stack.format_char("{} of {} entries{}", self.visible_rows.size(), history.size(), filtered ? " (filtered)" : "")
    );

    // the list lives in a child window, so route the copy off the whole panel instead
    if (
      ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
      ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)
    ) {
      self.copy_to_clipboard(self.selected_id != 0);
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
    selection_alive |= notif.id == self.selected_id;

    // counted before the filters so the toggles keep reading as totals while a filter is on
    self.type_counts[static_cast<usize>(notif.type)] += 1;

    if ((self.type_mask & (1u << static_cast<u32>(notif.type))) == 0) {
      continue;
    }
    if (!self.log_filter.PassFilter(notif.title.c_str())) {
      continue;
    }

    self.visible_rows.emplace_back(i);
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

  for (u32 i = 0; i < TYPE_COUNT; i++) {
    const auto type = static_cast<Notification::Type>(i);
    const auto bit = 1u << i;
    const auto enabled = (self.type_mask & bit) != 0;

    if (i > 0) {
      ImGui::SameLine();
    }

    ImGui::PushStyleColor(
      ImGuiCol_Text,
      enabled ? type_color(type).Value : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)
    );
    const auto toggled = UI::toggle_button(
      stack.format_char("{} {}###log_type_{}", type_icon(type), self.type_counts[i], i),
      enabled,
      {UI::scale(62.0f), button_size.y},
      1.f,
      1.f,
      ImGuiButtonFlags_None,
      ImGuiCol_Header
    );
    ImGui::PopStyleColor();
    UI::tooltip_hover(
      stack.format_char("{}: {} entries\nClick to toggle, ctrl+click to isolate", type_label(type), self.type_counts[i])
    );

    if (toggled) {
      // isolating one severity is what chasing an error looks like, so it gets the modifier
      self.type_mask = ImGui::GetIO().KeyCtrl ? bit : self.type_mask ^ bit;
    }
  }

  ImGui::SameLine();

  const auto search_cursor_x = ImGui::GetCursorPosX();
  const auto trailing_width = (button_size.x + style.ItemSpacing.x) * 3.0f;
  const auto search_width = ox::max(ImGui::GetContentRegionAvail().x - trailing_width, UI::scale(100.0f));
  self.log_filter.Draw("###log_search", search_width);
  if (!self.log_filter.IsActive()) {
    ImGui::SameLine();
    ImGui::SetCursorPosX(search_cursor_x + ImGui::GetFontSize() * 0.5f);
    ImGui::BeginDisabled();
    ImGui::TextUnformatted(stack.format_char(" {} Search messages...", ICON_MDI_MAGNIFY));
    ImGui::EndDisabled();
  }

  ImGui::SameLine();
  ImGui::SetCursorPosX(search_cursor_x + search_width + style.ItemSpacing.x);
  if (UI::button(stack.format_char("{}###log_search_clear", ICON_MDI_CLOSE), button_size)) {
    self.log_filter.Clear();
  }
  UI::tooltip_hover("Clear the search");

  ImGui::SameLine();
  if (UI::button(stack.format_char("{}###log_options", ICON_MDI_COG), button_size)) {
    ImGui::OpenPopup("###log_options_popup");
  }
  UI::tooltip_hover("Options");

  ImGui::SameLine();
  if (UI::button(stack.format_char("{}###log_clear", ICON_MDI_DELETE_SWEEP), button_size)) {
    self.notification_system->clear_history();
    self.selected_id = 0;
    self.visible_rows.clear();
  }
  UI::tooltip_hover("Clear the log");

  if (ImGui::BeginPopup("###log_options_popup")) {
    ImGui::MenuItem(stack.format_char("{} Timestamps", ICON_MDI_CLOCK_OUTLINE), nullptr, &self.show_timestamps);
    ImGui::MenuItem(stack.format_char("{} Monospace text", ICON_MDI_FORMAT_FONT), nullptr, &self.monospace);
    ImGui::MenuItem(stack.format_char("{} Details pane", ICON_MDI_TEXT_BOX_OUTLINE), nullptr, &self.show_details);
    ImGui::MenuItem(stack.format_char("{} Newest first", ICON_MDI_SORT_CLOCK_DESCENDING), nullptr, &self.newest_first);
    ImGui::MenuItem(stack.format_char("{} Follow new entries", ICON_MDI_ARROW_EXPAND_DOWN), nullptr, &self.auto_scroll);

    ImGui::Separator();

    if (ImGui::MenuItem(stack.format_char("{} Copy visible entries", ICON_MDI_CONTENT_COPY))) {
      self.copy_to_clipboard(false);
    }
    if (ImGui::MenuItem(stack.format_char("{} Show all severities", ICON_MDI_FILTER_OFF_OUTLINE))) {
      self.type_mask = ALL_TYPES_MASK;
    }

    ImGui::Separator();
    ImGui::TextDisabled("Search matches `a,b` and excludes `-a`");

    ImGui::EndPopup();
  }
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

  // a fixed column width covers its own cell padding, so the glyph needs it added or it clips
  ImGui::TableSetupColumn(
    "###log_severity",
    ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize,
    ImGui::CalcTextSize(ICON_MDI_ALERT_CIRCLE).x + style.CellPadding.x * 2.0f
  );
  ImGui::TableSetupColumn(
    "###log_time",
    ImGuiTableColumnFlags_WidthFixed | (self.show_timestamps ? 0 : ImGuiTableColumnFlags_Disabled),
    UI::scale(88.0f)
  );
  ImGui::TableSetupColumn("###log_message", ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableSetupColumn(
    "###log_repeat",
    ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize,
    UI::scale(42.0f)
  );

  const auto row_height = ImGui::GetTextLineHeight() + style.CellPadding.y * 2.0f;
  const auto at_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
  const auto at_top = ImGui::GetScrollY() <= 1.0f;

  // looked up once instead of per row: the module registry lookup is a hash probe and a profiler zone
  auto* mono_font = self.monospace ? App::mod<Editor>().editor_theme.mono_font : nullptr;

  auto clipper = ImGuiListClipper();
  clipper.Begin(static_cast<i32>(self.visible_rows.size()), row_height);
  while (clipper.Step()) {
    for (auto row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
      self.draw_row(history[self.visible_rows[static_cast<usize>(row)]], row_height, mono_font);
    }
  }

  // following the newest entry is only welcome while the user has not scrolled away from it
  if (self.auto_scroll && self.visible_rows.size() != self.last_visible_count) {
    if (self.newest_first && at_top) {
      ImGui::SetScrollY(0.0f);
    } else if (!self.newest_first && at_bottom) {
      ImGui::SetScrollY(ImGui::GetScrollMaxY());
    }
  }
  self.last_visible_count = self.visible_rows.size();

  if (self.visible_rows.empty()) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(2);
    ImGui::TextDisabled("%s", history.empty() ? "Nothing has happened yet." : "No entry matches the filters.");
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

  constexpr auto SELECTABLE_FLAGS = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap;
  const auto cursor = ImGui::GetCursorPos();
  // the row height is forced on TableNextRow, so the selectable takes the plain text height or it
  // would add its own padding on top and drift out of the clipper's fixed pitch
  if (ImGui::Selectable("###log_row", notif.id == self.selected_id, SELECTABLE_FLAGS, {0.0f, text_height})) {
    self.selected_id = notif.id;
  }
  const auto hovered = ImGui::IsItemHovered();

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
    ImGui::TextUnformatted(format_time(notif.wall_time).c_str());
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

  ImGui::BeginChild("###log_details", {0.0f, height}, ImGuiChildFlags_Borders);

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
        format_time(notif->wall_time),
        notif->repeat_count > 1 ? stack.format("  repeated {} times", notif->repeat_count) : std::string_view()
      )
    );

    const auto copy_label = stack.format_char("{} Copy", ICON_MDI_CONTENT_COPY);
    ImGui::SameLine();
    UI::align_right(ImGui::CalcTextSize(copy_label).x + ImGui::GetStyle().FramePadding.x * 2.0f);
    if (UI::button(copy_label)) {
      ImGui::SetClipboardText(notif->title.c_str());
    }

    ImGui::Separator();

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
  }

  ImGui::EndChild();
}

auto ActivityLogPanel::draw_context_menu(this ActivityLogPanel& self, const Notification& notif) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  if (ImGui::MenuItem(stack.format_char("{} Copy message", ICON_MDI_CONTENT_COPY))) {
    ImGui::SetClipboardText(notif.title.c_str());
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
    self.notification_system->clear_history();
    self.selected_id = 0;
    self.visible_rows.clear();
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

auto ActivityLogPanel::copy_to_clipboard(this const ActivityLogPanel& self, const bool only_selected) -> void {
  ZoneScoped;

  auto text = std::string();
  const auto append = [&text](const Notification& notif) {
    text += fmt::format("[{}] [{}] {}", format_time(notif.wall_time), type_label(notif.type), notif.title);
    if (notif.repeat_count > 1) {
      text += fmt::format(" (x{})", notif.repeat_count);
    }
    text += '\n';
  };

  if (only_selected) {
    const auto* notif = self.find_selected();
    if (notif == nullptr) {
      return;
    }
    append(*notif);
  } else {
    const auto& history = self.notification_system->notification_history;
    for (const auto row : self.visible_rows) {
      append(history[row]);
    }
  }

  if (!text.empty()) {
    ImGui::SetClipboardText(text.c_str());
  }
}
} // namespace ox
