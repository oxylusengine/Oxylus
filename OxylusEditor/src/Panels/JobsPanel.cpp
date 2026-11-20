#include "Panels/JobsPanel.hpp"

#include <algorithm>
#include <chrono>
#include <icons/IconsMaterialDesignIcons.h>
#include <imgui.h>
#include <implot.h>
#include <numeric>
#include <ranges>
#include <span>
#include <tracy/Tracy.hpp>
#include <utility>

#include "Core/App.hpp"
#include "Memory/Stack.hpp"
#include "UI/UI.hpp"
#include "Utils/EditorTheme.hpp"

namespace ox {
using JobClock = std::chrono::steady_clock;
using Nanos = std::chrono::nanoseconds;

constexpr auto OTHER_THREAD = ~0_u32;
constexpr auto STALL_ESCALATION = 4;

static auto format_duration(memory::ScopedStack& stack, const Nanos duration) -> std::string_view {
  using namespace std::chrono;

  if (duration < 1ms) {
    return stack.format("{} µs", duration_cast<microseconds>(duration).count());
  }

  if (duration < 1s) {
    return stack.format("{} ms", duration_cast<milliseconds>(duration).count());
  }

  if (duration < 60s) {
    return stack.format("{:.1f} s", duration_cast<std::chrono::duration<f64>>(duration).count());
  }

  const auto total_seconds = duration_cast<seconds>(duration).count();
  return stack.format("{}m {}s", total_seconds / 60, total_seconds % 60);
}

static auto short_file(const c8* path) -> std::string_view {
  const auto full = std::string_view(path);
  const auto separator = full.find_last_of("/\\");
  return separator == std::string_view::npos ? full : full.substr(separator + 1);
}

static auto location_label(memory::ScopedStack& stack, const std::source_location& location) -> const c8* {
  return stack.format_char("{}:{}", short_file(location.file_name()), location.line());
}

static auto passes_filter(
  const ImGuiTextFilter& filter, const std::string_view name, const std::source_location& location
) -> bool {
  if (!filter.IsActive()) {
    return true;
  }

  return filter.PassFilter(name.data(), name.data() + name.size()) || filter.PassFilter(location.file_name()) ||
         filter.PassFilter(location.function_name());
}

static auto is_active(const JobRecord& job) -> bool {
  return job.state == JobState::Running || job.state == JobState::Waiting;
}

static auto elapsed_of(const JobRecord& job, const JobClock::time_point now) -> Nanos {
  return std::chrono::duration_cast<Nanos>(now - job.started_at);
}

static auto stall_color(const Nanos elapsed, const JobClock::duration threshold) -> const ImVec4* {
  const auto limit = std::chrono::duration_cast<Nanos>(threshold);
  if (elapsed > limit * STALL_ESCALATION) {
    return &Gruvbox::bright_red.Value;
  }

  if (elapsed > limit) {
    return &Gruvbox::bright_yellow.Value;
  }

  return nullptr;
}

static auto state_color(const JobState state) -> const ImVec4* {
  switch (state) {
    case JobState::Queued   : return nullptr;
    case JobState::Running  : return &Gruvbox::bright_aqua.Value;
    case JobState::Waiting  : return &Gruvbox::bright_blue.Value;
    case JobState::Done     : return &Gruvbox::bright_green.Value;
    case JobState::Cancelled: return &Gruvbox::bright_orange.Value;
  }

  return nullptr;
}

static auto state_label(const JobState state) -> const c8* {
  switch (state) {
    case JobState::Queued   : return "Queued";
    case JobState::Running  : return "Running";
    case JobState::Waiting  : return "Waiting";
    case JobState::Done     : return "Done";
    case JobState::Cancelled: return "Cancelled";
  }

  return "Unknown";
}

static auto text_colored(const c8* text, const ImVec4* color) -> void {
  if (color != nullptr) {
    ImGui::PushStyleColor(ImGuiCol_Text, *color);
  }

  ImGui::TextUnformatted(text);

  if (color != nullptr) {
    ImGui::PopStyleColor();
  }
}

static auto draw_job_name(const JobRecord& job, const ImVec4* color) -> void {
  if (job.name.empty()) {
    ImGui::TextDisabled("<unnamed>");
    return;
  }

  text_colored(job.name.c_str(), color);
}

static auto draw_worker_cell(memory::ScopedStack& stack, const u32 worker) -> void {
  if (worker == OTHER_THREAD) {
    ImGui::TextDisabled("other");
    return;
  }

  ImGui::TextUnformatted(stack.format_char("{}", worker));
}

static auto begin_hover_row() -> bool {
  const auto cursor = ImGui::GetCursorPos();
  ImGui::Selectable(
    "##row",
    false,
    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
    {0.0f, ImGui::GetTextLineHeight()}
  );
  const auto is_hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
  ImGui::SetCursorPos(cursor);

  return is_hovered;
}

static auto draw_job_tooltip(const JobRecord& job, const JobClock::time_point now) -> void {
  memory::ScopedStack stack;

  if (!ImGui::BeginTooltip()) {
    return;
  }

  ImGui::TextUnformatted(stack.format_char("ID: {}", std::to_underlying(job.id)));
  if (job.parent != JobID::Invalid) {
    ImGui::TextUnformatted(stack.format_char("Parent: {}", std::to_underlying(job.parent)));
  }

  ImGui::PushTextWrapPos(UI::scale(500.0f));
  ImGui::TextUnformatted(stack.format_char("{}:{}", job.location.file_name(), job.location.line()));
  ImGui::TextUnformatted(job.location.function_name());
  ImGui::PopTextWrapPos();

  const auto submitted = std::chrono::duration_cast<Nanos>(now - job.submitted_at);
  ImGui::TextUnformatted(stack.format_char("Submitted: {} ago", format_duration(stack, submitted)));
  ImGui::TextUnformatted(stack.format_char("Barriers: {}", job.barrier_count));
  ImGui::EndTooltip();
}

static auto report_line(memory::ScopedStack& stack, const JobRecord& job, const JobClock::time_point now)
  -> std::string_view {
  const auto since = job.state == JobState::Queued ? job.submitted_at : job.started_at;
  const auto elapsed = std::chrono::duration_cast<Nanos>(now - since);
  const auto worker = job.worker == OTHER_THREAD ? std::string_view("other") : stack.format("{}", job.worker);

  return stack.format(
    "{} {} {} {}:{} worker={} elapsed={}\n",
    std::to_underlying(job.id),
    state_label(job.state),
    job.name.empty() ? std::string_view("<unnamed>") : std::string_view(job.name),
    job.location.file_name(),
    job.location.line(),
    worker,
    format_duration(stack, elapsed)
  );
}

static auto draw_active_row(
  const JobRecord& job,
  const JobClock::time_point now,
  const JobClock::duration threshold,
  const u32 depth,
  const c8* worker_text
) -> void {
  memory::ScopedStack stack;

  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::PushID(static_cast<i32>(std::to_underlying(job.id)));

  const auto is_hovered = begin_hover_row();
  if (worker_text != nullptr) {
    ImGui::TextUnformatted(worker_text);
  }

  const auto elapsed = elapsed_of(job, now);
  const auto* color = stall_color(elapsed, threshold);

  ImGui::TableSetColumnIndex(1);
  ImGui::Indent(UI::scale(14.0f) * static_cast<f32>(depth));
  draw_job_name(job, color);
  ImGui::Unindent(UI::scale(14.0f) * static_cast<f32>(depth));

  ImGui::TableSetColumnIndex(2);
  text_colored(state_label(job.state), state_color(job.state));

  ImGui::TableSetColumnIndex(3);
  if (job.state == JobState::Waiting) {
    const auto waiting = std::chrono::duration_cast<Nanos>(now - job.waiting_since);
    const auto elapsed_text = format_duration(stack, elapsed);
    text_colored(stack.format_char("{} (waiting {})", elapsed_text, format_duration(stack, waiting)), color);
  } else {
    text_colored(format_duration(stack, elapsed).data(), color);
  }

  ImGui::TableSetColumnIndex(4);
  ImGui::TextUnformatted(location_label(stack, job.location));

  if (is_hovered) {
    draw_job_tooltip(job, now);
  }

  ImGui::PopID();
}

JobsPanel::JobsPanel() : EditorPanelState("Jobs", ICON_MDI_COGS, false) {}

auto JobsPanel::on_render(this JobsPanel& self, const SwapchainInfo&) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  if (self.on_begin()) {
    auto& job_man = App::get_job_manager();
    auto& tracker = job_man.get_tracker();

    if (!tracker.is_tracking()) {
      ImGui::TextDisabled("Job tracking is off");
    } else {
      tracker.snapshot(self.snapshot);
      self.now = JobClock::now();
      self.stall_threshold = std::chrono::duration_cast<JobClock::duration>(
        std::chrono::duration<f32>(self.stall_seconds)
      );

      auto running = 0_u32;
      auto waiting = 0_u32;
      auto queued = 0_u32;
      auto stalled = 0_u32;
      for (const auto& job : self.snapshot.live) {
        switch (job.state) {
          case JobState::Queued : queued += 1; break;
          case JobState::Running: running += 1; break;
          case JobState::Waiting: waiting += 1; break;
          default               : break;
        }

        if (is_active(job) && elapsed_of(job, self.now) > self.stall_threshold) {
          stalled += 1;
        }
      }

      const auto worker_count = job_man.get_thread_count();
      self.record_sample(queued, running + waiting);
      self.draw_toolbar(worker_count, running, waiting, queued);
      self.draw_plot();

      if (ImGui::BeginTabBar("###jobs_tabs")) {
        const auto workers_label = stalled > 0 ? stack.format_char("Workers {} {}###workers", ICON_MDI_ALERT, stalled)
                                               : stack.format_char("Workers###workers");
        if (stalled > 0) {
          ImGui::PushStyleColor(ImGuiCol_Text, Gruvbox::bright_yellow.Value);
        }

        const auto is_workers_open = ImGui::BeginTabItem(workers_label);
        if (stalled > 0) {
          ImGui::PopStyleColor();
        }

        if (is_workers_open) {
          self.draw_workers(worker_count);
          ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem(stack.format_char("Queue ({})###queue", queued))) {
          self.draw_queue();
          ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("History###history")) {
          self.draw_history();
          ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Stats###stats")) {
          self.draw_stats();
          ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
      }
    }
  }

  self.on_end();
}

auto JobsPanel::record_sample(this JobsPanel& self, const u32 queued, const u32 active) -> void {
  const auto time = ImGui::GetTime();
  if (time - self.last_sample_time < SAMPLE_INTERVAL) {
    return;
  }

  self.last_sample_time = time;
  self.queued_samples[self.sample_head] = static_cast<f32>(queued);
  self.active_samples[self.sample_head] = static_cast<f32>(active);
  self.sample_head = (self.sample_head + 1) % SAMPLE_COUNT;
  self.sample_size = std::min(self.sample_size + 1, SAMPLE_COUNT);
}

auto JobsPanel::draw_toolbar(
  this JobsPanel& self, const u32 worker_count, const u32 running, const u32 waiting, const u32 queued
) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  ImGui::TextUnformatted(
    stack.format_char("{} workers · {} running · {} waiting · {} queued", worker_count, running, waiting, queued)
  );

