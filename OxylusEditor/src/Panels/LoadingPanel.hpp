#pragma once

#include <filesystem>
#include <string>
#include <vuk/ImageAttachment.hpp>

#include "EditorPanelState.hpp"

namespace ox {
class LoadingPanel : public EditorPanelState {
public:
  LoadingPanel();

  auto on_update(this LoadingPanel& self) -> void;
  auto on_render(this LoadingPanel& self, vuk::ImageAttachment swapchain_attachment) -> void;

  // Takes over from the project selector. The heavy part of the load runs a frame later, from
  // `on_update`, so the modal is already on screen before the directory walk blocks the thread.
  auto begin(this LoadingPanel& self, std::filesystem::path project_file) -> void;

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
