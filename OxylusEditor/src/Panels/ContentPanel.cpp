#include "ContentPanel.hpp"

#include <filesystem>
#include <icons/IconsMaterialDesignIcons.h>
#include <imgui.h>
#include <imspinner.h>
#include <misc/cpp/imgui_stdlib.h>
#include <vuk/runtime/vk/AllocatorHelpers.hpp>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Core/VFS.hpp"
#include "Editor.hpp"
#include "OS/OS.hpp"
#include "UI/PayloadData.hpp"
#include "UI/UI.hpp"
#include "Utils/EditorConfig.hpp"

namespace ox {
static const ankerl::unordered_dense::map<FileType, const char*> FILE_TYPES_TO_STRING = {
  {FileType::Unknown, "Unknown"},
  {FileType::Directory, "Directory"},

  {FileType::Meta, "Meta"},
  {FileType::Scene, "Scene"},
  {FileType::Prefab, "Prefab"},
  {FileType::Shader, "Shader"},
  {FileType::Texture, "Texture"},
  {FileType::Model, "Model"},
  {FileType::Script, "Script"},
  {FileType::Audio, "Audio"},
};

static const ankerl::unordered_dense::map<std::string, FileType> FILE_TYPES = {
  {"", FileType::Unknown},                                                                     //
  {".oxasset", FileType::Meta},                                                                //
  {".oxscene", FileType::Scene},                                                               //
  {".oxprefab", FileType::Prefab},                                                             //
  {".hlsl", FileType::Shader},     {".hlsli", FileType::Shader}, {".glsl", FileType::Shader},  //
  {".frag", FileType::Shader},     {".vert", FileType::Shader},  {".slang", FileType::Shader}, //

  {".png", FileType::Texture},     {".jpg", FileType::Texture},  {".jpeg", FileType::Texture}, //
  {".bmp", FileType::Texture},     {".gif", FileType::Texture},  {".ktx", FileType::Texture},  //
  {".ktx2", FileType::Texture},    {".tiff", FileType::Texture},                               //

  {".gltf", FileType::Model},      {".glb", FileType::Model},                                  //

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
  {FileType::Model, {0.20f, 0.80f, 0.75f, 1.00f}},
  {FileType::Audio, {0.20f, 0.80f, 0.50f, 1.00f}},
  {FileType::Script, {0.0f, 16.0f, 121.0f, 1.00f}},
};

static const ankerl::unordered_dense::map<FileType, const char*> FILE_TYPES_TO_ICON = {
  {FileType::Unknown, ICON_MDI_FILE},
  {FileType::Directory, ICON_MDI_FOLDER},
  {FileType::Meta, ICON_MDI_FILE_DOCUMENT},
  {FileType::Scene, ICON_MDI_IMAGE_FILTER_HDR},
  {FileType::Prefab, ICON_MDI_FILE},
  {FileType::Shader, ICON_MDI_IMAGE_FILTER_BLACK_WHITE},
  {FileType::Texture, ICON_MDI_FILE_IMAGE},
  {FileType::Model, ICON_MDI_VECTOR_POLYGON},
  {FileType::Audio, ICON_MDI_MICROPHONE},
  {FileType::Script, ICON_MDI_LANGUAGE_LUA},
  {FileType::Material, ICON_MDI_PALETTE_SWATCH},
};

static bool drag_drop_target(const std::filesystem::path& drop_path) {
  if (ImGui::BeginDragDropTarget()) {
    const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(PayloadData::DRAG_DROP_TARGET);
    if (payload) {
      auto* asset = static_cast<PayloadData*>(payload->Data);

      auto& asset_man = App::mod<AssetManager>();

      std::filesystem::path file_path = {};
      u32 counter = 0;
      do {
        file_path = drop_path /
                    fmt::format("{}{}", asset->get_str(), (counter > 0 ? "_" + std::to_string(counter) : ""));
        counter++;
      } while (std::filesystem::exists(file_path / ".oxasset"));

      if (!asset_man.export_asset(asset->uuid, file_path))
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
    ImGui::TextUnformatted(path_str.c_str());
    ImGui::EndDragDropSource();
  }
}

static void open_file(const std::filesystem::path& path) {
  const auto ext = path.extension().string();
  const auto& file_type_it = FILE_TYPES.find(ext);
  if (file_type_it != FILE_TYPES.end()) {
    const FileType file_type = file_type_it->second;
    switch (file_type) {
      case FileType::Scene: {
        App::mod<Editor>().open_scene(path);
        break;
      }
      case FileType::Unknown: break;
      case FileType::Prefab : break;
      case FileType::Texture: break;
      case FileType::Shader : [[fallthrough]];
      case FileType::Script : {
        os::open_file_externally(path);
        break;
      }
      case ox::FileType::Material: break;
      default                    : break;
    }
  } else {
    os::open_file_externally(path);
  }
}

auto ContentPanel::directory_tree_view_recursive(
  const std::filesystem::path& path, u32* count, i32* selectionMask, ImGuiTreeNodeFlags flags
) -> std::pair<bool, u32> {
  ZoneScoped;

  auto& editor_theme = App::mod<Editor>().editor_theme;

  bool any_node_clicked = false;
  u32 node_clicked = 0;

  if (path.empty())
    return {};

  for (const auto& entry : std::filesystem::directory_iterator(path)) {
    ImGuiTreeNodeFlags nodeFlags = flags;

    auto& entry_path = entry.path();
    auto file_name_str = entry_path.filename().string();
    if (file_name_str.starts_with('.')) {
      continue;
    }

    const bool entry_is_file = !std::filesystem::is_directory(entry_path);
    if (entry_is_file)
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

    const u64 id = *count;
    const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(id), nodeFlags, "");
    ImGui::PopStyleColor(selected ? 2 : 1);

    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
      if (!entry_is_file)
        update_directory_entries(entry_path);

      node_clicked = *count;
      any_node_clicked = true;
    }

    if (!entry_is_file)
      drag_drop_target(entry_path);
    drag_drop_from(entry_path);

    const char* folder_icon = ICON_MDI_FILE;
    if (entry_is_file) {
      auto file_type = FileType::Unknown;
      const auto& file_type_it = FILE_TYPES.find(entry_path.extension().string());
      if (file_type_it != FILE_TYPES.end())
        file_type = file_type_it->second;

      const auto& file_type_icon_it = FILE_TYPES_TO_ICON.find(file_type);
      if (file_type_icon_it != FILE_TYPES_TO_ICON.end())
        folder_icon = file_type_icon_it->second;
    } else {
      folder_icon = open ? ICON_MDI_FOLDER_OPEN : ICON_MDI_FOLDER;
    }

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, editor_theme.asset_icon_color);
    ImGui::TextUnformatted(folder_icon);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    auto name = entry_path.filename().string();
    ImGui::TextUnformatted(name.c_str());
    currently_visible_items_tree_view_++;

