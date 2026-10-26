#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "EditorPanelState.hpp"
#include "Render/Fwd.hpp"

namespace ox {
class ProjectPanel : public EditorPanelState {
public:
  ProjectPanel();

  auto on_update(this ProjectPanel& self) -> void {}
  auto on_render(this ProjectPanel& self, const SwapchainInfo& swapchain_info) -> void;

  auto load_project_for_editor(this ProjectPanel& self, const std::filesystem::path& filepath) -> void;

  // bring the launcher back when a load fails after the loading panel takes over
  auto show_error(this ProjectPanel& self, std::string message) -> void;

private:
  bool creating_project = false;
  bool select_projects_tab = false;
  bool close_requested = false;

  ImGuiTextFilter project_filter = {};

  std::string new_project_location = {};
  std::string new_project_name = "NewProject";
  std::string new_project_asset_dir = "Assets";
  std::string panel_error = {};

  auto draw_sidebar(this ProjectPanel& self) -> void;
  auto draw_recent_projects(this ProjectPanel& self) -> void;
  auto draw_new_project(this ProjectPanel& self) -> void;
  auto draw_samples(this ProjectPanel& self) -> void;
  auto browse_project(this ProjectPanel& self) -> void;
  auto continue_to_editor(this ProjectPanel& self) -> void;

  auto new_project(
    this ProjectPanel& self,
    const std::filesystem::path& project_dir,
    std::string_view project_name,
    const std::filesystem::path& project_asset_dir
  ) -> bool;
};
} // namespace ox
