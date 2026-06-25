#pragma once

#include "Asset/AssetManager.hpp"
#include "Core/UUID.hpp"
#include "EditorPanelState.hpp"
#include "UI/AssetManagerViewer.hpp"

namespace ox {
struct Material;
class Scene;
class InspectorPanel : public EditorPanelState {
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
  option<glm::vec3> euler_cache = nullopt;
  flecs::entity last_edited_entity = flecs::entity::null();

  InspectorPanel();

  auto on_update(this InspectorPanel& self) -> void {}
  auto on_render(this InspectorPanel& self, vuk::ImageAttachment swapchain_attachment) -> void;

  static auto draw_material_properties(
    ReadGuard<Material> material, const UUID& material_uuid, const std::filesystem::path& default_path
  ) -> bool;

  auto draw_components(this InspectorPanel& self, flecs::entity entity) -> void;
  auto draw_asset_info(this InspectorPanel& self, ReadGuard<Asset> asset) -> void;

  auto draw_model_asset(this InspectorPanel& self, ReadGuard<Asset> asset, ReadGuard<Model> model) -> void;
  auto draw_material_asset(this InspectorPanel& self, ReadGuard<Asset> asset, ReadGuard<Material> material) -> bool;
  auto draw_audio_asset(this InspectorPanel& self, ReadGuard<Asset> asset, ReadGuard<AudioSource> audio) -> void;
  auto draw_script_asset(this InspectorPanel& self, ReadGuard<Asset> asset, ReadGuard<LuaSystem> lua_system) -> bool;

private:
  struct ComponentClipboard {
    flecs::entity source_entity;
    flecs::id_t component_id = 0;

    auto is_valid() const -> bool { return source_entity.is_alive() && component_id != 0; }
  };

  ComponentClipboard component_clipboard = {};

  auto draw_component_context_menu(bool& remove_component, flecs::entity entity, flecs::id id) -> void;

  Scene* scene_;
  bool rename_entity_ = false;
};
} // namespace ox
