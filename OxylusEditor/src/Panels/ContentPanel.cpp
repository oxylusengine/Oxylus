#include "ContentPanel.hpp"

#include <filesystem>
#include <icons/IconsMaterialDesignIcons.h>
#include <imgui.h>
#include <vuk/runtime/vk/AllocatorHelpers.hpp>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Core/FileSystem.hpp"
#include "Core/VFS.hpp"
#include "EditorContext.hpp"
#include "EditorLayer.hpp"
#include "EditorUI.hpp"
#include "Thread/ThreadManager.hpp"
#include "Utils/FileWatch.hpp"
#include "Utils/PayloadData.hpp"
#include "Utils/Profiler.hpp"
#include "Utils/StringUtils.hpp"

namespace ox {
static const ankerl::unordered_dense::map<FileType, const char*> FILE_TYPES_TO_STRING = {
    {FileType::Unknown, "Unknown"},
    {FileType::Directory, "Directory"},

    {FileType::Meta, "Meta"},
    {FileType::Scene, "Scene"},
    {FileType::Prefab, "Prefab"},
    {FileType::Shader, "Shader"},
    {FileType::Texture, "Texture"},
    {FileType::Mesh, "Mesh"},
    {FileType::Script, "Script"},
    {FileType::Audio, "Audio"},
};

static const ankerl::unordered_dense::map<std::string, FileType> FILE_TYPES = {
    {"", FileType::Directory},                                                                   //
    {".oxasset", FileType::Meta},                                                                //
    {".oxscene", FileType::Scene},                                                               //
    {".oxprefab", FileType::Prefab},                                                             //
    {".hlsl", FileType::Shader},     {".hlsli", FileType::Shader}, {".glsl", FileType::Shader},  //
    {".frag", FileType::Shader},     {".vert", FileType::Shader},  {".slang", FileType::Shader}, //

    {".png", FileType::Texture},     {".jpg", FileType::Texture},  {".jpeg", FileType::Texture}, //
    {".bmp", FileType::Texture},     {".gif", FileType::Texture},  {".ktx", FileType::Texture},  //
    {".ktx2", FileType::Texture},    {".tiff", FileType::Texture},                               //

    {".gltf", FileType::Mesh},       {".glb", FileType::Mesh},                                   //

    {".mp3", FileType::Audio},       {".m4a", FileType::Audio},    {".wav", FileType::Audio},    //
    {".ogg", FileType::Audio},                                                                   //

    {".lua", FileType::Script},                                                                  //
};

static const ankerl::unordered_dense::map<FileType, ImVec4> TYPE_COLORS = {
    {FileType::Meta, {0.75f, 0.35f, 0.20f, 1.00f}},
    {FileType::Scene, {0.75f, 0.35f, 0.20f, 1.00f}},
    {FileType::Prefab, {0.10f, 0.50f, 0.80f, 1.00f}},
    {FileType::Shader, {0.10f, 0.50f, 0.80f, 1.00f}},
    {FileType::Texture, {0.80f, 0.20f, 0.30f, 1.00f}},
    {FileType::Mesh, {0.20f, 0.80f, 0.75f, 1.00f}},
    {FileType::Audio, {0.20f, 0.80f, 0.50f, 1.00f}},
    {FileType::Script, {0.0f, 16.0f, 121.0f, 1.00f}},
};

static const ankerl::unordered_dense::map<FileType, const char8_t*> FILE_TYPES_TO_ICON = {
    {FileType::Unknown, ICON_MDI_FILE},
    {FileType::Directory, ICON_MDI_FOLDER},
    {FileType::Meta, ICON_MDI_FILE_DOCUMENT},
    {FileType::Scene, ICON_MDI_IMAGE_FILTER_HDR},
    {FileType::Prefab, ICON_MDI_FILE},
    {FileType::Shader, ICON_MDI_IMAGE_FILTER_BLACK_WHITE},
    {FileType::Texture, ICON_MDI_FILE_IMAGE},
    {FileType::Mesh, ICON_MDI_VECTOR_POLYGON},
    {FileType::Audio, ICON_MDI_MICROPHONE},
    {FileType::Script, ICON_MDI_LANGUAGE_LUA},
    {FileType::Material, ICON_MDI_PALETTE_SWATCH},
};

static bool drag_drop_target(const std::filesystem::path& drop_path) {
  if (ImGui::BeginDragDropTarget()) {
    const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(PayloadData::DRAG_DROP_TARGET);
    if (payload) {
      auto* asset = static_cast<PayloadData*>(payload->Data);

      auto* asset_man = App::get_asset_manager();

      ::fs::path file_path = {};
      u32 counter = 0;
      do {
        file_path = drop_path /
                    fmt::format("{}{}", asset->get_str(), (counter > 0 ? "_" + std::to_string(counter) : ""));
        counter++;
      } while (::fs::exists(file_path / ".oxasset"));

      if (!asset_man->export_asset(asset->uuid, file_path.string()))
        OX_LOG_ERROR("Couldn't export asset!");
      return true;
    }

    ImGui::EndDragDropTarget();
  }

  return false;
}

static void drag_drop_from(const std::filesystem::path& filepath) {
  if (ImGui::BeginDragDropSource()) {
    const std::string path_str = filepath.string();
    const auto payload_data = PayloadData(path_str, UUID(nullptr));
    ImGui::SetDragDropPayload(PayloadData::DRAG_DROP_SOURCE, &payload_data, payload_data.size());
    ImGui::TextUnformatted(filepath.filename().string().c_str());
    ImGui::EndDragDropSource();
  }
}

static void open_file(const std::filesystem::path& path) {
  const std::string filepath_string = path.string();
  const char* filepath = filepath_string.c_str();
  const std::string ext = path.extension().string();
  const auto& file_type_it = FILE_TYPES.find(ext);
  if (file_type_it != FILE_TYPES.end()) {
    const FileType file_type = file_type_it->second;
    switch (file_type) {
      case FileType::Scene: {
        EditorLayer::get()->open_scene(filepath);
        break;
      }
      case FileType::Unknown: break;
      case FileType::Prefab : break;
      case FileType::Texture: break;
      case FileType::Shader : [[fallthrough]];
      case FileType::Script : {
        fs::open_file_externally(filepath);
        break;
      }
      case ox::FileType::Material: break;
      default                    : break;
    }
  } else {
    fs::open_file_externally(filepath);
  }
}

std::pair<bool, uint32_t> ContentPanel::directory_tree_view_recursive(const std::filesystem::path& path,
                                                                      uint32_t* count,
                                                                      int* selectionMask,
                                                                      ImGuiTreeNodeFlags flags) {
  ZoneScoped;

  auto& editor_theme = EditorLayer::get()->editor_theme;

  bool any_node_clicked = false;
  uint32_t node_clicked = 0;

  if (path.empty())
    return {};

  for (const auto& entry : std::filesystem::directory_iterator(path)) {
    ImGuiTreeNodeFlags nodeFlags = flags;

    auto& entryPath = entry.path();

    const bool entryIsFile = !std::filesystem::is_directory(entryPath);
    if (entryIsFile)
      nodeFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

    ImGui::TableNextRow();
    ImGui::TableNextColumn();

    const bool selected = (*selectionMask & BIT(*count)) != 0;
    if (selected) {
      nodeFlags |= ImGuiTreeNodeFlags_Selected;
      ImGui::PushStyleColor(ImGuiCol_Header, editor_theme.header_selected_color);
      ImGui::PushStyleColor(ImGuiCol_HeaderHovered, editor_theme.header_selected_color);
    } else {
      ImGui::PushStyleColor(ImGuiCol_HeaderHovered, editor_theme.header_hovered_color);
    }

    const uint64_t id = *count;
    const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(id), nodeFlags, "");
    ImGui::PopStyleColor(selected ? 2 : 1);

    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
      if (!entryIsFile)
        update_directory_entries(entryPath);

      node_clicked = *count;
      any_node_clicked = true;
    }