  ImGui::SetNextItemWidth(UI::scale(120.0f));
  ImGui::DragFloat(
    "##jobs_stall",
    &self.stall_seconds,
    0.1f,
    0.5f,
    120.0f,
    "stall: %.1f s",
    ImGuiSliderFlags_AlwaysClamp
  );
  UI::tooltip_hover("Running jobs older than this are highlighted");

  ImGui::SameLine();
  self.filter.Draw("##jobs_filter", UI::scale(180.0f));

  ImGui::SameLine();
  if (UI::button(stack.format_char("{} Copy report###jobs_copy", ICON_MDI_CONTENT_COPY))) {
    self.copy_report();
  }

  ImGui::SameLine();
  if (UI::button(stack.format_char("{} Clear history###jobs_clear", ICON_MDI_DELETE_SWEEP))) {
    App::get_job_manager().get_tracker().clear_history();
    self.snapshot.history.clear();
  }
}

auto JobsPanel::draw_plot(this JobsPanel& self) -> void {
  ZoneScoped;

  constexpr auto PLOT_FLAGS = ImPlotFlags_NoTitle | ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect |
                              ImPlotFlags_NoInputs;
  constexpr auto WINDOW_SECONDS = static_cast<f64>(SAMPLE_COUNT) * SAMPLE_INTERVAL;

  if (!ImPlot::BeginPlot("##jobs_plot", {-1.0f, UI::scale(80.0f)}, PLOT_FLAGS)) {
    return;
  }

  ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoDecorations, ImPlotAxisFlags_AutoFit);
  ImPlot::SetupAxisLimits(ImAxis_X1, -WINDOW_SECONDS, 0.0, ImPlotCond_Always);
  ImPlot::SetupLegend(ImPlotLocation_NorthWest);

  if (self.sample_size > 0) {
    const auto count = static_cast<i32>(self.sample_size);
    const auto offset = self.sample_size == SAMPLE_COUNT ? static_cast<i32>(self.sample_head) : 0;
    const auto spec = ImPlotSpec(ImPlotProp_Offset, offset);
    const auto x_start = -static_cast<f64>(self.sample_size - 1) * SAMPLE_INTERVAL;
    ImPlot::PlotLine("Queued", self.queued_samples.data(), count, SAMPLE_INTERVAL, x_start, spec);
    ImPlot::PlotLine("Running", self.active_samples.data(), count, SAMPLE_INTERVAL, x_start, spec);
  }

  ImPlot::EndPlot();
}

