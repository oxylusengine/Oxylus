#include "ContentPanel.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fmt/chrono.h>
#include <icons/IconsMaterialDesignIcons.h>
#include <imgui.h>
#include <imspinner.h>
#include <misc/cpp/imgui_stdlib.h>
#include <mutex>
#include <vuk/runtime/vk/AllocatorHelpers.hpp>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Core/VFS.hpp"
#include "Editor.hpp"
#include "Memory/Stack.hpp"
#include "OS/OS.hpp"
#include "ParticleEditorPanel.hpp"
#include "UI/PayloadData.hpp"
#include "UI/UI.hpp"

namespace ox {
static auto is_ascii_digit(const c8 value) -> bool { return value >= '0' && value <= '9'; }

static auto ascii_to_lower(const c8 value) -> c8 {
  if (value >= 'A' && value <= 'Z')
    return static_cast<c8>(value + ('a' - 'A'));
  return value;
}

static auto compare_natural_case_insensitive(std::string_view lhs, std::string_view rhs) -> i32 {
  usize lhs_index = 0;
  usize rhs_index = 0;

  while (lhs_index < lhs.size() && rhs_index < rhs.size()) {
    if (is_ascii_digit(lhs[lhs_index]) && is_ascii_digit(rhs[rhs_index])) {
      const usize lhs_run_begin = lhs_index;
      const usize rhs_run_begin = rhs_index;

      while (lhs_index < lhs.size() && is_ascii_digit(lhs[lhs_index]))
        ++lhs_index;
      while (rhs_index < rhs.size() && is_ascii_digit(rhs[rhs_index]))
        ++rhs_index;

      usize lhs_significant = lhs_run_begin;
      usize rhs_significant = rhs_run_begin;
      while (lhs_significant < lhs_index && lhs[lhs_significant] == '0')
        ++lhs_significant;
      while (rhs_significant < rhs_index && rhs[rhs_significant] == '0')
        ++rhs_significant;

      const usize lhs_significant_length = lhs_index - lhs_significant;
      const usize rhs_significant_length = rhs_index - rhs_significant;
      if (lhs_significant_length != rhs_significant_length)
        return lhs_significant_length < rhs_significant_length ? -1 : 1;

      for (usize digit_index = 0; digit_index < lhs_significant_length; ++digit_index) {
        const c8 lhs_digit = lhs[lhs_significant + digit_index];
        const c8 rhs_digit = rhs[rhs_significant + digit_index];
        if (lhs_digit != rhs_digit)
          return lhs_digit < rhs_digit ? -1 : 1;
      }

      const usize lhs_run_length = lhs_index - lhs_run_begin;
      const usize rhs_run_length = rhs_index - rhs_run_begin;
      if (lhs_run_length != rhs_run_length)
        return lhs_run_length < rhs_run_length ? -1 : 1;

      continue;
    }

    const c8 lhs_char = ascii_to_lower(lhs[lhs_index]);
    const c8 rhs_char = ascii_to_lower(rhs[rhs_index]);
    if (lhs_char != rhs_char)
      return static_cast<unsigned char>(lhs_char) < static_cast<unsigned char>(rhs_char) ? -1 : 1;

    ++lhs_index;
    ++rhs_index;
  }

  if (lhs_index != lhs.size() || rhs_index != rhs.size())
    return lhs_index == lhs.size() ? -1 : 1;

  const i32 exact_comparison = lhs.compare(rhs);
  if (exact_comparison == 0)
    return 0;
  return exact_comparison < 0 ? -1 : 1;
}

static auto format_file_size(const u64 size_bytes) -> std::string {
  constexpr std::string_view SIZE_UNITS[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  constexpr f64 UNIT_SIZE = 1024.0;

  if (size_bytes < 1024)
    return fmt::format("{} B", size_bytes);

  f64 display_size = static_cast<f64>(size_bytes);
  usize unit_index = 0;
  while (display_size >= UNIT_SIZE && unit_index + 1 < std::size(SIZE_UNITS)) {
    display_size /= UNIT_SIZE;
    ++unit_index;
  }

  return fmt::format("{:.1f} {}", display_size, SIZE_UNITS[unit_index]);
}

static auto format_modified_time(const std::filesystem::file_time_type modified_time) -> std::string {
  const auto system_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
    std::filesystem::file_time_type::clock::to_sys(modified_time)
  );
  const auto time = std::chrono::system_clock::to_time_t(system_time);
  std::tm local_time = {};
#if defined(_WIN32)
  if (localtime_s(&local_time, &time) != 0)
    return "—";
#else
  if (localtime_r(&time, &local_time) == nullptr)
    return "—";
#endif
  return fmt::format("{:%Y-%m-%d %H:%M}", local_time);
}

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
  {FileType::Material, "Material"},
  {FileType::Terrain, "Terrain"},
  {FileType::ParticleSystem, "Particle System"},
};

