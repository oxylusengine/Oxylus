#pragma once

#include "Asset/AssetManager.hpp"
#include "Panels/EditorPanel.hpp"

namespace ox {
class AssetManagerPanel : public EditorPanel {
public:
  AssetManagerPanel();

  ~AssetManagerPanel() override = default;

  AssetManagerPanel(const AssetManagerPanel& other) = delete;
  AssetManagerPanel(AssetManagerPanel&& other) = delete;
  AssetManagerPanel& operator=(const AssetManagerPanel& other) = delete;
  AssetManagerPanel& operator=(AssetManagerPanel&& other) = delete;

  void on_update() override;
  void on_render(vuk::Extent3D extent, vuk::Format format) override;

private:
  std::vector<Asset> model_assets = {};
  std::vector<Asset> texture_assets = {};
  std::vector<Asset> material_assets = {};
  std::vector<Asset> scene_assets = {};
  std::vector<Asset> audio_assets = {};
  std::vector<Asset> script_assets = {};
  std::vector<Asset> shader_assets = {};
  std::vector<Asset> font_assets = {};

  void clear_vectors(this AssetManagerPanel& self);
};
} // namespace ox