auto JobsPanel::draw_workers(this JobsPanel& self, const u32 worker_count) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  constexpr auto TABLE_FLAGS = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV |
                               ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
  if (!ImGui::BeginTable("###workers_table", 5, TABLE_FLAGS)) {
    return;
  }

  ImGui::TableSetupColumn("Worker", ImGuiTableColumnFlags_WidthFixed, UI::scale(80.0f));
  ImGui::TableSetupColumn("Job", ImGuiTableColumnFlags_WidthStretch, 3.0f);
  ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, UI::scale(80.0f));
  ImGui::TableSetupColumn("Elapsed", ImGuiTableColumnFlags_WidthStretch, 2.0f);
  ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthStretch, 2.0f);
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableHeadersRow();

  auto active_count = usize{0};
  for (const auto& job : self.snapshot.live) {
    active_count += job.state != JobState::Queued ? 1 : 0;
  }

  const auto active = stack.alloc<const JobRecord*>(active_count);
  auto active_index = usize{0};
  for (const auto& job : self.snapshot.live) {
    if (job.state != JobState::Queued) {
      active[active_index++] = &job;
    }
  }

  const auto is_filtered = self.filter.IsActive();
  const auto find_child = [&](const JobRecord& parent) -> const JobRecord* {
    for (const auto* candidate : active) {
      if (candidate->worker == parent.worker && candidate->parent == parent.id) {
        return candidate;
      }
    }

    return nullptr;
  };
  const auto find_root = [&](const u32 worker) -> const JobRecord* {
    for (const auto* candidate : active) {
      if (candidate->worker != worker) {
        continue;
      }

      const auto is_nested = std::ranges::any_of(active, [&](const JobRecord* other) {
        return other->worker == worker && other->id == candidate->parent;
      });
      if (!is_nested) {
        return candidate;
      }
    }

    return nullptr;
  };

  for (u32 worker = 0; worker < worker_count; worker++) {
    const auto* root = find_root(worker);
    if (root == nullptr) {
      if (is_filtered) {
        continue;
      }

      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(stack.format_char("Worker {}", worker));
      ImGui::TableSetColumnIndex(1);
      ImGui::TextDisabled("Idle");
      continue;
    }

    auto is_first = true;
    auto depth = 0_u32;
    for (const auto* job = root; job != nullptr && depth <= active_count; job = find_child(*job), depth++) {
      if (!passes_filter(self.filter, job->name, job->location)) {
        continue;
      }

      draw_active_row(
        *job,
        self.now,
        self.stall_threshold,
        depth,
        is_first ? stack.format_char("Worker {}", worker) : nullptr
      );
      is_first = false;
    }
  }

  auto is_other_header_drawn = false;
  for (const auto* job : active) {
    if (job->worker != OTHER_THREAD || !passes_filter(self.filter, job->name, job->location)) {
      continue;
    }

    const auto has_parent = std::ranges::any_of(active, [&](const JobRecord* other) {
      return other->worker == OTHER_THREAD && other->id == job->parent;
    });
    draw_active_row(
      *job,
      self.now,
      self.stall_threshold,
      has_parent ? 1 : 0,
      is_other_header_drawn ? nullptr : "Other threads"
    );
    is_other_header_drawn = true;
  }

  ImGui::EndTable();
}