static const ankerl::unordered_dense::map<std::string, FileType> FILE_TYPES = {
  {"", FileType::Unknown},                   //
  {".oxasset", FileType::Meta},              //
  {".oxscene", FileType::Scene},             //
  {".oxprefab", FileType::Prefab},           //
  {".oxterrain", FileType::Terrain},         //
  {".oxparticle", FileType::ParticleSystem}, //
  {".hlsl", FileType::Shader},
  {".hlsli", FileType::Shader},
  {".glsl", FileType::Shader},               //
  {".frag", FileType::Shader},
  {".vert", FileType::Shader},
  {".slang", FileType::Shader},              //

  {".png", FileType::Texture},
  {".jpg", FileType::Texture},
  {".jpeg", FileType::Texture}, //
  {".bmp", FileType::Texture},
  {".gif", FileType::Texture},
  {".ktx", FileType::Texture},  //
  {".ktx2", FileType::Texture},
  {".tiff", FileType::Texture}, //

  {".gltf", FileType::Model},
  {".glb", FileType::Model},    //

  {".mp3", FileType::Audio},
  {".m4a", FileType::Audio},
  {".wav", FileType::Audio},  //
  {".ogg", FileType::Audio},  //

  {".lua", FileType::Script}, //
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
  {FileType::Material, {0.85f, 0.60f, 0.15f, 1.00f}},
  {FileType::Terrain, {0.45f, 0.70f, 0.30f, 1.00f}},
  {FileType::ParticleSystem, {0.60f, 0.35f, 0.85f, 1.00f}},
};

static const ankerl::unordered_dense::map<FileType, const char*> FILE_TYPES_TO_ICON = {
  {FileType::Unknown, ICON_MDI_FILE},
  {FileType::Directory, ICON_MDI_FOLDER},
  {FileType::Meta, ICON_MDI_FILE_DOCUMENT},
  {FileType::Scene, ICON_MDI_FILE_TREE},
  {FileType::Prefab, ICON_MDI_FILE},
  {FileType::Shader, ICON_MDI_IMAGE_FILTER_BLACK_WHITE},
  {FileType::Texture, ICON_MDI_FILE_IMAGE},
  {FileType::Model, ICON_MDI_VECTOR_POLYGON},
  {FileType::Audio, ICON_MDI_MICROPHONE},
  {FileType::Script, ICON_MDI_LANGUAGE_LUA},
  {FileType::Material, ICON_MDI_PALETTE_SWATCH},
  {FileType::Terrain, ICON_MDI_TERRAIN},
  {FileType::ParticleSystem, ICON_MDI_SHIMMER},
};

static auto standalone_asset_file_type(const std::filesystem::path& path) -> option<FileType> {
  auto companion_path = path;
  companion_path.replace_extension("");
  if (std::filesystem::exists(companion_path)) {
    return nullopt;
  }

  auto& asset_man = App::mod<AssetManager>();
  auto meta_file = asset_man.read_meta_file(path);
  if (!meta_file) {
    return nullopt;
  }

  auto type_json = meta_file->doc["type"].get_number();
  if (type_json.error()) {
    return nullopt;
  }

  switch (static_cast<AssetType>(type_json.value_unsafe().get_uint64())) {
    case AssetType::Material: return FileType::Material;
    default                 : return nullopt;
  }
}

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
      } while (std::filesystem::exists(fmt::format("{}.oxasset", file_path)));

      if (!asset_man.export_asset(asset->uuid, file_path))
        OX_LOG_ERROR("Couldn't export asset!");

      ImGui::EndDragDropTarget();
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
      case ox::FileType::Material  : break;
      case FileType::ParticleSystem: {
        // `open_asset` loads it and holds the ref for as long as the panel shows it
        if (const auto uuid = App::mod<AssetManager>().import_asset(path)) {
          App::mod<Editor>().editor_panel_registry.get<ParticleEditorPanel>().open_asset(uuid);
        }
        break;
      }
      default: break;
    }
  } else {
    os::open_file_externally(path);
  }
}

auto ContentPanel::sort_field_label(const SortField field) -> std::string_view {
  switch (field) {
    case SortField::Name        : return "Name";
    case SortField::DateModified: return "Date modified";
    case SortField::Type        : return "Type";
    case SortField::Size        : return "Size";
  }
  return "Name";
}

auto ContentPanel::sort_entries(std::span<File> entries, const SortField field, const bool ascending) -> void {
  std::ranges::stable_sort(entries, [field, ascending](const File& lhs, const File& rhs) -> bool {
    if (lhs.is_directory != rhs.is_directory)
      return lhs.is_directory;

    i32 primary_comparison = 0;
    switch (field) {
      case SortField::Name: {
        primary_comparison = compare_natural_case_insensitive(lhs.name, rhs.name);
        break;
      }
      case SortField::DateModified: {
        if (lhs.has_modified_time != rhs.has_modified_time)
          return lhs.has_modified_time;
        if (lhs.has_modified_time) {
          if (lhs.modified_time < rhs.modified_time)
            primary_comparison = -1;
          else if (lhs.modified_time > rhs.modified_time)
            primary_comparison = 1;
        }
        break;
      }
      case SortField::Type: {
        if (!lhs.is_directory)
          primary_comparison = compare_natural_case_insensitive(lhs.file_type_string, rhs.file_type_string);
        break;
      }
      case SortField::Size: {
        if (!lhs.is_directory) {
          if (lhs.has_size != rhs.has_size)
            return lhs.has_size;
          if (lhs.has_size) {
            if (lhs.size_bytes < rhs.size_bytes)
              primary_comparison = -1;
            else if (lhs.size_bytes > rhs.size_bytes)
              primary_comparison = 1;
          }
        }
        break;
      }
    }

    if (primary_comparison != 0)
      return ascending ? primary_comparison < 0 : primary_comparison > 0;

    const i32 name_comparison = compare_natural_case_insensitive(lhs.name, rhs.name);
    if (name_comparison != 0)
      return name_comparison < 0;

    return lhs.file_path < rhs.file_path;
  });
}

