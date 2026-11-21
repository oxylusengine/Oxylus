#pragma once

#include <array>
#include <chrono>
#include <imgui.h>
#include <vector>

#include "Core/JobManager.hpp"
#include "Panels/EditorPanelState.hpp"
#include "Render/Fwd.hpp"

namespace ox {
class JobsPanel : public EditorPanelState {
public:
  f32 stall_seconds = 5.0f;

  JobsPanel();

  auto on_update(this JobsPanel& self) -> void {}
  auto on_render(this JobsPanel& self, const SwapchainInfo& swapchain_info) -> void;

private:
  static constexpr usize SAMPLE_COUNT = 300;
  static constexpr f64 SAMPLE_INTERVAL = 0.1;

  auto record_sample(this JobsPanel& self, u32 queued, u32 active) -> void;
  auto draw_toolbar(this JobsPanel& self, u32 worker_count, u32 running, u32 waiting, u32 queued) -> void;
  auto draw_plot(this JobsPanel& self) -> void;
  auto draw_workers(this JobsPanel& self, u32 worker_count) -> void;
  auto draw_queue(this JobsPanel& self) -> void;
  auto draw_history(this JobsPanel& self) -> void;
  auto draw_stats(this JobsPanel& self) -> void;
  auto draw_cancel_popup(this JobsPanel& self) -> void;
  auto copy_report(this JobsPanel& self) -> void;

  JobSnapshot snapshot = {};
  ImGuiTextFilter filter = {};
  std::vector<u32> visible_rows = {};
  std::chrono::steady_clock::time_point now = {};
  std::chrono::steady_clock::duration stall_threshold = {};

  std::array<f32, SAMPLE_COUNT> queued_samples = {};
  std::array<f32, SAMPLE_COUNT> active_samples = {};
  usize sample_head = 0;
  usize sample_size = 0;
  f64 last_sample_time = -1.0;

  JobID cancel_target = JobID::Invalid;
  bool is_cancel_popup_requested = false;
};
} // namespace ox
