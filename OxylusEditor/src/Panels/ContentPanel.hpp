#pragma once

#include <ankerl/unordered_dense.h>
#include <filesystem>
#include <imgui.h>
#include <mutex>
#include <stack>
#include <vector>
#include <vuk/Types.hpp>
#include <vuk/Value.hpp>

#include "Core/Base.hpp"
#include "EditorPanel.hpp"
#include "ThumbnailRenderPipeline.hpp"

namespace ox {
class Texture;

enum class FileType { Unknown = 0, Directory, Meta, Scene, Prefab, Shader, Texture, Mesh, Audio, Script, Material };

class ContentPanel : public EditorPanel {
public:
  ContentPanel();

  ~ContentPanel() override = default;

  ContentPanel(const ContentPanel& other) = delete;
  ContentPanel(ContentPanel&& other) = delete;
  ContentPanel& operator=(const ContentPanel& other) = delete;
  ContentPanel& operator=(ContentPanel&& other) = delete;

  void init();
  void on_update() override;
  void on_render(vuk::Extent3D extent, vuk::Format format) override;

  void invalidate();

private:
  std::pair<bool, uint32_t> directory_tree_view_recursive(const std::filesystem::path& path,
                                                          uint32_t* count,
                                                          int* selectionMask,
                                                          ImGuiTreeNodeFlags flags);
  void render_header();
  void render_side_view();
  void render_body(bool grid);
  void update_directory_entries(const std::filesystem::path& directory);
  void refresh() { update_directory_entries(_current_directory); }

  void draw_context_menu_items(const std::filesystem::path& context, bool isDir);

  struct File {
    std::string name;
    std::string file_path;
    std::string extension;
    std::filesystem::directory_entry directory_entry;
    std::shared_ptr<Texture> thumbnail = nullptr;
    bool is_directory = false;

    FileType type;
    std::string_view file_type_string;
    ImVec4 file_type_indicator_color;
  };

  std::filesystem::path _assets_directory;
  std::filesystem::path _current_directory;
  std::stack<std::filesystem::path> _back_stack;
  std::vector<File> _directory_entries;
  std::mutex _directory_mutex;
  u32 _currently_visible_items_tree_view = 0;
  f32 thumbnail_max_limit = 256.0f;
  f32 thumbnail_size_grid_limit = 96.0f; // lower values than this will switch to grid view
  ImGuiTextFilter m_filter;
  f32 _elapsed_time = 0.0f;

  bool mesh_thumbnails_enabled = false;
  ankerl::unordered_dense::map<std::string, std::shared_ptr<Texture>> thumbnail_cache_textures;
  ankerl::unordered_dense::map<std::string, vuk::ImageAttachment> thumbnail_cache_meshes;
  ankerl::unordered_dense::map<std::string, std::unique_ptr<ThumbnailRenderPipeline>> thumbnail_render_pipeline_cache;

  std::shared_ptr<Texture> _white_texture;
  std::filesystem::path _directory_to_delete;
};
} // namespace ox
