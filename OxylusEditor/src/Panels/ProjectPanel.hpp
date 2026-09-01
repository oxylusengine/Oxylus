#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "Asset/Texture.hpp"
#include "EditorPanelState.hpp"

namespace ox {
class ProjectPanel : public EditorPanelState {
public:
  ProjectPanel();

  auto on_update(this ProjectPanel& self) -> void {}
  auto on_render(this ProjectPanel& self, vuk::ImageAttachment swapchain_attachment) -> void;

  auto load_project_for_editor(this ProjectPanel& self, const std::filesystem::path& filepath) -> void;

private:
  Texture engine_banner = {};

  bool creating_project = false;
  bool close_requested = false;

  std::string new_project_location = {};
  std::string new_project_name = "NewProject";
  std::string new_project_asset_dir = "Assets";
  std::string panel_error = {};

  auto new_project(
    this ProjectPanel& self,
    const std::filesystem::path& project_dir,
    std::string_view project_name,
    const std::filesystem::path& project_asset_dir
  ) -> bool;
};
} // namespace ox