auto ContentPanel::set_sort(this ContentPanel& self, const SortField field, const bool ascending) -> void {
  if (self.sort_field_ == field && self.sort_ascending_ == ascending)
    return;

  {
    std::unique_lock lock(self.directory_mutex);
    self.sort_field_ = field;
    self.sort_ascending_ = ascending;
    sort_entries(std::span(self.directory_entries), field, ascending);
  }

  auto& editor_cvar = App::mod<Editor>().editor_cvar;
  editor_cvar.cvar_content_sort_field.set(static_cast<i32>(field));
  editor_cvar.cvar_content_sort_ascending.set(ascending);
}

auto ContentPanel::directory_tree_view_recursive(
  this ContentPanel& self,
  const std::filesystem::path& path,
  const std::filesystem::path& selected_directory,
  const ImGuiTreeNodeFlags flags
) -> void {
  ZoneScoped;

  if (path.empty())
    return;

  std::error_code iterator_error;
  std::vector<std::filesystem::path> directories;
  auto directory_it = std::filesystem::directory_iterator(
    path,
    std::filesystem::directory_options::skip_permission_denied,
    iterator_error
  );
  const auto directory_end = std::filesystem::directory_iterator();
  while (!iterator_error && directory_it != directory_end) {
    const auto& entry = *directory_it;
    const auto& entry_path = entry.path();
    const auto file_name = entry_path.filename().string();

    std::error_code type_error;
    const bool is_directory = entry.is_directory(type_error);
    if (!type_error && is_directory && !file_name.starts_with('.'))
      directories.emplace_back(entry_path);

    directory_it.increment(iterator_error);
  }

  std::ranges::sort(directories, [](const std::filesystem::path& lhs, const std::filesystem::path& rhs) -> bool {
    const auto lhs_name = lhs.filename().string();
    const auto rhs_name = rhs.filename().string();
    const i32 comparison = compare_natural_case_insensitive(lhs_name, rhs_name);
    if (comparison != 0)
      return comparison < 0;
    return lhs < rhs;
  });

  for (const auto& entry_path : directories) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();

    ImGuiTreeNodeFlags node_flags = flags;
    const bool selected = selected_directory == entry_path;
    if (selected)
      node_flags |= ImGuiTreeNodeFlags_Selected;

    ImVec4 active_color = ImGui::GetStyleColorVec4(ImGuiCol_Tab);
    ImVec4 hovered_color = ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered);
    ImGui::PushStyleColor(ImGuiCol_Header, active_color);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, selected ? active_color : hovered_color);

    const auto path_string = entry_path.string();
    const bool open = ImGui::TreeNodeEx(path_string.c_str(), node_flags, "");
    ImGui::PopStyleColor(2);

    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
      self.update_directory_entries(entry_path);

    drag_drop_target(entry_path);
    drag_drop_from(entry_path);

    ImGui::SameLine();
    if (selected)
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    ImGui::TextUnformatted(open ? ICON_MDI_FOLDER_OPEN : ICON_MDI_FOLDER);
    ImGui::SameLine();
    const auto entry_name = entry_path.filename().string();
    ImGui::TextUnformatted(entry_name.c_str());
    if (selected)
      ImGui::PopStyleColor();

    if (open) {
      self.directory_tree_view_recursive(entry_path, selected_directory, flags);
      ImGui::TreePop();
    }
  }
}

ContentPanel::ContentPanel() : EditorPanelState("Contents", ICON_MDI_FOLDER_STAR, true) {
  u8 white_texture_data[16 * 16 * 4];
  memset(white_texture_data, 0xff, 16 * 16 * 4);
  white_texture = Texture::create({
    .format = vuk::Format::eR8G8B8A8Unorm,
    .extent = vuk::Extent3D{.width = 16u, .height = 16u, .depth = 1u},
    .usage = vuk::ImageUsageFlagBits::eSampled,
  });
  white_texture.upload(std::span(white_texture_data), vuk::eFragmentSampled);
}

void ContentPanel::init(this ContentPanel& self) {
  auto vfs = App::get_vfs();
  if (!vfs.is_mounted_dir(VFS::PROJECT_DIR))
    return;

  auto& editor_cvar = App::mod<Editor>().editor_cvar;
  const i32 sort_field = editor_cvar.cvar_content_sort_field.get();
  if (sort_field >= static_cast<i32>(SortField::Name) && sort_field <= static_cast<i32>(SortField::Size))
    self.sort_field_ = static_cast<SortField>(sort_field);
  else
    self.sort_field_ = SortField::Name;
  self.sort_ascending_ = editor_cvar.cvar_content_sort_ascending.as_bool();

  auto assets_dir = vfs.resolve_physical_dir(VFS::PROJECT_DIR, "");
  self.assets_directory = assets_dir;
  self.current_directory = self.assets_directory;
  self.refresh();

  self.filewatch = std::make_unique<filewatch::FileWatch<std::string>>(
    self.assets_directory.string(),
    [&self](const auto&, const filewatch::Event e) { self.refresh(); }
  );
}

void ContentPanel::on_update(this ContentPanel& self) {
  ZoneScoped;

  self.elapsed_time_ += static_cast<float>(App::get_timestep());
}