    const std::string filepath = entryPath.string();

    if (!entryIsFile)
      drag_drop_target(entryPath);
    drag_drop_from(entryPath);

    auto name = fs::get_name_with_extension(filepath);

    const char8_t* folderIcon = ICON_MDI_FILE;
    if (entryIsFile) {
      auto fileType = FileType::Unknown;
      const auto& fileTypeIt = FILE_TYPES.find(entryPath.extension().string());
      if (fileTypeIt != FILE_TYPES.end())
        fileType = fileTypeIt->second;

      const auto& fileTypeIconIt = FILE_TYPES_TO_ICON.find(fileType);
      if (fileTypeIconIt != FILE_TYPES_TO_ICON.end())
        folderIcon = fileTypeIconIt->second;
    } else {
      folderIcon = open ? ICON_MDI_FOLDER_OPEN : ICON_MDI_FOLDER;
    }

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, editor_theme.asset_icon_color);
    ImGui::TextUnformatted(StringUtils::from_char8_t(folderIcon));
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextUnformatted(name.data());
    _currently_visible_items_tree_view++;

    (*count)--;

    if (!entryIsFile) {
      if (open) {
        const auto [isClicked, clickedNode] = directory_tree_view_recursive(entryPath, count, selectionMask, flags);

        if (!any_node_clicked) {
          any_node_clicked = isClicked;
          node_clicked = clickedNode;
        }

        ImGui::TreePop();
      }
    }
  }

  return {any_node_clicked, node_clicked};
}

