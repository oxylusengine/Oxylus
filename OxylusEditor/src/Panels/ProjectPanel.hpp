#pragma once

#include "EditorPanel.hpp"

namespace ox {
class ProjectPanel : public EditorPanel {
public:
  ProjectPanel();
  ~ProjectPanel() override = default;

  void on_update() override;
  void on_render(vuk::Extent3D extent, vuk::Format format) override;

  void load_project_for_editor(const std::string& filepath);

private:
  static void
  new_project(const std::string& project_dir, const std::string& project_name, const std::string& project_asset_dir);

  std::string new_project_dir = {};
  std::string new_project_name = "NewProject";
  std::string new_project_asset_dir = "Assets";
};
} // namespace ox