void ContentPanel::on_render(this ContentPanel& self, vuk::ImageAttachment swapchain_attachment) {
  constexpr ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar;

  constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_Resizable | ImGuiTableFlags_ContextMenuInBody;

  if (self.assets_directory.empty()) {
    self.init();
  }

  self.on_begin(windowFlags);
  {
    self.render_header();
    ImGui::Separator();
    const ImVec2 availableRegion = ImGui::GetContentRegionAvail();
    if (ImGui::BeginTable("MainViewTable", 2, tableFlags, availableRegion)) {
      ImGui::TableSetupColumn("##side_view", ImGuiTableColumnFlags_WidthFixed, UI::scale(150.0f));
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      self.render_side_view();
      ImGui::TableNextColumn();
      self.render_body(App::mod<Editor>().editor_cvar.cvar_file_thumbnail_size.get() >= self.thumbnail_size_grid_limit);

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
  ImGui::SameLine();
  self.render_sort_menu();

  auto& editor_cvar = App::mod<Editor>().editor_cvar;

  if (ImGui::BeginPopup("SettingsPopup")) {
    UI::begin_properties(ImGuiTableFlags_SizingStretchSame);
    UI::property("Show meta files", reinterpret_cast<bool*>(editor_cvar.cvar_show_meta_files.get_ptr()));
    UI::end_properties();
    ImGui::SeparatorText("Thumbnails");
    UI::begin_properties(ImGuiTableFlags_SizingStretchSame);
    UI::property(
      "Thumbnail Size",
      editor_cvar.cvar_file_thumbnail_size.get_ptr(),
      self.thumbnail_size_grid_limit - 0.1f,
      self.thumbnail_max_limit
    );
    UI::property("Show file thumbnails", reinterpret_cast<bool*>(editor_cvar.cvar_file_thumbnails.get_ptr()));
    UI::end_properties();
    if (UI::button("Reset thumbnail cache"))
      App::mod<Editor>().thumbnail_manager.reset();
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
    if (self.current_directory == self.assets_directory)
      disabled_back_button = true;

    if (disabled_back_button) {
      ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
      ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
    }

    if (UI::button(ICON_MDI_ARROW_LEFT_CIRCLE_OUTLINE)) {
      self.back_stack.push(self.current_directory);
      self.update_directory_entries(self.current_directory.parent_path());
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
    if (self.back_stack.empty())
      disabled_front_button = true;

    if (disabled_front_button) {
      ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
      ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
    }

    if (UI::button(ICON_MDI_ARROW_RIGHT_CIRCLE_OUTLINE)) {
      const auto& top = self.back_stack.top();
      self.update_directory_entries(top);
      self.back_stack.pop();
    }

    if (disabled_front_button) {
      ImGui::PopStyleVar();
      ImGui::PopItemFlag();
    }
  }

  std::filesystem::path directory_to_open = {};

  ImGui::SameLine();
  if (UI::button(ICON_MDI_HOME)) {
    directory_to_open = self.assets_directory;
  }

  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_Button, {0.0f, 0.0f, 0.0f, 0.0f});
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.0f, 0.0f, 0.0f, 0.0f});

  std::filesystem::path displayed_directory;
  {
    auto read_lock = std::shared_lock(self.directory_mutex);
    displayed_directory = self.current_directory;
  }

  const std::filesystem::path normalized_root = self.assets_directory.lexically_normal();
  const std::filesystem::path normalized_directory = displayed_directory.lexically_normal();
  const std::filesystem::path relative_directory = normalized_directory.lexically_relative(normalized_root);
  std::filesystem::path current = self.assets_directory;

  ImGui::SameLine();
  ImGui::TextUnformatted(ICON_MDI_FOLDER);

  ImGui::SameLine();
  if (ImGui::Button("/###ContentBreadcrumbRoot") && normalized_directory != normalized_root)
    directory_to_open = self.assets_directory;

  if (!relative_directory.empty() && relative_directory != ".") {
    for (const auto& path : relative_directory) {
      current /= path;
      const auto normalized_current = current.lexically_normal();
      const auto button_str = path.string();

      ImGui::SameLine();
      ImGui::PushID(normalized_current.string().c_str());
      if (ImGui::Button(button_str.c_str()) && normalized_current != normalized_directory)
        directory_to_open = current;
      ImGui::PopID();

      if (normalized_current != normalized_directory) {
        ImGui::SameLine();
        ImGui::TextUnformatted("/");
      }
    }
  }
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar();

  if (!directory_to_open.empty() && directory_to_open.lexically_normal() != normalized_directory) {
    self.update_directory_entries(directory_to_open);
  } else if (self.refresh_requested) {
    self.refresh();
  }
  self.refresh_requested = false;
}