ContentPanel::ContentPanel() : EditorPanel("Contents", ICON_MDI_FOLDER_STAR, true) {
  const auto& window = App::get()->get_window();
  const f32 scale = window.get_content_scale();
  thumbnail_max_limit *= scale;
  thumbnail_size_grid_limit *= scale;

  _white_texture = std::make_shared<Texture>();
  char white_texture_data[16 * 16 * 4];
  memset(white_texture_data, 0xff, 16 * 16 * 4);
  _white_texture->create({},
                         {.preset = Preset::eRTT2DUnmipped,
                          .format = vuk::Format::eR8G8B8A8Unorm,
                          .mime = {},
                          .loaded_data = white_texture_data,
                          .extent = vuk::Extent3D{.width = 16u, .height = 16u, .depth = 1u}});

  ThumbnailRenderPipeline rp;
  rp.init(App::get_vkcontext());
}

void ContentPanel::init() {
  if (!App::get_vfs()->is_mounted_dir(VFS::PROJECT_DIR))
    return;

  auto assets_dir = App::get_vfs()->resolve_physical_dir(VFS::PROJECT_DIR, "");
  _assets_directory = assets_dir;
  _current_directory = _assets_directory;
  refresh();

  [[maybe_unused]]
  static filewatch::FileWatch<std::string> watch(_assets_directory.string(),
                                                 [this](const auto&, const filewatch::Event) {
                                                   ThreadManager::get()->asset_thread.queue_job([this] { refresh(); });
                                                 });
}

void ContentPanel::on_update() { _elapsed_time += static_cast<float>(App::get_timestep()); }

void ContentPanel::on_render(vuk::Extent3D extent, vuk::Format format) {
  constexpr ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar;

  constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_Resizable | ImGuiTableFlags_ContextMenuInBody;

  if (_assets_directory.empty()) {
    init();
  }

  on_begin(windowFlags);
  {
    render_header();
    ImGui::Separator();
    const ImVec2 availableRegion = ImGui::GetContentRegionAvail();
    if (ImGui::BeginTable("MainViewTable", 2, tableFlags, availableRegion)) {
      ImGui::TableSetupColumn("##side_view", ImGuiTableColumnFlags_WidthFixed, 150);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      render_side_view();
      ImGui::TableNextColumn();
      render_body(EditorCVar::cvar_file_thumbnail_size.get() >= thumbnail_size_grid_limit);

      ImGui::EndTable();
    }
  }
  on_end();
}

void ContentPanel::invalidate() {
  if (!App::get_vfs()->is_mounted_dir(VFS::PROJECT_DIR))
    return;

  auto assets_dir = App::get_vfs()->resolve_physical_dir(VFS::PROJECT_DIR, "");
  _assets_directory = assets_dir;
  _current_directory = _assets_directory;
  refresh();
}

