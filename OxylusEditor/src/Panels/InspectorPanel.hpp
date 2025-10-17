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
    std::string path = {};
  };

  struct DialogSaveEvent {
    std::string path = {};
  };

  AssetManagerViewer viewer = {};

  InspectorPanel();

  void on_render(vuk::Extent3D extent, vuk::Format format) override;

  static void draw_material_properties(Material* material, const UUID& material_uuid, std::string_view default_path);

private:
  Scene* _scene;
  bool _rename_entity = false;

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
