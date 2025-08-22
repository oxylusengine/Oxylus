#include "UI/AssetManagerViewer.hpp"

#include <imgui.h>

#include "Core/App.hpp"
#include "Core/FileSystem.hpp"
#include "UI/UI.hpp"

namespace ox {
auto draw_asset_table_columns(const Asset& asset) -> bool {
  ZoneScoped;

  bool is_selected = false;

  auto* asset_man = App::get_asset_manager();

  const auto uuid_str = asset.uuid.str();

  {
    ImGui::TableSetColumnIndex(0);
    ImGui::PushID(uuid_str.c_str());

    constexpr ImGuiSelectableFlags selectable_flags = ImGuiSelectableFlags_SpanAllColumns |
                                                      ImGuiSelectableFlags_AllowOverlap |
                                                      ImGuiSelectableFlags_AllowDoubleClick;

    auto name = fs::get_name_with_extension(asset.path);

    if (ImGui::Selectable(name.c_str(), false, selectable_flags, ImVec2(0.f, 20.f))) {
      is_selected = true;
    }
    ImGui::PopID();
  }

  if (ImGui::BeginPopupContextItem(uuid_str.c_str(), ImGuiPopupFlags_MouseButtonRight)) {
    if (ImGui::Button("Load")) {
      asset_man->load_asset(asset.uuid);
    }

    ImGui::SameLine();

    if (ImGui::Button("Unload")) {
      if (asset.ref_count < 1)
        asset_man->unload_asset(asset.uuid);
      else
        OX_LOG_ERROR("Can't unload asset with {} references!", asset.ref_count);
    }

    if (!asset.is_loaded())
      ImGui::Text("ID: Invalid ID");
    else
      ImGui::Text("ID: %llu", static_cast<u64>(asset.texture_id));

    ImGui::Text("RefCount: %llu", static_cast<u64>(asset.ref_count));

    ImGui::EndPopup();
  }

  {
    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(uuid_str.c_str());
  }

  return is_selected;
}

auto AssetManagerViewer::draw_asset_table(const char* tree_name,
                                          const char* table_name,
                                          const std::vector<Asset>& assets,
                                          ImGuiTreeNodeFlags tree_flags,
                                          i32 table_columns_count,
                                          ImGuiTableFlags table_flags,
                                          Asset* selected) -> void {
  ZoneScoped;

  if (ImGui::TreeNodeEx(tree_name, tree_flags, "%s", tree_name)) {
    if (ImGui::BeginTable(table_name, table_columns_count, table_flags)) {
      ImGui::TableSetupColumn("Name");
      ImGui::TableSetupColumn("UUID");

      for (const auto& asset : assets) {
        auto name = fs::get_name_with_extension(asset.path);
        if (!text_filter.PassFilter(name.c_str())) {
          continue;
        }

        ImGui::TableNextRow();
        if (draw_asset_table_columns(asset)) {
          if (selected)
            *selected = asset;
        }
      }

      ImGui::EndTable();
    }

    ImGui::TreePop();
  }
}

auto AssetManagerViewer::render(const char* id, bool* visible, AssetType default_filter, Asset* selected) -> void {
  ZoneScoped;

  auto* asset_manager = App::get_asset_manager();

  const auto& registry = asset_manager->registry();

  for (const auto& [uuid, asset] : registry) {
    if (uuid) {
      if (default_filter != AssetType::None && asset.type != default_filter) {
        continue;
      }
      switch (asset.type) {
        case AssetType::None  : break;
        case AssetType::Shader: {
          shader_assets.emplace_back(asset);
          break;
        }
        case AssetType::Mesh: {
          mesh_assets.emplace_back(asset);
          break;
        }
        case AssetType::Texture: {
          texture_assets.emplace_back(asset);
          break;
        }
        case AssetType::Material: {
          material_assets.emplace_back(asset);
          break;
        }
        case AssetType::Font: {
          font_assets.emplace_back(asset);
          break;
        }
        case AssetType::Scene: {
          scene_assets.emplace_back(asset);
          break;
        }
        case AssetType::Audio: {
          audio_assets.emplace_back(asset);
          break;
        }
        case AssetType::Script: {
          script_assets.emplace_back(asset);
          break;
        }
      }
    }
  }

  ImGui::SetNextWindowSize(ImVec2(ImGui::GetMainViewport()->Size.x / 2, ImGui::GetMainViewport()->Size.y / 2),
                           ImGuiCond_Appearing);
  const auto center = ImGui::GetMainViewport()->GetCenter();
  ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (ImGui::Begin(id, visible, ImGuiWindowFlags_NoCollapse)) {
    constexpr ImGuiTreeNodeFlags TREE_FLAGS = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowItemOverlap |
                                              ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_FramePadding |
                                              ImGuiTreeNodeFlags_DefaultOpen;
    constexpr i32 TABLE_COLUMNS_COUNT = 5;
    constexpr ImGuiTableFlags TABLE_FLAGS = ImGuiTableFlags_Resizable | ImGuiTableFlags_Hideable |
                                            ImGuiTableFlags_Borders | ImGuiTableFlags_ContextMenuInBody |
                                            ImGuiTableFlags_SizingStretchProp;

    if (ImGui::Button(filter_icon)) {
      ImGui::OpenPopup("asset_picker_filter");
    }
    if (ImGui::BeginPopup("asset_picker_filter")) {
      if (ImGui::Button("Select All")) {
        for (auto&& [type, flag] : asset_type_filter_flags) {
          flag = true;
        }
      }

      ImGui::SameLine();

      if (ImGui::Button("Deselect All")) {
        for (auto&& [type, flag] : asset_type_filter_flags) {
          flag = false;
        }
      }

      UI::begin_properties();

      for (auto&& [type, flag] : asset_type_filter_flags) {
        UI::property(AssetManager::to_asset_type_sv(type).data(), &flag);
      }

      UI::end_properties();
      ImGui::EndPopup();
    }

    // only enable the default filter if set
    if (default_filter != AssetType::None) {
      for (auto&& [type, flag] : asset_type_filter_flags) {
        flag = false;
      }

      asset_type_filter_flags[default_filter] = true;
    }

    ImGui::SameLine();

    const float filter_cursor_pos_x = ImGui::GetCursorPosX();
    text_filter.Draw("##asset_manager_viewer_filter", ImGui::GetContentRegionAvail().x);
    if (!text_filter.IsActive()) {
      ImGui::SameLine();
      ImGui::SetCursorPosX(filter_cursor_pos_x + ImGui::GetFontSize() * 0.5f);
      auto search_txt = fmt::format(" {} Search...", search_icon);
      ImGui::TextUnformatted(search_txt.c_str());
    }

    i32 open_action = -1;

    if (UI::button("Expand All"))
      open_action = 1;
    ImGui::SameLine();
    if (UI::button("Collapse All"))
      open_action = 0;

    if (asset_type_filter_flags[AssetType::Texture]) {
      if (open_action != -1)
        ImGui::SetNextItemOpen(open_action != 0);
      draw_asset_table(
          "Texture Assets", "textures_table", texture_assets, TREE_FLAGS, TABLE_COLUMNS_COUNT, TABLE_FLAGS, selected);
    }

    if (asset_type_filter_flags[AssetType::Mesh]) {
      if (open_action != -1)
        ImGui::SetNextItemOpen(open_action != 0);
      draw_asset_table(
          "Mesh Assets", "meshes_table", mesh_assets, TREE_FLAGS, TABLE_COLUMNS_COUNT, TABLE_FLAGS, selected);
    }

    if (asset_type_filter_flags[AssetType::Material]) {
      if (open_action != -1)
        ImGui::SetNextItemOpen(open_action != 0);
      draw_asset_table("Material Assets",
                       "materials_table",
                       material_assets,
                       TREE_FLAGS,
                       TABLE_COLUMNS_COUNT,
                       TABLE_FLAGS,
                       selected);
    }

    if (asset_type_filter_flags[AssetType::Scene]) {
      if (open_action != -1)
        ImGui::SetNextItemOpen(open_action != 0);
      draw_asset_table(
          "Scene Assets", "scenes_table", scene_assets, TREE_FLAGS, TABLE_COLUMNS_COUNT, TABLE_FLAGS, selected);
    }

    if (asset_type_filter_flags[AssetType::Audio]) {
      if (open_action != -1)
        ImGui::SetNextItemOpen(open_action != 0);
      draw_asset_table(
          "Audio Assets", "audio_table", audio_assets, TREE_FLAGS, TABLE_COLUMNS_COUNT, TABLE_FLAGS, selected);
    }

    if (asset_type_filter_flags[AssetType::Script]) {
      if (open_action != -1)
        ImGui::SetNextItemOpen(open_action != 0);
      draw_asset_table(
          "Script Assets", "script_table", script_assets, TREE_FLAGS, TABLE_COLUMNS_COUNT, TABLE_FLAGS, selected);
    }

    if (asset_type_filter_flags[AssetType::Shader]) {
      if (open_action != -1)
        ImGui::SetNextItemOpen(open_action != 0);
      draw_asset_table(
          "Shader Assets", "shader_table", shader_assets, TREE_FLAGS, TABLE_COLUMNS_COUNT, TABLE_FLAGS, selected);
    }

    if (asset_type_filter_flags[AssetType::Font]) {
      if (open_action != -1)
        ImGui::SetNextItemOpen(open_action != 0);
      draw_asset_table(
          "Font Assets", "font_table", font_assets, TREE_FLAGS, TABLE_COLUMNS_COUNT, TABLE_FLAGS, selected);
    }
  }
  ImGui::End();

  clear_vectors();
}

auto AssetManagerViewer::clear_vectors(this AssetManagerViewer& self) -> void {
  ZoneScoped;
  self.mesh_assets.clear();
  self.texture_assets.clear();
  self.material_assets.clear();
  self.scene_assets.clear();
  self.audio_assets.clear();
  self.script_assets.clear();
  self.shader_assets.clear();
  self.font_assets.clear();
}
} // namespace ox