void ContentPanel::render_header() {
  if (ImGui::Button(StringUtils::from_char8_t(ICON_MDI_COGS)))
    ImGui::OpenPopup("SettingsPopup");
  if (ImGui::BeginPopup("SettingsPopup")) {
    UI::begin_properties(ImGuiTableFlags_SizingStretchSame);
    UI::property("Thumbnail Size",
                 EditorCVar::cvar_file_thumbnail_size.get_ptr(),
                 thumbnail_size_grid_limit - 0.1f,
                 thumbnail_max_limit,
                 nullptr,
                 0.1f,
                 "");
    UI::property("Show file thumbnails", reinterpret_cast<bool*>(EditorCVar::cvar_file_thumbnails.get_ptr()));
    UI::end_properties();
    ImGui::EndPopup();
  }

  ImGui::SameLine();
  const float cursorPosX = ImGui::GetCursorPosX();
  m_filter.Draw("###ConsoleFilter", ImGui::GetContentRegionAvail().x);
  if (!m_filter.IsActive()) {
    ImGui::SameLine();
    ImGui::SetCursorPosX(cursorPosX + ImGui::GetFontSize() * 0.5f);
    ImGui::TextUnformatted(StringUtils::from_char8_t(ICON_MDI_MAGNIFY " Search..."));
  }

  ImGui::Spacing();
  ImGui::Spacing();

  // Back button
  {
    bool disabledBackButton = false;
    if (_current_directory == _assets_directory)
      disabledBackButton = true;

    if (disabledBackButton) {
      ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
      ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
    }

    if (ImGui::Button(StringUtils::from_char8_t(ICON_MDI_ARROW_LEFT_CIRCLE_OUTLINE))) {
      _back_stack.push(_current_directory);
      update_directory_entries(_current_directory.parent_path());
    }

    if (disabledBackButton) {
      ImGui::PopStyleVar();
      ImGui::PopItemFlag();
    }
  }

  ImGui::SameLine();

  // Front button
  {
    bool disabledFrontButton = false;
    if (_back_stack.empty())
      disabledFrontButton = true;

    if (disabledFrontButton) {
      ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
      ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
    }

    if (ImGui::Button(StringUtils::from_char8_t(ICON_MDI_ARROW_RIGHT_CIRCLE_OUTLINE))) {
      const auto& top = _back_stack.top();
      update_directory_entries(top);
      _back_stack.pop();
    }

    if (disabledFrontButton) {
      ImGui::PopStyleVar();
      ImGui::PopItemFlag();
    }
  }

  ImGui::SameLine();

  ImGui::TextUnformatted(StringUtils::from_char8_t(ICON_MDI_FOLDER));

  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_Button, {0.0f, 0.0f, 0.0f, 0.0f});
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.0f, 0.0f, 0.0f, 0.0f});
  std::filesystem::path current = _assets_directory.parent_path();
  std::filesystem::path directoryToOpen;
  const std::filesystem::path currentDirectory = relative(_current_directory, current);
  for (const auto& path : currentDirectory) {
    current /= path;
    ImGui::SameLine();
    if (ImGui::Button(path.filename().string().c_str()))
      directoryToOpen = current;

    if (_current_directory != current) {
      ImGui::SameLine();
      ImGui::TextUnformatted("/");
    }
  }
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar();

  if (!directoryToOpen.empty())
    update_directory_entries(directoryToOpen);
}

