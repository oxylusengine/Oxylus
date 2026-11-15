#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "EditorPanelState.hpp"
#include "Render/Fwd.hpp"

namespace ox {
class LoadingPanel : public EditorPanelState {
public:
  LoadingPanel();

  auto on_update(this LoadingPanel& self) -> void;
  auto on_render(this LoadingPanel& self, const SwapchainInfo& swapchain_info) -> void;

  // Takes over from the project selector. The heavy part of the load runs a frame later, from
  // `on_update`, so the modal is already on screen before the directory walk blocks the thread.
  auto begin(this LoadingPanel& self, std::filesystem::path project_file) -> void;

  auto is_failed(this const LoadingPanel& self) -> bool { return self.phase == Phase::Failed; }
  auto failure_text(this const LoadingPanel& self) -> std::string_view { return self.failure; }
  // past the asset scan, only the start scene and thumbnails are left
  auto is_past_scan(this const LoadingPanel& self) -> bool {
    return self.phase == Phase::OpeningScene || self.phase == Phase::Thumbnails;
  }
  auto is_loading(this const LoadingPanel& self) -> bool {
    return self.visible && !self.is_failed() && self.phase != Phase::Thumbnails;
  }
  // what the Back button does
  auto dismiss_failure(this LoadingPanel& self) -> void;

private:
  enum class Phase : u8 { Discovering = 0, Cooking, OpeningScene, Thumbnails, Failed };

  struct PhaseProgress {
    usize completed = 0;
    usize total = 0;
    std::string current = {};
  };

  Phase phase = Phase::Discovering;
  std::filesystem::path project_file = {};
  std::string project_name = {};
  std::string failure = {};
  bool close_requested = false;

  PhaseProgress baking = {};
  PhaseProgress registering = {};
  PhaseProgress thumbnails = {};

  auto load_project(this LoadingPanel& self) -> void;
  auto start_thumbnail_prewarm(this LoadingPanel& self) -> void;
};
} // namespace ox
