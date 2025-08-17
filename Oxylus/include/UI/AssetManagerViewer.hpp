#pragma once

#include <imgui.h>

#include "Asset/AssetManager.hpp"

namespace ox {
class AssetManagerViewer {
public:
  AssetManagerViewer() = default;

  const char* search_icon = "";
  const char* filter_icon = "Filter";

  auto render(const char* id, bool* visible, AssetType default_filter = AssetType::None, Asset* selected = nullptr)
      -> void;

private:
  std::vector<Asset> mesh_assets = {};
  std::vector<Asset> texture_assets = {};
  std::vector<Asset> material_assets = {};
  std::vector<Asset> scene_assets = {};
  std::vector<Asset> audio_assets = {};
  std::vector<Asset> script_assets = {};
  std::vector<Asset> shader_assets = {};
  std::vector<Asset> font_assets = {};

  ankerl::unordered_dense::map<AssetType, bool> asset_type_filter_flags = {
      {AssetType::Shader, true},
      {AssetType::Texture, true},
      {AssetType::Material, true},
      {AssetType::Font, true},
      {AssetType::Scene, true},
      {AssetType::Audio, true},
      {AssetType::Script, true},
  };

  ImGuiTextFilter text_filter = {};

  auto clear_vectors(this AssetManagerViewer& self) -> void;

  auto draw_asset_table(const char* tree_name,
                        const char* table_name,
                        const std::vector<Asset>& assets,
                        ImGuiTreeNodeFlags tree_flags,
                        i32 table_columns_count,
                        ImGuiTableFlags table_flags,
                        Asset* selected) -> void;
};
} // namespace ox
