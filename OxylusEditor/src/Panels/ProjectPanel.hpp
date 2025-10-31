#pragma once

#include <filesystem>

#include "EditorPanel.hpp"

namespace ox {
class ProjectPanel : public EditorPanel {
public:
  ProjectPanel();
  ~ProjectPanel() override = default;

  void on_update() override;
  void on_render(vuk::ImageAttachment swapchain_attachment) override;

  void load_project_for_editor(const std::filesystem::path& filepath);

private:
  static void new_project(
    const std::filesystem::path& project_dir,
    const std::string& project_name,
    const std::filesystem::path& project_asset_dir
  );

  std::filesystem::path new_project_dir = {};
  std::string new_project_name = "NewProject";
  std::filesystem::path new_project_asset_dir = "Assets";
};
} // namespace ox
