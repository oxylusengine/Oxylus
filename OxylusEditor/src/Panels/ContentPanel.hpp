#pragma once

#include <FileWatch.hpp>
#include <ankerl/unordered_dense.h>
#include <filesystem>
#include <imgui.h>
#include <span>
#include <stack>
#include <vector>
#include <vuk/Types.hpp>
#include <vuk/Value.hpp>

#include "Asset/Texture.hpp"
#include "Core/Types.hpp"
#include "EditorPanelState.hpp"

namespace ox {
class Texture;

enum class FileType {
  Unknown = 0,
  Directory,
  Meta,
  Scene,
  Prefab,
  Shader,
  Texture,
  Model,
  Audio,
  Script,
  Material,
  Terrain,
  ParticleSystem
};

class ContentPanel : public EditorPanelState {
public:
  ContentPanel();

  auto init(this ContentPanel& self) -> void;
  auto on_update(this ContentPanel& self) -> void;
  auto on_render(this ContentPanel& self, vuk::ImageAttachment swapchain_attachment) -> void;

private:
  enum class SortField : u8 {
    Name = 0,
    DateModified,
    Type,
    Size,
  };

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
    u64 size_bytes = 0;
    std::filesystem::file_time_type modified_time = {};
    bool has_size = false;
    bool has_modified_time = false;
    std::string size_string = {};
    std::string modified_time_string = {};
  };

  std::filesystem::path assets_directory;
  std::filesystem::path current_directory;
  std::stack<std::filesystem::path> back_stack;
  std::vector<File> directory_entries;
  std::shared_mutex directory_mutex;
  f32 thumbnail_max_limit = 256.0f;
  f32 thumbnail_size_grid_limit = 96.0f; // lower values than this will switch to grid view
  ImGuiTextFilter filter_;
  f32 elapsed_time_ = 0.0f;
  SortField sort_field_ = SortField::Name;
  bool sort_ascending_ = true;

  std::string new_asset_name = {};
  bool should_open_new_asset_popup = false;
  bool refresh_requested = false;
  AssetType new_asset_type = AssetType::Material;

  Texture white_texture = {};
  std::filesystem::path directory_to_delete;

  std::unique_ptr<filewatch::FileWatch<std::string>> filewatch = nullptr;

  auto draw_context_menu_items(this ContentPanel& self, const std::filesystem::path& context, bool isDir)
    -> std::filesystem::path;
  auto directory_tree_view_recursive(
    this ContentPanel& self,
    const std::filesystem::path& path,
    const std::filesystem::path& selected_directory,
    ImGuiTreeNodeFlags flags
  ) -> void;
  auto render_header(this ContentPanel& self) -> void;
  auto render_sort_menu(this ContentPanel& self) -> void;
  auto render_details_headers(this ContentPanel& self) -> void;
  auto render_side_view(this ContentPanel& self) -> void;
  auto render_body(this ContentPanel& self, bool grid) -> void;
  auto update_directory_entries(this ContentPanel& self, const std::filesystem::path& directory) -> void;
  auto refresh(this ContentPanel& self) -> void;
  auto set_sort(this ContentPanel& self, SortField field, bool ascending) -> void;

  static auto sort_entries(std::span<File> entries, SortField field, bool ascending) -> void;
  static auto sort_field_label(SortField field) -> std::string_view;
};
} // namespace ox
