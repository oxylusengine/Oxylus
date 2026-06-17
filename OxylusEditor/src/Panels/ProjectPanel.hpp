#pragma once

#include <filesystem>

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
  std::shared_ptr<Texture> engine_banner = nullptr;

  std::filesystem::path new_project_dir = {};
  std::string new_project_name = "NewProject";
  std::filesystem::path new_project_asset_dir = "Assets";

  static void new_project(
    const std::filesystem::path& project_dir,
    const std::string& project_name,
    const std::filesystem::path& project_asset_dir
  );
};
} // namespace ox