    (*count)--;

    if (!entry_is_file) {
      if (open) {
        const auto [isClicked, clickedNode] = directory_tree_view_recursive(entry_path, count, selectionMask, flags);

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

ContentPanel::ContentPanel() : EditorPanelState("Contents", ICON_MDI_FOLDER_STAR, true) {
  _white_texture = std::make_shared<Texture>();
  char white_texture_data[16 * 16 * 4];
  memset(white_texture_data, 0xff, 16 * 16 * 4);
  _white_texture->create(
    {},
    {.preset = Preset::eRTT2DUnmipped,
     .format = vuk::Format::eR8G8B8A8Unorm,
     .mime = {},
     .loaded_data = white_texture_data,
     .extent = vuk::Extent3D{.width = 16u, .height = 16u, .depth = 1u}}
  );
}

void ContentPanel::init(this ContentPanel& self) {
  auto vfs = App::get_vfs();
  if (!vfs.is_mounted_dir(VFS::PROJECT_DIR))
    return;

  auto assets_dir = vfs.resolve_physical_dir(VFS::PROJECT_DIR, "");
  self.assets_directory_ = assets_dir;
  self.current_directory_ = self.assets_directory_;
  self.refresh();

  self.thumbnail_manager.init();

  self.filewatch = std::make_unique<filewatch::FileWatch<std::string>>(
    self.assets_directory_.string(),
    [&self](const auto&, const filewatch::Event e) { self.refresh(); }
  );
}

void ContentPanel::on_update(this ContentPanel& self) {
  ZoneScoped;

  self.elapsed_time_ += static_cast<float>(App::get_timestep());

  self.thumbnail_manager.update();
}

void ContentPanel::on_render(this ContentPanel& self, vuk::ImageAttachment swapchain_attachment) {
  constexpr ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar;

  constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_Resizable | ImGuiTableFlags_ContextMenuInBody;

  if (self.assets_directory_.empty()) {
    self.init();
  }

  self.on_begin(windowFlags);
  {
    self.render_header();
    ImGui::Separator();
    const ImVec2 availableRegion = ImGui::GetContentRegionAvail();
    if (ImGui::BeginTable("MainViewTable", 2, tableFlags, availableRegion)) {
      ImGui::TableSetupColumn("##side_view", ImGuiTableColumnFlags_WidthFixed, 150);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      self.render_side_view();
      ImGui::TableNextColumn();
      self.render_body(EditorCVar::cvar_file_thumbnail_size.get() >= self.thumbnail_size_grid_limit);

      ImGui::EndTable();
    }
  }
  self.on_end();
}

void ContentPanel::render_header(this ContentPanel& self) {
  if (UI::button(ICON_MDI_COG))
    ImGui::OpenPopup("SettingsPopup");
  ImGui::SameLine();
  if (UI::button(ICON_MDI_REFRESH)) {
    self.refresh();
  }

  if (ImGui::BeginPopup("SettingsPopup")) {
    UI::begin_properties(ImGuiTableFlags_SizingStretchSame);
    UI::property("Show meta files", reinterpret_cast<bool*>(EditorCVar::cvar_show_meta_files.get_ptr()));
    UI::end_properties();
    ImGui::SeparatorText("Thumbnails");
    UI::begin_properties(ImGuiTableFlags_SizingStretchSame);
    UI::property(
      "Thumbnail Size",
      EditorCVar::cvar_file_thumbnail_size.get_ptr(),
      self.thumbnail_size_grid_limit - 0.1f,
      self.thumbnail_max_limit
    );
    UI::property("Show file thumbnails", reinterpret_cast<bool*>(EditorCVar::cvar_file_thumbnails.get_ptr()));
    UI::end_properties();
    if (UI::button("Reset thumbnail cache"))
      self.thumbnail_manager.reset();
    ImGui::EndPopup();
  }

  ImGui::SameLine();
  const float cursorPosX = ImGui::GetCursorPosX();
  self.filter_.Draw("###ConsoleFilter", ImGui::GetContentRegionAvail().x);
  if (!self.filter_.IsActive()) {
    ImGui::SameLine();
    ImGui::SetCursorPosX(cursorPosX + ImGui::GetFontSize() * 0.5f);
    ImGui::TextUnformatted(ICON_MDI_MAGNIFY " Search...");
  }

  ImGui::Spacing();
  ImGui::Spacing();

  // Back button
  {
    bool disabled_back_button = false;
    if (self.current_directory_ == self.assets_directory_)
      disabled_back_button = true;

    if (disabled_back_button) {
      ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
      ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
    }

    if (UI::button(ICON_MDI_ARROW_LEFT_CIRCLE_OUTLINE)) {
      self.back_stack_.push(self.current_directory_);
      self.update_directory_entries(self.current_directory_.parent_path());
    }

    if (disabled_back_button) {
      ImGui::PopStyleVar();
      ImGui::PopItemFlag();
    }
  }

  ImGui::SameLine();

  // Front button
  {
    bool disabled_front_button = false;
    if (self.back_stack_.empty())
      disabled_front_button = true;

    if (disabled_front_button) {
      ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
      ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
    }

    if (UI::button(ICON_MDI_ARROW_RIGHT_CIRCLE_OUTLINE)) {
      const auto& top = self.back_stack_.top();
      self.update_directory_entries(top);
      self.back_stack_.pop();
    }

    if (disabled_front_button) {
      ImGui::PopStyleVar();
      ImGui::PopItemFlag();
    }
  }

  std::filesystem::path directory_to_open = {};

  ImGui::SameLine();
  if (UI::button(ICON_MDI_HOME)) {
    directory_to_open = self.assets_directory_;
  }

  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_Button, {0.0f, 0.0f, 0.0f, 0.0f});
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.0f, 0.0f, 0.0f, 0.0f});
  std::filesystem::path current = self.assets_directory_.parent_path();
  const std::filesystem::path current_directory = std::filesystem::relative(self.current_directory_, current);