auto JobsPanel::draw_queue(this JobsPanel& self) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  constexpr auto TABLE_FLAGS = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV |
                               ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;

  self.visible_rows.clear();
  for (u32 i = 0; i < static_cast<u32>(self.snapshot.live.size()); i++) {
    const auto& job = self.snapshot.live[i];
    if (job.state == JobState::Queued && passes_filter(self.filter, job.name, job.location)) {
      self.visible_rows.push_back(i);
    }
  }

  if (ImGui::BeginTable("###queue_table", 6, TABLE_FLAGS)) {
    ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, UI::scale(70.0f));
    ImGui::TableSetupColumn("Job", ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn("Waited", ImGuiTableColumnFlags_WidthFixed, UI::scale(80.0f));
    ImGui::TableSetupColumn("Barriers", ImGuiTableColumnFlags_WidthFixed, UI::scale(70.0f));
    ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, UI::scale(150.0f));
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    const auto row_height = ImGui::GetTextLineHeight() + ImGui::GetStyle().CellPadding.y * 2.0f;
    auto clipper = ImGuiListClipper();
    clipper.Begin(static_cast<i32>(self.visible_rows.size()), row_height);
    while (clipper.Step()) {
      for (auto row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
        memory::ScopedStack row_stack;

        const auto& job = self.snapshot.live[self.visible_rows[static_cast<usize>(row)]];
        ImGui::TableNextRow(ImGuiTableRowFlags_None, row_height);
        ImGui::TableSetColumnIndex(0);
        ImGui::PushID(static_cast<i32>(std::to_underlying(job.id)));

        const auto is_row_hovered = begin_hover_row();
        ImGui::TextUnformatted(row_stack.format_char("{}", std::to_underlying(job.id)));

        ImGui::TableSetColumnIndex(1);
        draw_job_name(job, nullptr);

        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(location_label(row_stack, job.location));

        ImGui::TableSetColumnIndex(3);
        const auto waited = std::chrono::duration_cast<Nanos>(self.now - job.submitted_at);
        const auto* waited_color = waited > self.stall_threshold ? &Gruvbox::bright_yellow.Value : nullptr;
        text_colored(format_duration(row_stack, waited).data(), waited_color);

        ImGui::TableSetColumnIndex(4);
        ImGui::TextUnformatted(row_stack.format_char("{}", job.barrier_count));

        ImGui::TableSetColumnIndex(5);
        if (ImGui::SmallButton("Run next")) {
          App::get_job_manager().prioritize(job.id);
        }

        auto is_action_hovered = ImGui::IsItemHovered();
        UI::tooltip_hover("Move this job to the front of the queue");

        ImGui::SameLine();
        const auto is_blocked = job.barrier_count > 0;
        ImGui::BeginDisabled(is_blocked);
        if (ImGui::SmallButton("Cancel")) {
          self.cancel_target = job.id;
          self.is_cancel_popup_requested = true;
        }
        ImGui::EndDisabled();

        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_ForTooltip)) {
          is_action_hovered = true;
          ImGui::SetTooltip(
            "%s",
            is_blocked ? "Something waits on this job, it can't be cancelled"
                       : "The job is dropped without running, and whoever submitted it isn't told"
          );
        }

        if (is_row_hovered && !is_action_hovered) {
          draw_job_tooltip(job, self.now);
        }

        ImGui::PopID();
      }
    }

    if (self.visible_rows.empty()) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(1);
      ImGui::TextDisabled("No queued jobs");
    }

    ImGui::EndTable();
  }

  if (self.is_cancel_popup_requested) {
    ImGui::OpenPopup("Cancel job###cancel_job");
    self.is_cancel_popup_requested = false;
  }

  self.draw_cancel_popup();
}

