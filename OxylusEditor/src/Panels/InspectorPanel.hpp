#pragma once

#include "Asset/AssetManager.hpp"
#include "Core/UUID.hpp"
#include "EditorPanel.hpp"
#include "UI/AssetManagerViewer.hpp"

namespace ox {
struct Material;
class Scene;
class InspectorPanel : public EditorPanel {
public:
  struct DialogLoadEvent {
    UUID* asset_uuid = {};
    std::filesystem::path path = {};
  };

  struct DialogSaveEvent {
    UUID asset_uuid = {};
    std::filesystem::path path = {};
  };

  AssetManagerViewer viewer = {};

  InspectorPanel();

  void on_render(vuk::Extent3D extent, vuk::Format format) override;

  static void draw_material_properties(
    Material* material, const UUID& material_uuid, const std::filesystem::path& default_path
  );

private:
  Scene* scene_;
  bool rename_entity_ = false;

  void draw_components(flecs::entity entity);
  void draw_asset_info(Asset* asset);

  void draw_shader_asset(UUID* uuid, Asset* asset);
  void draw_model_asset(UUID* uuid, Asset* asset);
  void draw_texture_asset(UUID* uuid, Asset* asset);
  void draw_material_asset(UUID* uuid, Asset* asset);
  void draw_font_asset(UUID* uuid, Asset* asset);
  void draw_scene_asset(UUID* uuid, Asset* asset);
  void draw_audio_asset(UUID* uuid, Asset* asset);
  bool draw_script_asset(UUID* uuid, Asset* asset);
};
} // namespace ox
