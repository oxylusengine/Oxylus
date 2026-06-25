#pragma once

#include <FileWatch.hpp>
#include <ankerl/unordered_dense.h>
#include <filesystem>
#include <imgui.h>
#include <stack>
#include <vector>
#include <vuk/Types.hpp>
#include <vuk/Value.hpp>

#include "Core/Types.hpp"
#include "EditorPanelState.hpp"

namespace ox {
class Texture;

enum class FileType { Unknown = 0, Directory, Meta, Scene, Prefab, Shader, Texture, Model, Audio, Script, Material };

class ContentPanel : public EditorPanelState {
public:
  ContentPanel();

  auto init(this ContentPanel& self) -> void;
  auto on_update(this ContentPanel& self) -> void;
  auto on_render(this ContentPanel& self, vuk::ImageAttachment swapchain_attachment) -> void;

private:
  std::filesystem::path draw_context_menu_items(this ContentPanel& self, const std::filesystem::path& context, bool isDir);

  struct File {
    std::string name;
    std::filesystem::path file_path;
    std::filesystem::directory_entry directory_entry;
    std::shared_ptr<Texture> thumbnail = nullptr;
    std::string icon;
    bool is_directory = false;

    FileType type;
    std::string_view file_type_string;
    ImVec4 file_type_indicator_color;
  };

  std::filesystem::path assets_directory_;
  std::filesystem::path current_directory_;
  std::stack<std::filesystem::path> back_stack_;
  std::vector<File> directory_entries_;
  std::shared_mutex directory_mutex_;
  u32 currently_visible_items_tree_view_ = 0;
  f32 thumbnail_max_limit = 256.0f;
  f32 thumbnail_size_grid_limit = 96.0f; // lower values than this will switch to grid view
  ImGuiTextFilter filter_;
  f32 elapsed_time_ = 0.0f;

  std::string new_asset_name_ = {};
  bool should_open_new_asset_popup = false;

  std::shared_ptr<Texture> _white_texture;
  std::filesystem::path _directory_to_delete;

  std::unique_ptr<filewatch::FileWatch<std::string>> filewatch = nullptr;

  std::pair<bool, uint32_t> directory_tree_view_recursive(
    const std::filesystem::path& path, uint32_t* count, int* selectionMask, ImGuiTreeNodeFlags flags
  );
  auto render_header(this ContentPanel& self) -> void;
  auto render_side_view(this ContentPanel& self) -> void;
  auto render_body(this ContentPanel& self, bool grid) -> void;
  auto update_directory_entries(this ContentPanel& self, const std::filesystem::path& directory) -> void;
  auto refresh(this ContentPanel& self) -> void;
};
} // namespace ox