  ImGui::SameLine();
  ImGui::TextUnformatted(ICON_MDI_FOLDER);
  for (const auto& path : current_directory) {
    current /= path;
    ImGui::SameLine();
    auto button_str = current.filename().string();
    if (ImGui::Button(button_str.c_str())) {
      directory_to_open = current;
    }

    if (self.current_directory_ != current) {
      ImGui::SameLine();
      ImGui::TextUnformatted("/");
    }
  }
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar();

  if (!directory_to_open.empty())
    self.update_directory_entries(directory_to_open);
}

void ContentPanel::render_side_view(this ContentPanel& self) {
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

    const auto& editor_theme = App::mod<Editor>().editor_theme;

    ImGuiTreeNodeFlags node_flags = tree_node_flags;
    const bool selected = self.current_directory_ == self.assets_directory_ && selection_mask == 0;
    if (selected) {
      node_flags |= ImGuiTreeNodeFlags_Selected;
      ImGui::PushStyleColor(ImGuiCol_Header, editor_theme.header_selected_color);
      ImGui::PushStyleColor(ImGuiCol_HeaderHovered, editor_theme.header_selected_color);
    } else {
      ImGui::PushStyleColor(ImGuiCol_HeaderHovered, editor_theme.header_hovered_color);
    }

    const bool opened = ImGui::TreeNodeEx(self.assets_directory_.string().c_str(), node_flags, "");
    ImGui::PopStyleColor(selected ? 2 : 1);

    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
      self.update_directory_entries(self.assets_directory_);
      selection_mask = 0;
    }
    const char* folderIcon = opened ? ICON_MDI_FOLDER_OPEN : ICON_MDI_FOLDER;
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, editor_theme.asset_icon_color);
    ImGui::TextUnformatted(folderIcon);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextUnformatted("Assets");

    if (opened) {
      u32 count = 0;
      const auto [is_clicked, clicked_node] = self.directory_tree_view_recursive(
        self.assets_directory_,
        &count,
        &selection_mask,
        tree_node_flags
      );

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
      App::mod<Editor>().get_context().reset();
  }

  ImGui::PopStyleVar();
}