auto JobsPanel::draw_cancel_popup(this JobsPanel& self) -> void {
  memory::ScopedStack stack;

  if (!ImGui::BeginPopupModal("Cancel job###cancel_job", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    return;
  }

  const auto it = std::ranges::find(self.snapshot.live, self.cancel_target, &JobRecord::id);
  if (it == self.snapshot.live.end()) {
    ImGui::TextUnformatted("The job already left the queue.");
    if (ImGui::Button("Close")) {
      ImGui::CloseCurrentPopup();
    }
  } else {
    ImGui::TextUnformatted(
      stack.format_char("Cancel '{}'?", it->name.empty() ? std::string_view("<unnamed>") : std::string_view(it->name))
    );
    ImGui::TextDisabled("It is dropped without running.");

    if (ImGui::Button("Yes")) {
      App::get_job_manager().cancel(self.cancel_target);
      self.cancel_target = JobID::Invalid;
      ImGui::CloseCurrentPopup();
    }

    ImGui::SameLine();
    if (ImGui::Button("No")) {
      ImGui::CloseCurrentPopup();
    }
  }

  ImGui::EndPopup();
}

auto JobsPanel::draw_history(this JobsPanel& self) -> void {
  ZoneScoped;

  constexpr auto TABLE_FLAGS = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV |
                               ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
  if (!ImGui::BeginTable("###history_table", 6, TABLE_FLAGS)) {
    return;
  }

  ImGui::TableSetupColumn("Job", ImGuiTableColumnFlags_WidthStretch, 3.0f);
  ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, UI::scale(80.0f));
  ImGui::TableSetupColumn("Queued for", ImGuiTableColumnFlags_WidthStretch, 1.0f);
  ImGui::TableSetupColumn("Ran for", ImGuiTableColumnFlags_WidthStretch, 1.0f);
  ImGui::TableSetupColumn("Worker", ImGuiTableColumnFlags_WidthFixed, UI::scale(70.0f));
  ImGui::TableSetupColumn("Finished", ImGuiTableColumnFlags_WidthStretch, 1.0f);
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableHeadersRow();

  self.visible_rows.clear();
  for (const auto i : std::views::iota(0_u32, static_cast<u32>(self.snapshot.history.size())) | std::views::reverse) {
    const auto& job = self.snapshot.history[i];
    if (passes_filter(self.filter, job.name, job.location)) {
      self.visible_rows.push_back(i);
    }
  }

  const auto row_height = ImGui::GetTextLineHeight() + ImGui::GetStyle().CellPadding.y * 2.0f;
  auto clipper = ImGuiListClipper();
  clipper.Begin(static_cast<i32>(self.visible_rows.size()), row_height);
  while (clipper.Step()) {
    for (auto row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
      memory::ScopedStack stack;

      const auto& job = self.snapshot.history[self.visible_rows[static_cast<usize>(row)]];
      const auto is_cancelled = job.state == JobState::Cancelled;
      ImGui::TableNextRow(ImGuiTableRowFlags_None, row_height);
      ImGui::TableSetColumnIndex(0);
      ImGui::PushID(static_cast<i32>(std::to_underlying(job.id)));

      const auto is_hovered = begin_hover_row();
      draw_job_name(job, nullptr);

      ImGui::TableSetColumnIndex(1);
      text_colored(state_label(job.state), state_color(job.state));

      const auto queued_until = is_cancelled ? job.finished_at : job.started_at;
      ImGui::TableSetColumnIndex(2);
      ImGui::TextUnformatted(
        format_duration(stack, std::chrono::duration_cast<Nanos>(queued_until - job.submitted_at)).data()
      );

      ImGui::TableSetColumnIndex(3);
      if (is_cancelled) {
        ImGui::TextDisabled("-");
      } else {
        ImGui::TextUnformatted(
          format_duration(stack, std::chrono::duration_cast<Nanos>(job.finished_at - job.started_at)).data()
        );
      }

      ImGui::TableSetColumnIndex(4);
      draw_worker_cell(stack, job.worker);

      ImGui::TableSetColumnIndex(5);
      const auto since = std::chrono::duration_cast<Nanos>(self.now - job.finished_at);
      ImGui::TextUnformatted(stack.format_char("{} ago", format_duration(stack, since)));

      if (is_hovered) {
        draw_job_tooltip(job, self.now);
      }

      ImGui::PopID();
    }
  }

  ImGui::EndTable();
}