void ContentPanel::render_side_view() {
  ZoneScoped;
  static int selection_mask = 0;

  constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_NoPadInnerX | ImGuiTableFlags_NoPadOuterX |
                                         ImGuiTableFlags_ContextMenuInBody | ImGuiTableFlags_ScrollY;

  constexpr ImGuiTreeNodeFlags tree_node_flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_FramePadding |
                                                 ImGuiTreeNodeFlags_SpanFullWidth;

  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {0, 0});
  if (ImGui::BeginTable("SideViewTable", 1, tableFlags)) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();

    const auto& editor_theme = EditorLayer::get()->editor_theme;

    ImGuiTreeNodeFlags node_flags = tree_node_flags;
    const bool selected = _current_directory == _assets_directory && selection_mask == 0;
    if (selected) {
      node_flags |= ImGuiTreeNodeFlags_Selected;
      ImGui::PushStyleColor(ImGuiCol_Header, editor_theme.header_selected_color);
      ImGui::PushStyleColor(ImGuiCol_HeaderHovered, editor_theme.header_selected_color);
    } else {
      ImGui::PushStyleColor(ImGuiCol_HeaderHovered, editor_theme.header_hovered_color);
    }

    const bool opened = ImGui::TreeNodeEx(_assets_directory.string().c_str(), node_flags, "");
    ImGui::PopStyleColor(selected ? 2 : 1);

    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
      update_directory_entries(_assets_directory);
      selection_mask = 0;
    }
    const char8_t* folderIcon = opened ? ICON_MDI_FOLDER_OPEN : ICON_MDI_FOLDER;
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, editor_theme.asset_icon_color);
    ImGui::TextUnformatted(StringUtils::from_char8_t(folderIcon));
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextUnformatted("Assets");

    if (opened) {
      uint32_t count = 0;
      const auto [is_clicked, clicked_node] = directory_tree_view_recursive(
          _assets_directory, &count, &selection_mask, tree_node_flags);

      if (is_clicked) {
        // (process outside of tree loop to avoid visual inconsistencies during the clicking frame)
        if (ImGui::GetIO().KeyCtrl)
          selection_mask ^= BIT(clicked_node); // CTRL+click to toggle
        else
          selection_mask = BIT(clicked_node);  // Click to single-select
      }

      ImGui::TreePop();
    }
    ImGui::EndTable();
    if (ImGui::IsItemClicked())
      EditorLayer::get()->get_context().reset();
  }

  ImGui::PopStyleVar();
}