void ContentPanel::render_body(this ContentPanel& self, bool grid) {
  const auto& editor_theme = App::mod<Editor>().editor_theme;
  auto& editor_context = App::mod<Editor>().get_context();
  auto& render_context = App::get_rendercontext();

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
  i32 column_count = static_cast<i32>(panel_width / cell_size);
  if (column_count < 1)
    column_count = 1;

  float line_height = ImGui::GetTextLineHeight();
  i32 flags = ImGuiTableFlags_ContextMenuInBody | ImGuiTableFlags_ScrollY;

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

  ImGui::SetNextItemAllowOverlap();
  ImGui::SetCursorPos(cursor_pos);

  if (ImGui::BeginTable("BodyTable", column_count, flags)) {
    bool any_item_hovered = false;

    i32 i = 0;

    auto read_lock = std::shared_lock(self.directory_mutex_);
    for (auto& file : self.directory_entries_) {
      if (!self.filter_.PassFilter(file.name.c_str()))
        continue;

      if (!(bool)EditorCVar::cvar_show_meta_files.get()) {
        if (file.type == FileType::Meta)
          continue;
      }

      ImGui::PushID(i);

      const bool is_dir = file.is_directory;
      const char* filename = file.name.c_str();

      auto file_path_str = file.file_path.string();

      ImGui::TableNextColumn();

      const auto& path = file.directory_entry.path();
      std::string str_path = path.string();

      if (grid) {
        cursor_pos = ImGui::GetCursorPos();

        bool highlight = false;
        if (editor_context.type == EditorContext::Type::File) {
          highlight = file_path_str == editor_context.str.value_or(std::string{});
        }

        // Background button
        static std::string id = "###";
        id[2] = static_cast<char>(i);
        const bool clicked = UI::toggle_button(id.c_str(), highlight, background_thumbnail_size, 0.1f);
        if (clicked) {
          editor_context.reset(EditorContext::Type::File, str_path);
        }
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, editor_theme.popup_item_spacing);
        if (ImGui::BeginPopupContextItem()) {
          if (ImGui::MenuItem("Delete")) {
            self._directory_to_delete = path;
            ImGui::CloseCurrentPopup();
          }
          if (ImGui::MenuItem("Rename")) {
            ImGui::CloseCurrentPopup();
          }

          ImGui::Separator();

          if (auto p = self.draw_context_menu_items(path, is_dir); !p.empty()) {
            directory_to_open = p;
          }
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
            self.filter_.Clear();
          } else {
            open_file(path);
            editor_context.reset();
          }
        }

        // Foreground Image
        ImGui::SetCursorPos({cursor_pos.x + padding, cursor_pos.y + padding});
        ImGui::SetNextItemAllowOverlap();
        UI::image(
          *self._white_texture,
          {background_thumbnail_size.x - padding * 2.f, background_thumbnail_size.y - padding * 2.f},
          {},
          {},
          App::mod<Editor>().editor_theme.window_bg_alternative_color
        );

        // Thumbnail Image
        ImGui::SetCursorPos({cursor_pos.x + thumbnail_padding * 0.75f, cursor_pos.y + thumbnail_padding});
        ImGui::SetNextItemAllowOverlap();

        auto use_thumbnail_image = !is_dir && EditorCVar::cvar_file_thumbnails.get() &&
                                   (file.type == FileType::Texture || file.type == FileType::Model);
        auto thumbnail_image = option<std::shared_ptr<Texture>>(nullopt);
        if (use_thumbnail_image) {
          if (file.type == FileType::Texture) {
            thumbnail_image = self.thumbnail_manager.get_thumbnail_texture(file_path_str);
          } else if (file.type == FileType::Model) {
            thumbnail_image = self.thumbnail_manager.get_thumbnail_model(file_path_str);
          }
        }
        if (use_thumbnail_image) {
          if (thumbnail_image.has_value()) {
            UI::image(**thumbnail_image, {thumb_image_size, thumb_image_size});
          } else {
            ImSpinner::detail::SpinnerConfig config{};
            config.setSpinnerType(ImSpinner::e_st_ang);
            config.setSpeed(6.f);
            config.setAngle(4.f);
            config.setThickness(2.f);
            config.setRadius(thumb_image_size / 2.f);
            config.setColor(ImColor(1.f, 1.f, 1.f, 1.f));
            ImGui::PushFont(nullptr, thumb_image_size);
            ImSpinner::Spinner("SpinnerAng270NoBg", config);
            ImGui::PopFont();
          }
        } else {
          ImGui::PushFont(nullptr, thumb_image_size);
          ImGui::TextUnformatted(file.icon.c_str());
          ImGui::PopFont();
        }

        // Type color frame
        const ImVec2 type_color_frame_size = {scaled_thumbnail_size_x, scaled_thumbnail_size_x * 0.03f};
        ImGui::SetCursorPosX(cursor_pos.x + padding);
        UI::image(
          *self._white_texture,
          type_color_frame_size,
          {0, 0},
          {1, 1},
          is_dir ? ImVec4(0.0f, 0.0f, 0.0f, 0.0f) : file.file_type_indicator_color
        );

        const ImVec2 rect_min = ImGui::GetItemRectMin();
        const ImVec2 rect_size = ImGui::GetItemRectSize();
        const ImRect clip_rect = ImRect(
          {rect_min.x + padding * 1.0f, rect_min.y + padding * 2.0f},
          {rect_min.x + rect_size.x, rect_min.y + scaled_thumbnail_size_x - editor_theme.regular_font_size * 2.0f}
        );
        ImGui::PushFont(nullptr, 14.f);
        UI::clipped_text(
          clip_rect.Min,
          clip_rect.Max,
          filename,
          nullptr,
          nullptr,
          {0, 0},
          nullptr,
          clip_rect.GetSize().x
        );
        ImGui::PopFont();

        if (!is_dir) {
          constexpr auto y_pos_pad = 10.f;
          ImGui::SetCursorPos(
            {cursor_pos.x + padding * 2.0f,
             cursor_pos.y + background_thumbnail_size.y - editor_theme.small_font_size * 2.0f + y_pos_pad}
          );
          ImGui::BeginDisabled();
          ImGui::PushFont(nullptr, editor_theme.small_font_size);
          ImGui::TextUnformatted(file.file_type_string.data());
          ImGui::PopFont();
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
          self.filter_.Clear();
        }

        drag_drop_from(file.file_path.c_str());

        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() - line_height);

        auto file_type = FileType::Unknown;
        auto ext_str = file.file_path.has_extension() ? "" : file.file_path.extension().string();
        const auto& file_type_it = FILE_TYPES.find(ext_str);
        if (file_type_it != FILE_TYPES.end()) {
          file_type = file_type_it->second;
        }
        auto file_icon = FILE_TYPES_TO_ICON.at(file_type);
        ImGui::TextUnformatted(file_icon);
        ImGui::SameLine();

        ImGui::TextUnformatted(filename);

        if (opened)
          ImGui::TreePop();
      }

      ImGui::PopID();
      ++i;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, editor_theme.popup_item_spacing);
    if (
      ImGui::BeginPopupContextWindow(
        "AssetPanelHierarchyContextWindow",
        ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems
      )
    ) {
      editor_context.reset();
      if (auto p = self.draw_context_menu_items(self.current_directory_, true); !p.empty()) {
        directory_to_open = p;
      }
      ImGui::EndPopup();
    }
    ImGui::PopStyleVar();

    ImGui::EndTable();

    if (!any_item_hovered && ImGui::IsItemClicked())
      editor_context.reset();
  }

  ImGui::PopStyleVar();

  if (!self._directory_to_delete.empty()) {
    if (!ImGui::IsPopupOpen("Delete?"))
      ImGui::OpenPopup("Delete?");
  }

  if (ImGui::BeginPopupModal("Delete?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text(
      "%s will be deleted. \nAre you sure? This operation cannot be undone!\n\n",
      self._directory_to_delete.string().c_str()
    );
    ImGui::Separator();
    if (ImGui::Button("OK", ImVec2(120, 0))) {
      std::filesystem::remove_all(self._directory_to_delete);
      self._directory_to_delete.clear();
      self.refresh();
      ImGui::CloseCurrentPopup();
    }
    ImGui::SetItemDefaultFocus();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
      ImGui::CloseCurrentPopup();
      self._directory_to_delete.clear();
    }
    editor_context.reset();
    ImGui::EndPopup();
  }

  if (self.should_open_new_asset_popup)
    ImGui::OpenPopup("New Material");

  if (ImGui::BeginPopupModal("New Material", nullptr, ImGuiWindowFlags_NoResize)) {
    UI::begin_properties();
    UI::input_text("Name", &self.new_asset_name_);
    UI::end_properties();

    if (ImGui::Button("Create", ImVec2(120, 0))) {
      if (!self.new_asset_name_.empty()) {
        auto& asset_man = App::mod<AssetManager>();
        auto asset = asset_man.create_asset(AssetType::Material, self.current_directory_.string());
        asset_man.load_asset(asset);
        if (asset_man.export_asset(asset, (self.current_directory_ / self.new_asset_name_).string())) {
          OX_LOG_INFO("Created new material asset {}", self.new_asset_name_);
          self.refresh();
        } else {
          OX_LOG_ERROR("Couldn't create material asset {}", self.new_asset_name_);
        }
        self.new_asset_name_.clear();
        self.should_open_new_asset_popup = false;
        ImGui::CloseCurrentPopup();
      }
    }

    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
      self.new_asset_name_.clear();
      self.should_open_new_asset_popup = false;
      ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
  }

  if (!directory_to_open.empty())
    self.update_directory_entries(directory_to_open);
}