auto JobsPanel::draw_stats(this JobsPanel& self) -> void {
  ZoneScoped;

  constexpr auto TABLE_FLAGS = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV |
                               ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingStretchProp;
  if (!ImGui::BeginTable("###stats_table", 8, TABLE_FLAGS)) {
    return;
  }

  ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch, 3.0f);
  ImGui::TableSetupColumn(
    "Completed",
    ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_PreferSortDescending,
    1.0f
  );
  ImGui::TableSetupColumn(
    "Cancelled",
    ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_PreferSortDescending,
    1.0f
  );
  ImGui::TableSetupColumn("Avg", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_PreferSortDescending, 1.0f);
  ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_PreferSortDescending, 1.0f);
  ImGui::TableSetupColumn(
    "Total",
    ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending,
    1.0f
  );
  ImGui::TableSetupColumn(
    "Max queued",
    ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_PreferSortDescending,
    1.0f
  );
  ImGui::TableSetupColumn(
    "Last",
    ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_PreferSortDescending,
    1.0f
  );
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableHeadersRow();

  self.visible_rows.clear();
  for (u32 i = 0; i < static_cast<u32>(self.snapshot.stats.size()); i++) {
    const auto& stat = self.snapshot.stats[i];
    if (passes_filter(self.filter, stat.label, stat.location)) {
      self.visible_rows.push_back(i);
    }
  }

  const auto average_of = [](const JobStats& stat) -> Nanos {
    return stat.completed > 0 ? stat.total_run / static_cast<i64>(stat.completed) : Nanos{};
  };

  if (const auto* specs = ImGui::TableGetSortSpecs(); specs != nullptr && specs->SpecsCount > 0) {
    const auto& spec = specs->Specs[0];
    const auto is_ascending = spec.SortDirection == ImGuiSortDirection_Ascending;
    std::ranges::stable_sort(self.visible_rows, [&](const u32 lhs_index, const u32 rhs_index) {
      const auto& lhs = self.snapshot.stats[lhs_index];
      const auto& rhs = self.snapshot.stats[rhs_index];
      const auto order = [&]() -> std::partial_ordering {
        switch (spec.ColumnIndex) {
          case 0 : return lhs.label <=> rhs.label;
          case 1 : return lhs.completed <=> rhs.completed;
          case 2 : return lhs.cancelled <=> rhs.cancelled;
          case 3 : return average_of(lhs) <=> average_of(rhs);
          case 4 : return lhs.max_run <=> rhs.max_run;
          case 5 : return lhs.total_run <=> rhs.total_run;
          case 6 : return lhs.max_queued <=> rhs.max_queued;
          default: return lhs.last_finished_at <=> rhs.last_finished_at;
        }
      }();
      return is_ascending ? order < 0 : order > 0;
    });
  }

  const auto row_height = ImGui::GetTextLineHeight() + ImGui::GetStyle().CellPadding.y * 2.0f;
  auto clipper = ImGuiListClipper();
  clipper.Begin(static_cast<i32>(self.visible_rows.size()), row_height);
  while (clipper.Step()) {
    for (auto row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
      memory::ScopedStack stack;

      const auto& stat = self.snapshot.stats[self.visible_rows[static_cast<usize>(row)]];
      ImGui::TableNextRow(ImGuiTableRowFlags_None, row_height);

      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(stat.label.c_str());

      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(stack.format_char("{}", stat.completed));

      ImGui::TableSetColumnIndex(2);
      ImGui::TextUnformatted(stack.format_char("{}", stat.cancelled));

      ImGui::TableSetColumnIndex(3);
      if (stat.completed > 0) {
        ImGui::TextUnformatted(format_duration(stack, average_of(stat)).data());
      } else {
        ImGui::TextDisabled("-");
      }

      ImGui::TableSetColumnIndex(4);
      ImGui::TextUnformatted(format_duration(stack, stat.max_run).data());

      ImGui::TableSetColumnIndex(5);
      ImGui::TextUnformatted(format_duration(stack, stat.total_run).data());

      ImGui::TableSetColumnIndex(6);
      ImGui::TextUnformatted(format_duration(stack, stat.max_queued).data());

      ImGui::TableSetColumnIndex(7);
      const auto since = std::chrono::duration_cast<Nanos>(self.now - stat.last_finished_at);
      ImGui::TextUnformatted(stack.format_char("{} ago", format_duration(stack, since)));
    }
  }

  ImGui::EndTable();
}

auto JobsPanel::copy_report(this JobsPanel& self) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  auto length = usize{0};
  for (const auto& job : self.snapshot.live) {
    memory::ScopedStack line_stack;
    length += report_line(line_stack, job, self.now).size();
  }

  const auto buffer = stack.alloc<c8>(length + 1);
  auto* cursor = buffer.data();
  for (const auto& job : self.snapshot.live) {
    memory::ScopedStack line_stack;
    cursor = std::ranges::copy(report_line(line_stack, job, self.now), cursor).out;
  }

  *cursor = '\0';
  ImGui::SetClipboardText(buffer.data());
}
} // namespace ox