void ContentPanel::render_body(bool grid) {
  const auto& editor_theme = EditorLayer::get()->editor_theme;
  auto& editor_context = EditorLayer::get()->get_context();
  auto& vk_context = App::get_vkcontext();

  std::filesystem::path directory_to_open;

  constexpr float padding = 2.0f;
  const float scaled_thumbnail_size = EditorCVar::cvar_file_thumbnail_size.get() * ImGui::GetIO().FontGlobalScale;
  const float scaled_thumbnail_size_x = scaled_thumbnail_size * 0.55f;
  const float cell_size = scaled_thumbnail_size_x + 2 * padding + scaled_thumbnail_size_x * 0.1f;

  constexpr float overlay_padding_y = 6.0f * padding;
  constexpr float thumbnail_padding = overlay_padding_y * 0.5f;
  const float thumb_image_size = scaled_thumbnail_size_x - thumbnail_padding;

  const ImVec2 background_thumbnail_size = {scaled_thumbnail_size_x + padding * 2, scaled_thumbnail_size};

  const float panel_width = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize;
  int column_count = static_cast<int>(panel_width / cell_size);
  if (column_count < 1)
    column_count = 1;

  float line_height = ImGui::GetTextLineHeight();
  int flags = ImGuiTableFlags_ContextMenuInBody | ImGuiTableFlags_ScrollY;

  if (!grid) {
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {0, 0});
    column_count = 1;
    flags |= ImGuiTableFlags_NoPadOuterX | ImGuiTableFlags_NoPadInnerX | ImGuiTableFlags_SizingStretchSame;
  } else {
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {scaled_thumbnail_size_x * 0.05f, scaled_thumbnail_size_x * 0.05f});
    flags |= ImGuiTableFlags_PadOuterX | ImGuiTableFlags_SizingFixedFit;
  }

  ImVec2 cursor_pos = ImGui::GetCursorPos();
  const ImVec2 region = ImGui::GetContentRegionAvail();
  ImGui::InvisibleButton("##DragDropTargetAssetPanelBody", region);

  ImGui::SetItemAllowOverlap();
  ImGui::SetCursorPos(cursor_pos);

  if (ImGui::BeginTable("BodyTable", column_count, flags)) {
    bool any_item_hovered = false;

    int i = 0;

    for (auto& file : _directory_entries) {
      if (!m_filter.PassFilter(file.name.c_str()))
        continue;

      ImGui::PushID(i);

      const bool is_dir = file.is_directory;
      const char* filename = file.name.c_str();

      std::string texture_name = {};
      if (!is_dir && EditorCVar::cvar_file_thumbnails.get()) {
        if (file.type == FileType::Texture) {
          if (thumbnail_cache_textures.contains(file.file_path)) {
            texture_name = file.file_path;
          } else {
            // make sure this runs only if it's not already queued
            if (ThreadManager::get()->asset_thread.get_queue_size() == 0) {
              ThreadManager::get()->asset_thread.queue_job([this, file_path = file.file_path] {
                auto thumbnail_texture = std::make_shared<Texture>();
                thumbnail_texture->create(file_path, {.preset = Preset::eRTT2DUnmipped});
                thumbnail_cache_textures.emplace(file_path, thumbnail_texture);
              });
            }
            texture_name = file.file_path;
          }
        } else if (file.type == FileType::Mesh) {
          if (thumbnail_cache_meshes.contains(file.file_path)) {
            texture_name = file.file_path;
          } else if (mesh_thumbnails_enabled) {
            const auto name = fs::get_file_name(file.file_path);
            auto rp = std::make_unique<ThumbnailRenderPipeline>();
            rp->set_name(name);

            auto* asset_man = App::get_asset_manager();
            if (auto asset_uuid = asset_man->import_asset(file.file_path); asset_uuid) {
              if (asset_man->load_mesh(asset_uuid)) {
                auto* mesh_asset = asset_man->get_mesh(asset_uuid);
                rp->set_mesh(mesh_asset);
              }
            }

            auto thumb = rp->on_render(
                               vk_context,
                               RenderPipeline::RenderInfo{.extent = {(u32)thumb_image_size, (u32)thumb_image_size, 1},
                                                          .format = vuk::Format::eR8G8B8A8Srgb})
                             .as_released(vuk::eFragmentSampled, vuk::DomainFlagBits::eGraphicsQueue);

            auto ia = vk_context.wait_on_rg(std::move(thumb), false);

            thumbnail_render_pipeline_cache.emplace(file.file_path, std::move(rp));
            thumbnail_cache_meshes.emplace(file.file_path, std::move(ia));
            texture_name = file.file_path;
          }
        }
      }

      ImGui::TableNextColumn();

      const auto& path = file.directory_entry.path();
      std::string str_path = path.string();

      if (grid) {
        cursor_pos = ImGui::GetCursorPos();

        bool highlight = false;
        if (editor_context.type == EditorContext::Type::File) {
          highlight = file.file_path == editor_context.str.value_or(std::string{});
        }

        // Background button
        static std::string id = "###";
        id[2] = static_cast<char>(i);
        const bool clicked = UI::toggle_button(id.c_str(), highlight, background_thumbnail_size, 0.1f);
        if (clicked) {
          editor_context.reset();
          editor_context.type = EditorContext::Type::File;
          editor_context.str.emplace(str_path);
        }
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, editor_theme.popup_item_spacing);
        if (ImGui::BeginPopupContextItem()) {
          if (ImGui::MenuItem("Delete")) {
            _directory_to_delete = path;
            ImGui::CloseCurrentPopup();
          }
          if (ImGui::MenuItem("Rename")) {
            ImGui::CloseCurrentPopup();
          }

          ImGui::Separator();

          draw_context_menu_items(path, is_dir);
          ImGui::EndPopup();
        }
        ImGui::PopStyleVar();

        if (is_dir)
          drag_drop_target(file.file_path);

        drag_drop_from(file.file_path);

        if (ImGui::IsItemHovered())
          any_item_hovered = true;

        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
          if (is_dir) {
            directory_to_open = path;
            m_filter.Clear();
          } else {
            open_file(path);
            editor_context.reset();
          }
        }

        // Foreground Image
        ImGui::SetCursorPos({cursor_pos.x + padding, cursor_pos.y + padding});
        ImGui::SetItemAllowOverlap();
        UI::image(*_white_texture,
                  {background_thumbnail_size.x - padding * 2.f, background_thumbnail_size.y - padding * 2.f},
                  {},
                  {},
                  EditorLayer::get()->editor_theme.window_bg_alternative_color);

        // Thumbnail Image
        ImGui::SetCursorPos({cursor_pos.x + thumbnail_padding * 0.75f, cursor_pos.y + thumbnail_padding});
        ImGui::SetItemAllowOverlap();
        if (thumbnail_cache_textures.contains(texture_name)) {
          UI::image(*thumbnail_cache_textures[texture_name], {thumb_image_size, thumb_image_size});
        } else if (thumbnail_cache_meshes.contains(texture_name)) {
          auto texture = Texture::from_attachment(*vk_context.frame_allocator, thumbnail_cache_meshes[texture_name]);
          texture->set_name(fs::get_file_name(texture_name));
          UI::image(*texture, {thumb_image_size, thumb_image_size});
        } else {
          auto file_type = FileType::Unknown;
          const auto& file_type_it = FILE_TYPES.find(file.extension.empty() ? "" : file.extension);
          if (file_type_it != FILE_TYPES.end()) {
            file_type = file_type_it->second;
          }
          ImGui::PushFontSize(thumb_image_size);
          ImGui::TextUnformatted(StringUtils::from_char8_t(FILE_TYPES_TO_ICON.at(file_type)));
          ImGui::PopFontSize();
        }

        // Type Color frame
        const ImVec2 type_color_frame_size = {scaled_thumbnail_size_x, scaled_thumbnail_size_x * 0.03f};
        ImGui::SetCursorPosX(cursor_pos.x + padding);
        UI::image(*_white_texture,
                  type_color_frame_size,
                  {0, 0},
                  {1, 1},
                  is_dir ? ImVec4(0.0f, 0.0f, 0.0f, 0.0f) : file.file_type_indicator_color);

        const ImVec2 rect_min = ImGui::GetItemRectMin();
        const ImVec2 rect_size = ImGui::GetItemRectSize();
        const ImRect clip_rect = ImRect(
            {rect_min.x + padding * 1.0f, rect_min.y + padding * 2.0f},
            {rect_min.x + rect_size.x,
             rect_min.y + scaled_thumbnail_size_x - editor_theme.regular_font_size * 2.0f});
        ImGui::PushFontSize(14.f);
        UI::clipped_text(
            clip_rect.Min, clip_rect.Max, filename, nullptr, nullptr, {0, 0}, nullptr, clip_rect.GetSize().x);
        ImGui::PopFontSize();

        if (!is_dir) {
          constexpr auto y_pos_pad = 10.f;
          ImGui::SetCursorPos(
              {cursor_pos.x + padding * 2.0f,
               cursor_pos.y + background_thumbnail_size.y - editor_theme.small_font_size * 2.0f + y_pos_pad});
          ImGui::BeginDisabled();
          ImGui::PushFontSize(editor_theme.small_font_size);
          ImGui::TextUnformatted(file.file_type_string.data());
          ImGui::PopFontSize();
          ImGui::EndDisabled();
        }
      } else {
        constexpr ImGuiTreeNodeFlags tree_node_flags = ImGuiTreeNodeFlags_FramePadding |
                                                       ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_Leaf;

        const bool opened = ImGui::TreeNodeEx(file.name.c_str(), tree_node_flags, "");

        if (ImGui::IsItemHovered())
          any_item_hovered = true;

        if (is_dir && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
          directory_to_open = path;
          m_filter.Clear();
        }

        drag_drop_from(file.file_path.c_str());

        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() - line_height);
        if (thumbnail_cache_textures.contains(texture_name)) {
          UI::image(*thumbnail_cache_textures[texture_name], {thumb_image_size, thumb_image_size});
        } else {
          auto file_type = FileType::Unknown;
          const auto& file_type_it = FILE_TYPES.find(file.extension.empty() ? "" : file.extension);
          if (file_type_it != FILE_TYPES.end()) {
            file_type = file_type_it->second;
          }
          ImGui::TextUnformatted(StringUtils::from_char8_t(FILE_TYPES_TO_ICON.at(file_type)));
        }
        ImGui::SameLine();

        ImGui::TextUnformatted(filename);

        if (opened)
          ImGui::TreePop();
      }

      ImGui::PopID();
      ++i;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, editor_theme.popup_item_spacing);
    if (ImGui::BeginPopupContextWindow("AssetPanelHierarchyContextWindow",
                                       ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
      editor_context.reset();
      draw_context_menu_items(_current_directory, true);
      ImGui::EndPopup();
    }
    ImGui::PopStyleVar();

    ImGui::EndTable();

    if (!any_item_hovered && ImGui::IsItemClicked())
      editor_context.reset();
  }

  ImGui::PopStyleVar();

  if (!_directory_to_delete.empty()) {
    if (!ImGui::IsPopupOpen("Delete?"))
      ImGui::OpenPopup("Delete?");
  }

  if (ImGui::BeginPopupModal("Delete?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("%s will be deleted. \nAre you sure? This operation cannot be undone!\n\n",
                _directory_to_delete.string().c_str());
    ImGui::Separator();
    if (ImGui::Button("OK", ImVec2(120, 0))) {
      std::filesystem::remove_all(_directory_to_delete);
      _directory_to_delete.clear();
      ThreadManager::get()->asset_thread.queue_job([this] { refresh(); });
      ImGui::CloseCurrentPopup();
    }
    ImGui::SetItemDefaultFocus();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
      ImGui::CloseCurrentPopup();
      _directory_to_delete.clear();
    }
    editor_context.reset();
    ImGui::EndPopup();
  }

  if (!directory_to_open.empty())
    update_directory_entries(directory_to_open);
}