void ContentPanel::update_directory_entries(this ContentPanel& self, const std::filesystem::path& directory) {
  ZoneScoped;

  std::unique_lock lock(self.directory_mutex_);
  self.current_directory_ = directory;
  self.directory_entries_.clear();

  if (directory.empty())
    return;

  const auto directory_it = std::filesystem::directory_iterator(directory);
  for (auto& directory_entry : directory_it) {
    const auto& path = directory_entry.path();
    auto file_name_str = path.filename().string();
    const auto relative_path = std::filesystem::relative(path, self.assets_directory_);
    auto extension_str = path.extension().string();

    if (file_name_str.starts_with('.'))
      continue;

    auto file_type = FileType::Unknown;
    const auto& file_type_it = FILE_TYPES.find(extension_str);
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

    const auto file_icon = directory_entry.is_directory() ? ICON_MDI_FOLDER : FILE_TYPES_TO_ICON.at(file_type);

    File entry = {
      path.filename().string(),
      path,
      directory_entry,
      nullptr,
      file_icon,
      directory_entry.is_directory(),
      file_type,
      file_type_string,
      file_type_color
    };

    self.directory_entries_.push_back(entry);
  }

  self.elapsed_time_ = 0.0f;
}

void ContentPanel::refresh(this ContentPanel& self) {
  ZoneScoped;

  self.update_directory_entries(self.current_directory_);
}

