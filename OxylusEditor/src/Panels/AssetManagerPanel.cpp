#include "AssetManagerPanel.hpp"

#include <Tracy.hpp>
#include <icons/IconsMaterialDesignIcons.h>
#include <imgui.h>

#include "Core/App.hpp"
#include "EditorUI.hpp"

namespace ox {
static void draw_asset_table_columns(const Asset& asset) {
  ZoneScoped;

  auto* asset_man = App::get_asset_manager();

  const auto uuid_str = asset.uuid.str();

  {
    ImGui::TableSetColumnIndex(0);
    ImGui::PushID(uuid_str.c_str());
    ImGui::SmallButton("..");
    if (ImGui::BeginPopupContextItem(0, ImGuiPopupFlags_MouseButtonLeft)) {
      if (ImGui::Button("Load")) {
        asset_man->load_asset(asset.uuid);
      }

      if (ImGui::Button("Unload")) {
        if (asset.ref_count < 1)
          asset_man->unload_asset(asset.uuid);
        else
          OX_LOG_ERROR("Can't unload asset with {} references!", asset.ref_count);
      }
      ImGui::EndPopup();
    }
    ImGui::PopID();
  }

  {
    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(uuid_str.c_str());
  }

  {
    ImGui::TableSetColumnIndex(2);
    ImGui::TextUnformatted(asset.path.c_str());
  }

  {
    ImGui::TableSetColumnIndex(3);
    if (!asset.is_loaded())
      ImGui::Text("Invalid ID");
    else
      ImGui::Text("%lu", static_cast<u64>(asset.texture_id));
  }

  {
    ImGui::TableSetColumnIndex(4);
    ImGui::Text("%lu", static_cast<u64>(asset.ref_count));
  }
}

static void draw_asset_table(const char* tree_name,
                             const char* table_name,
                             const std::vector<Asset>& assets,
                             ImGuiTreeNodeFlags tree_flags,
                             i32 table_columns_count,
                             ImGuiTableFlags table_flags) {
  ZoneScoped;

  if (ImGui::TreeNodeEx(tree_name, tree_flags, "%s", tree_name)) {
    if (ImGui::BeginTable(table_name, table_columns_count, table_flags)) {
      ImGui::TableSetupColumn(" .. ", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize(" .. ").x);
      ImGui::TableSetupColumn("UUID");
      ImGui::TableSetupColumn("Path");
      ImGui::TableSetupColumn("ID");
      ImGui::TableSetupColumn("Ref Count");

      ImGui::TableHeadersRow();

      for (const auto& asset : assets) {
        ImGui::TableNextRow();
        draw_asset_table_columns(asset);
      }

      ImGui::EndTable();
    }

    ImGui::TreePop();
  }
}

AssetManagerPanel::AssetManagerPanel() : EditorPanel("Asset Manager", ICON_MDI_FOLDER_SYNC, false) {}

void AssetManagerPanel::on_update() {
  ZoneScoped;

  auto* asset_manager = App::get_asset_manager();

  const auto& registry = asset_manager->registry();

  for (const auto& [uuid, asset] : registry) {
    if (uuid) {
      switch (asset.type) {
        case AssetType::None  : break;
        case AssetType::Shader: {
          shader_assets.emplace_back(asset);
          break;
        }
        case AssetType::Model: {
          model_assets.emplace_back(asset);
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
}

void AssetManagerPanel::on_render(vuk::Extent3D extent, vuk::Format format) {
  ZoneScoped;

  on_begin();

  i32 open_action = -1;

  if (UI::button("Expand All"))
    open_action = 1;
  ImGui::SameLine();
  if (UI::button("Collapse All"))
    open_action = 0;

  constexpr ImGuiTreeNodeFlags TREE_FLAGS = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowItemOverlap |
                                            ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_FramePadding;
  constexpr i32 TABLE_COLUMNS_COUNT = 5;
  constexpr ImGuiTableFlags TABLE_FLAGS = ImGuiTableFlags_Resizable | ImGuiTableFlags_Hideable |
                                          ImGuiTableFlags_Borders | ImGuiTableFlags_ContextMenuInBody |
                                          ImGuiTableFlags_SizingStretchProp;

  UI::help_marker("\"Invalid ID\" means asset is not loaded yet or has been unloaded.");

  if (open_action != -1)
    ImGui::SetNextItemOpen(open_action != 0);
  draw_asset_table("Texture Assets", "textures_table", texture_assets, TREE_FLAGS, TABLE_COLUMNS_COUNT, TABLE_FLAGS);

  if (open_action != -1)
    ImGui::SetNextItemOpen(open_action != 0);
  draw_asset_table("Model Assets", "models_table", model_assets, TREE_FLAGS, TABLE_COLUMNS_COUNT, TABLE_FLAGS);

  if (open_action != -1)
    ImGui::SetNextItemOpen(open_action != 0);
  draw_asset_table("Material Assets", "materials_table", material_assets, TREE_FLAGS, TABLE_COLUMNS_COUNT, TABLE_FLAGS);

  if (open_action != -1)
    ImGui::SetNextItemOpen(open_action != 0);
  draw_asset_table("Scene Assets", "scenes_table", scene_assets, TREE_FLAGS, TABLE_COLUMNS_COUNT, TABLE_FLAGS);

  if (open_action != -1)
    ImGui::SetNextItemOpen(open_action != 0);
  draw_asset_table("Audio Assets", "audio_table", audio_assets, TREE_FLAGS, TABLE_COLUMNS_COUNT, TABLE_FLAGS);

  if (open_action != -1)
    ImGui::SetNextItemOpen(open_action != 0);
  draw_asset_table("Script Assets", "script_table", script_assets, TREE_FLAGS, TABLE_COLUMNS_COUNT, TABLE_FLAGS);

  if (open_action != -1)
    ImGui::SetNextItemOpen(open_action != 0);
  draw_asset_table("Shader Assets", "shader_table", shader_assets, TREE_FLAGS, TABLE_COLUMNS_COUNT, TABLE_FLAGS);

  if (open_action != -1)
    ImGui::SetNextItemOpen(open_action != 0);
  draw_asset_table("Font Assets", "font_table", font_assets, TREE_FLAGS, TABLE_COLUMNS_COUNT, TABLE_FLAGS);

  on_end();

  clear_vectors();
}

void AssetManagerPanel::clear_vectors(this AssetManagerPanel& self) {
  ZoneScoped;

  self.model_assets.clear();
  self.texture_assets.clear();
  self.material_assets.clear();
  self.scene_assets.clear();
  self.audio_assets.clear();
  self.script_assets.clear();
  self.shader_assets.clear();
  self.font_assets.clear();
}
} // namespace ox