void ContentPanel::update_directory_entries(const std::filesystem::path& directory) {
  ZoneScoped;
  std::lock_guard lock(_directory_mutex);
  _current_directory = directory;
  _directory_entries.clear();

  if (directory.empty())
    return;

  const auto directory_it = std::filesystem::directory_iterator(directory);
  for (auto& directory_entry : directory_it) {
    const auto& path = directory_entry.path();
    const auto relative_path = relative(path, _assets_directory);
    const std::string filename = relative_path.filename().string();
    const std::string extension = relative_path.extension().string();

    auto file_type = FileType::Unknown;
    const auto& file_type_it = FILE_TYPES.find(extension);
    if (file_type_it != FILE_TYPES.end())
      file_type = file_type_it->second;

    std::string_view file_type_string = FILE_TYPES_TO_STRING.at(FileType::Unknown);
    const auto& file_string_type_it = FILE_TYPES_TO_STRING.find(file_type);
    if (file_string_type_it != FILE_TYPES_TO_STRING.end())
      file_type_string = file_string_type_it->second;

    ImVec4 file_type_color = {1.0f, 1.0f, 1.0f, 1.0f};
    const auto& file_type_color_it = TYPE_COLORS.find(file_type);
    if (file_type_color_it != TYPE_COLORS.end())
      file_type_color = file_type_color_it->second;

    File entry = {filename,
                  path.string(),
                  extension,
                  directory_entry,
                  nullptr,
                  directory_entry.is_directory(),
                  file_type,
                  file_type_string,
                  file_type_color};

    _directory_entries.push_back(entry);
  }

  _elapsed_time = 0.0f;
}