auto ContentPanel::render_sort_menu(this ContentPanel& self) -> void {
  memory::ScopedStack stack;

  const auto direction_icon = self.sort_ascending_ ? ICON_MDI_ARROW_UP : ICON_MDI_ARROW_DOWN;
  const auto button_label = stack.format_char(
    "{} {} {}###ContentSortButton",
    ICON_MDI_SORT_VARIANT,
    sort_field_label(self.sort_field_),
    direction_icon
  );
  if (UI::button(button_label))
    ImGui::OpenPopup("ContentSortPopup");

  UI::tooltip_hover(stack.format_char(
    "Sort by {} ({})",
    sort_field_label(self.sort_field_),
    self.sort_ascending_ ? "ascending" : "descending"
  ));

  if (!ImGui::BeginPopup("ContentSortPopup"))
    return;

  ImGui::SeparatorText("Sort by");
  const auto draw_sort_field = [&self](const SortField field) -> void {
    const bool selected = self.sort_field_ == field;
    if (ImGui::MenuItem(sort_field_label(field).data(), nullptr, selected) && !selected) {
      const bool default_ascending = field != SortField::DateModified;
      self.set_sort(field, default_ascending);
    }
  };
  draw_sort_field(SortField::Name);
  draw_sort_field(SortField::DateModified);
  draw_sort_field(SortField::Type);
  draw_sort_field(SortField::Size);

  ImGui::Separator();
  if (ImGui::MenuItem("Ascending", nullptr, self.sort_ascending_) && !self.sort_ascending_)
    self.set_sort(self.sort_field_, true);
  if (ImGui::MenuItem("Descending", nullptr, !self.sort_ascending_) && self.sort_ascending_)
    self.set_sort(self.sort_field_, false);

  ImGui::EndPopup();
}

auto ContentPanel::render_details_headers(this ContentPanel& self) -> void {
  memory::ScopedStack stack;

  ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
  const auto draw_header = [&self, &stack](const i32 column, const SortField field) -> void {
    ImGui::TableSetColumnIndex(column);

    const bool selected = self.sort_field_ == field;
    const auto label = selected ? stack.format_char(
                                    "{} {}###ContentSortHeader{}",
                                    sort_field_label(field),
                                    self.sort_ascending_ ? ICON_MDI_ARROW_UP : ICON_MDI_ARROW_DOWN,
                                    column
                                  )
                                : stack.format_char("{}###ContentSortHeader{}", sort_field_label(field), column);
    ImGui::TableHeader(label);
    if (ImGui::IsItemClicked()) {
      const bool ascending = selected ? !self.sort_ascending_ : field != SortField::DateModified;
      self.set_sort(field, ascending);
    }
  };

  draw_header(0, SortField::Name);
  draw_header(1, SortField::DateModified);
  draw_header(2, SortField::Type);
  draw_header(3, SortField::Size);
}

void ContentPanel::render_side_view(this ContentPanel& self) {
  ZoneScoped;

  constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_NoPadInnerX | ImGuiTableFlags_NoPadOuterX |
                                         ImGuiTableFlags_ContextMenuInBody | ImGuiTableFlags_ScrollY;

  constexpr ImGuiTreeNodeFlags tree_node_flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_FramePadding |
                                                 ImGuiTreeNodeFlags_SpanFullWidth;

  std::filesystem::path selected_directory;
  {
    auto read_lock = std::shared_lock(self.directory_mutex);
    selected_directory = self.current_directory;
  }

  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {0, 0});
  if (ImGui::BeginTable("SideViewTable", 1, tableFlags)) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();

    ImGuiTreeNodeFlags node_flags = tree_node_flags;
    const bool selected = selected_directory == self.assets_directory;
    if (selected) {
      node_flags |= ImGuiTreeNodeFlags_Selected;
    }

    ImVec4 active_color = ImGui::GetStyleColorVec4(ImGuiCol_Tab);
    ImVec4 hovered_color = ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered);
    ImGui::PushStyleColor(ImGuiCol_Header, active_color);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, selected ? active_color : hovered_color);

    const bool opened = ImGui::TreeNodeEx(self.assets_directory.string().c_str(), node_flags, "");
    ImGui::PopStyleColor(2);

    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
      self.update_directory_entries(self.assets_directory);
    const char* folder_icon = opened ? ICON_MDI_FOLDER_OPEN : ICON_MDI_FOLDER;
    ImGui::SameLine();
    if (selected)
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    ImGui::TextUnformatted(folder_icon);
    ImGui::SameLine();
    ImGui::TextUnformatted("Assets");
    if (selected)
      ImGui::PopStyleColor();

    if (opened) {
      self.directory_tree_view_recursive(self.assets_directory, selected_directory, tree_node_flags);
      ImGui::TreePop();
    }
    ImGui::EndTable();
    if (ImGui::IsItemClicked())
      App::mod<Editor>().get_context().reset();
  }

  ImGui::PopStyleVar();
}