auto ContentPanel::draw_context_menu_items(this ContentPanel& self, const std::filesystem::path& context, bool is_dir)
  -> std::filesystem::path {
  ZoneScoped;

  std::filesystem::path dir_to_open = {};

  if (ImGui::MenuItem("Open")) {
    if (is_dir) {
      dir_to_open = context.string();
    } else {
      os::open_file_externally(context);
    }
  }
  if (is_dir) {
    if (ImGui::BeginMenu("Create")) {
      if (ImGui::MenuItem("Folder")) {
        i32 i = 0;
        bool created = false;
        std::string new_folder_path;
        while (!created) {
          std::string folder_name = "New Folder" + (i == 0 ? "" : fmt::format(" ({})", i));
          new_folder_path = (context / folder_name).string();
          created = std::filesystem::create_directory(new_folder_path);
          ++i;
        }
        auto& editor_context = App::mod<Editor>().get_context();
        editor_context.reset(EditorContext::Type::File, new_folder_path);
      }
      if (ImGui::MenuItem("Material")) {
        self.new_asset_name_.clear();
        self.should_open_new_asset_popup = true;
      }
      ImGui::EndMenu();
    }
  }
  if (ImGui::MenuItem("Show in Explorer")) {
    os::open_folder_select_file(context);
  }
  if (ImGui::MenuItem("Copy Path")) {
    auto str = context.string();
    ImGui::SetClipboardText(str.c_str());
  }

  if (is_dir) {
    if (ImGui::MenuItem("Refresh")) {
      self.refresh();
    }
  }

  return dir_to_open;
}
} // namespace ox