void ContentPanel::draw_context_menu_items(const std::filesystem::path& context, bool isDir) {
  if (isDir) {
    if (ImGui::BeginMenu("Create")) {
      if (ImGui::MenuItem("Folder")) {
        int i = 0;
        bool created = false;
        std::string newFolderPath;
        while (!created) {
          std::string folderName = "New Folder" + (i == 0 ? "" : fmt::format(" ({})", i));
          newFolderPath = (context / folderName).string();
          created = std::filesystem::create_directory(newFolderPath);
          ++i;
        }
        auto& editor_context = EditorLayer::get()->get_context();
        editor_context.reset();
        editor_context.str = newFolderPath;
        editor_context.type = EditorContext::Type::File;
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndMenu();
    }
  }
  if (ImGui::MenuItem("Show in Explorer")) {
    fs::open_folder_select_file(context.string().c_str());
    ImGui::CloseCurrentPopup();
  }
  if (ImGui::MenuItem("Open")) {
    fs::open_file_externally(context.string().c_str());
    ImGui::CloseCurrentPopup();
  }
  if (ImGui::MenuItem("Copy Path")) {
    ImGui::SetClipboardText(context.string().c_str());
    ImGui::CloseCurrentPopup();
  }

  if (isDir) {
    if (ImGui::MenuItem("Refresh")) {
      refresh();
      ImGui::CloseCurrentPopup();
    }
  }
}
} // namespace ox