void ContentPanel::render_body(this ContentPanel& self, bool grid) {
  auto& editor = App::mod<Editor>();
  const auto& editor_theme = editor.editor_theme;
  auto& editor_context = editor.get_context();

  std::filesystem::path directory_to_open;

  auto& editor_cvar = App::mod<Editor>().editor_cvar;

  const float padding = UI::scale(2.0f);
  const float scaled_thumbnail_size = UI::scale(editor_cvar.cvar_file_thumbnail_size.get());
  const float scaled_thumbnail_size_x = scaled_thumbnail_size * 0.55f;
  const float cell_size = scaled_thumbnail_size_x + 2 * padding + scaled_thumbnail_size_x * 0.1f;

  const float thumbnail_content_padding = padding * 2.0f;
  const float thumbnail_image_offset = padding + thumbnail_content_padding;
  const float thumb_image_size = scaled_thumbnail_size_x - thumbnail_content_padding * 2.0f;
  const float thumb_image_font_size = thumb_image_size / App::get_ui_scale();

  constexpr float thumbnail_name_font_size = 14.0f;
  const float thumbnail_name_height = UI::scale(thumbnail_name_font_size) * 2.0f;
  const float thumbnail_name_type_spacing = padding * 2.0f;
  const float type_color_frame_height = std::max(UI::scale(1.0f), scaled_thumbnail_size_x * 0.03f);
  const float type_color_frame_offset_y = thumbnail_image_offset + thumb_image_size + thumbnail_content_padding;
  const float thumbnail_name_offset_y = type_color_frame_offset_y + type_color_frame_height + padding;
  const float thumbnail_type_offset_y = thumbnail_name_offset_y + thumbnail_name_height + thumbnail_name_type_spacing;
  const float thumbnail_card_height = thumbnail_type_offset_y + UI::scale(editor_theme.small_font_size) +
                                      padding * 2.0f;
  const ImVec2 background_thumbnail_size = {scaled_thumbnail_size_x + padding * 2.0f, thumbnail_card_height};

  const float panel_width = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize;
  i32 column_count = static_cast<i32>(panel_width / cell_size);
  if (column_count < 1)
    column_count = 1;

  const float line_height = ImGui::GetTextLineHeight();
  ImGuiTableFlags flags = ImGuiTableFlags_ContextMenuInBody | ImGuiTableFlags_ScrollY;

  if (!grid) {
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, UI::scale(ImVec2(4.0f, 2.0f)));
    column_count = 4;
    flags |= ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_RowBg |
             ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
  } else {
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {scaled_thumbnail_size_x * 0.05f, scaled_thumbnail_size_x * 0.05f});
    flags |= ImGuiTableFlags_PadOuterX | ImGuiTableFlags_SizingFixedFit;
  }

  ImVec2 cursor_pos = ImGui::GetCursorPos();
  const ImVec2 region = ImGui::GetContentRegionAvail();
  ImGui::InvisibleButton("##DragDropTargetAssetPanelBody", region);

  ImGui::SetNextItemAllowOverlap();
  ImGui::SetCursorPos(cursor_pos);

  const char* table_id = grid ? "ContentGrid" : "ContentDetails";
  if (ImGui::BeginTable(table_id, column_count, flags)) {
    if (!grid) {
      ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.55f);
      ImGui::TableSetupColumn("Date modified", ImGuiTableColumnFlags_WidthStretch, 0.20f);
      ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 0.15f);
      ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch, 0.10f);
      self.render_details_headers();
    }

    bool any_item_hovered = false;

    {
      auto read_lock = std::shared_lock(self.directory_mutex);
      for (auto& file : self.directory_entries) {
        if (!self.filter_.PassFilter(file.name.c_str()))
          continue;

        if (!editor_cvar.cvar_show_meta_files.as_bool()) {
          if (file.type == FileType::Meta)
            continue;
        }

        const bool is_dir = file.is_directory;
        const char* filename = file.name.c_str();
        const auto file_path_str = file.file_path.string();
        const auto& path = file.file_path;
        ImGui::PushID(file_path_str.c_str());

        const bool highlight = editor_context.type == EditorContext::Type::File &&
                               file_path_str == editor_context.str.value_or(std::string{});

        if (grid) {
          ImGui::TableNextColumn();
          cursor_pos = ImGui::GetCursorPos();

          // Background button
          const bool clicked = UI::toggle_button("##ContentEntry", highlight, background_thumbnail_size, 0.1f);
          if (clicked) {
            editor_context.reset(EditorContext::Type::File, file_path_str);
          }
          ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, UI::scale(editor_theme.popup_item_spacing));
          if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Delete")) {
              self.directory_to_delete = path;
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
            self.white_texture.view(),
            {background_thumbnail_size.x - padding * 2.f, background_thumbnail_size.y - padding * 2.f},
            {},
            {},
            editor.editor_theme.window_bg_alternative_color
          );

          // Thumbnail Image
          ImGui::SetCursorPos({cursor_pos.x + thumbnail_image_offset, cursor_pos.y + thumbnail_image_offset});
          ImGui::SetNextItemAllowOverlap();

          auto use_thumbnail_image = !is_dir && editor_cvar.cvar_file_thumbnails.get() &&
                                     (file.type == FileType::Texture || file.type == FileType::Model ||
                                      file.type == FileType::Material || file.type == FileType::Terrain);
          auto thumbnail_image = TextureView{};
          if (use_thumbnail_image) {
            if (file.type == FileType::Texture) {
              thumbnail_image = editor.thumbnail_manager.get_thumbnail_texture(file_path_str);
            } else if (file.type == FileType::Model) {
              thumbnail_image = editor.thumbnail_manager.get_thumbnail_model(file_path_str);
            } else if (file.type == FileType::Material) {
              thumbnail_image = editor.thumbnail_manager.get_thumbnail_material(file_path_str);
            } else if (file.type == FileType::Terrain) {
              thumbnail_image = editor.thumbnail_manager.get_thumbnail_terrain(file_path_str);
            }

            // Otherwise the spinner below waits on a thumbnail that will never arrive.
            if (!thumbnail_image && editor.thumbnail_manager.thumbnail_unavailable(file_path_str)) {
              use_thumbnail_image = false;
            }
          }
          if (use_thumbnail_image) {
            if (thumbnail_image) {
              UI::image(thumbnail_image, {thumb_image_size, thumb_image_size});
            } else {
              ImSpinner::detail::SpinnerConfig config{};
              config.setSpinnerType(ImSpinner::e_st_ang);
              config.setSpeed(6.f);
              config.setAngle(4.f);
              config.setThickness(UI::scale(2.0f));
              config.setRadius(thumb_image_size / 2.f);
              config.setColor(ImColor(1.f, 1.f, 1.f, 1.f));
              ImGui::PushFont(nullptr, thumb_image_font_size);
              ImSpinner::Spinner("SpinnerAng270NoBg", config);
              ImGui::PopFont();
            }
          } else {
            ImGui::PushFont(nullptr, thumb_image_font_size);
            ImGui::TextUnformatted(file.icon.c_str());
            ImGui::PopFont();
          }

          // Type color frame
          const ImVec2 type_color_frame_size = {scaled_thumbnail_size_x, type_color_frame_height};
          ImGui::SetCursorPos({cursor_pos.x + padding, cursor_pos.y + type_color_frame_offset_y});
          UI::image(
            self.white_texture.view(),
            type_color_frame_size,
            {0, 0},
            {1, 1},
            is_dir ? ImVec4(0.0f, 0.0f, 0.0f, 0.0f) : file.file_type_indicator_color
          );

          const ImVec2 rect_min = ImGui::GetItemRectMin();
          const ImVec2 rect_size = ImGui::GetItemRectSize();
          const ImRect clip_rect = ImRect(
            {rect_min.x + padding, rect_min.y + type_color_frame_height + padding},
            {rect_min.x + rect_size.x - padding, rect_min.y + type_color_frame_height + padding + thumbnail_name_height}
          );
          ImGui::PushFont(nullptr, thumbnail_name_font_size);
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
            ImGui::SetCursorPos({cursor_pos.x + padding * 2.0f, cursor_pos.y + thumbnail_type_offset_y});
            ImGui::BeginDisabled();
            ImGui::PushFont(nullptr, editor_theme.small_font_size);
            ImGui::TextUnformatted(file.file_type_string.data());
            ImGui::PopFont();
            ImGui::EndDisabled();
          }
        } else {
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);

          memory::ScopedStack row_stack;
          const auto entry_label = row_stack.format_char("{}  {}###ContentEntry", file.icon, filename);
          constexpr ImGuiSelectableFlags selectable_flags = ImGuiSelectableFlags_SpanAllColumns |
                                                            ImGuiSelectableFlags_AllowOverlap |
                                                            ImGuiSelectableFlags_AllowDoubleClick;
          const bool clicked = ImGui::Selectable(
            entry_label,
            highlight,
            selectable_flags,
            ImVec2(0.0f, line_height + ImGui::GetStyle().FramePadding.y * 2.0f)
          );
          const bool hovered = ImGui::IsItemHovered();
          if (clicked)
            editor_context.reset(EditorContext::Type::File, file_path_str);

          ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, UI::scale(editor_theme.popup_item_spacing));
          if (ImGui::BeginPopupContextItem("ContentEntryContext")) {
            if (ImGui::MenuItem("Delete")) {
              self.directory_to_delete = path;
              ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Rename"))
              ImGui::CloseCurrentPopup();

            ImGui::Separator();
            if (auto p = self.draw_context_menu_items(path, is_dir); !p.empty())
              directory_to_open = p;
            ImGui::EndPopup();
          }
          ImGui::PopStyleVar();

          if (is_dir)
            drag_drop_target(path);
          drag_drop_from(path);

          any_item_hovered |= hovered;
          if (hovered && ImGui::IsMouseDoubleClicked(0)) {
            if (is_dir) {
              directory_to_open = path;
              self.filter_.Clear();
            } else {
              open_file(path);
              editor_context.reset();
            }
          }

          ImGui::TableSetColumnIndex(1);
          ImGui::TextUnformatted(file.modified_time_string.c_str());
          ImGui::TableSetColumnIndex(2);
          ImGui::TextUnformatted(file.file_type_string.data());
          ImGui::TableSetColumnIndex(3);
          ImGui::TextUnformatted(file.size_string.c_str());
        }

        ImGui::PopID();
      }
    }

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, UI::scale(editor_theme.popup_item_spacing));
    if (
      ImGui::BeginPopupContextWindow(
        "AssetPanelHierarchyContextWindow",
        ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems
      )
    ) {
      editor_context.reset();
      if (auto p = self.draw_context_menu_items(self.current_directory, true); !p.empty()) {
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

  if (!self.directory_to_delete.empty()) {
    if (!ImGui::IsPopupOpen("Delete?"))
      ImGui::OpenPopup("Delete?");
  }

  if (ImGui::BeginPopupModal("Delete?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text(
      "%s will be deleted. \nAre you sure? This operation cannot be undone!\n\n",
      self.directory_to_delete.string().c_str()
    );
    ImGui::Separator();
    if (ImGui::Button("OK", UI::scale(ImVec2(120.0f, 0.0f)))) {
      std::filesystem::remove_all(self.directory_to_delete);
      self.directory_to_delete.clear();
      self.refresh();
      ImGui::CloseCurrentPopup();
    }
    ImGui::SetItemDefaultFocus();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", UI::scale(ImVec2(120.0f, 0.0f)))) {
      ImGui::CloseCurrentPopup();
      self.directory_to_delete.clear();
    }
    editor_context.reset();
    ImGui::EndPopup();
  }

  if (self.should_open_new_asset_popup)
    ImGui::OpenPopup("New Asset");

  if (ImGui::BeginPopupModal("New Asset", nullptr, ImGuiWindowFlags_NoResize)) {
    UI::begin_properties();
    UI::input_text("Name", &self.new_asset_name);
    UI::end_properties();

    if (ImGui::Button("Create", UI::scale(ImVec2(120.0f, 0.0f)))) {
      if (!self.new_asset_name.empty()) {
        auto& asset_man = App::mod<AssetManager>();
        auto asset_path = self.current_directory / self.new_asset_name;
        if (asset_path.extension() == ".oxasset") {
          asset_path.replace_extension("");
        }

        if (self.new_asset_type == AssetType::ParticleSystem && asset_path.extension() != ".oxparticle") {
          asset_path.replace_extension(".oxparticle");
        }

        auto asset = asset_man.create_asset(self.new_asset_type, asset_path);
        asset_man.load_asset(asset);
        if (asset_man.export_asset(asset, asset_path)) {
          OX_LOG_INFO(
            "Created new {} asset {}",
            AssetManager::to_asset_type_sv(self.new_asset_type),
            self.new_asset_name
          );
          self.refresh();
        } else {
          OX_LOG_ERROR(
            "Couldn't create {} asset {}",
            AssetManager::to_asset_type_sv(self.new_asset_type),
            self.new_asset_name
          );
        }
        self.new_asset_name.clear();
        self.should_open_new_asset_popup = false;
        ImGui::CloseCurrentPopup();
      }
    }

    ImGui::SameLine();
    if (ImGui::Button("Cancel", UI::scale(ImVec2(120.0f, 0.0f)))) {
      self.new_asset_name.clear();
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

  std::unique_lock lock(self.directory_mutex);
  self.current_directory = directory;
  self.directory_entries.clear();

  if (directory.empty())
    return;

  std::error_code iterator_error;
  auto directory_it = std::filesystem::directory_iterator(
    directory,
    std::filesystem::directory_options::skip_permission_denied,
    iterator_error
  );
  const auto directory_end = std::filesystem::directory_iterator();
  while (!iterator_error && directory_it != directory_end) {
    const auto& directory_entry = *directory_it;
    const auto path = directory_entry.path();
    auto file_name_str = path.filename().string();

    if (file_name_str.starts_with('.')) {
      directory_it.increment(iterator_error);
      continue;
    }

    std::error_code type_error;
    const bool is_directory = directory_entry.is_directory(type_error);
    auto file_type = is_directory ? FileType::Directory : FileType::Unknown;
    if (!is_directory) {
      const auto extension = path.extension().string();
      const auto& file_type_it = FILE_TYPES.find(extension);
      if (file_type_it != FILE_TYPES.end())
        file_type = file_type_it->second;
    }

    if (file_type == FileType::Meta) {
      if (auto standalone_type = standalone_asset_file_type(path)) {
        file_type = standalone_type.value();
      }
    }

    std::string_view file_type_string = FILE_TYPES_TO_STRING.at(FileType::Unknown);
    const auto& file_string_type_it = FILE_TYPES_TO_STRING.find(file_type);
    if (file_string_type_it != FILE_TYPES_TO_STRING.end())
      file_type_string = file_string_type_it->second;

    ImVec4 file_type_color = {1.0f, 1.0f, 1.0f, 1.0f};
    const auto& file_type_color_it = TYPE_COLORS.find(file_type);
    if (file_type_color_it != TYPE_COLORS.end())
      file_type_color = file_type_color_it->second;

    std::error_code modified_time_error;
    const auto modified_time = directory_entry.last_write_time(modified_time_error);
    const bool has_modified_time = !modified_time_error;

    u64 size_bytes = 0;
    bool has_size = false;
    if (!is_directory) {
      std::error_code size_error;
      const auto file_size = directory_entry.file_size(size_error);
      if (!size_error) {
        size_bytes = static_cast<u64>(file_size);
        has_size = true;
      }
    }

    const auto file_icon = is_directory ? ICON_MDI_FOLDER : FILE_TYPES_TO_ICON.at(file_type);
    File entry = {
      .name = std::move(file_name_str),
      .file_path = path,
      .directory_entry = directory_entry,
      .thumbnail = nullptr,
      .icon = file_icon,
      .is_directory = is_directory,
      .type = file_type,
      .file_type_string = file_type_string,
      .file_type_indicator_color = file_type_color,
      .size_bytes = size_bytes,
      .modified_time = modified_time,
      .has_size = has_size,
      .has_modified_time = has_modified_time,
      .size_string = is_directory ? ""
                     : has_size   ? format_file_size(size_bytes)
                                  : "—",
      .modified_time_string = has_modified_time ? format_modified_time(modified_time) : "—",
    };

    self.directory_entries.emplace_back(std::move(entry));
    directory_it.increment(iterator_error);
  }

  sort_entries(std::span(self.directory_entries), self.sort_field_, self.sort_ascending_);
  self.elapsed_time_ = 0.0f;
}

void ContentPanel::refresh(this ContentPanel& self) {
  ZoneScoped;

  std::filesystem::path current_directory;
  {
    auto read_lock = std::shared_lock(self.directory_mutex);
    current_directory = self.current_directory;
  }
  self.update_directory_entries(current_directory);
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
        self.new_asset_name.clear();
        self.new_asset_type = AssetType::Material;
        self.should_open_new_asset_popup = true;
      }
      if (ImGui::MenuItem("Particle System")) {
        self.new_asset_name.clear();
        self.new_asset_type = AssetType::ParticleSystem;
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
      self.refresh_requested = true;
    }
  }

  return dir_to_open;
}
} // namespace ox
