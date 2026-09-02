#pragma once

#include "Asset/AssetManager.hpp"
#include "AssetManagerPanel.hpp"
#include "Core/UUID.hpp"
#include "EditorPanelState.hpp"

namespace ox {
struct Material;
class Scene;
class InspectorPanel : public EditorPanelState {
public:
  struct DialogSaveEvent {
    UUID asset_uuid = {};
    std::filesystem::path path = {};
  };

  AssetBrowser asset_browser = {};
  option<glm::vec3> euler_cache = nullopt;
  flecs::entity last_edited_entity = flecs::entity::null();

  InspectorPanel();

  auto on_update(this InspectorPanel& self) -> void {}
  auto on_render(this InspectorPanel& self, vuk::ImageAttachment swapchain_attachment) -> void;

  auto handle_editor_context(this InspectorPanel& self) -> void;

  static auto draw_material_properties(
    ReadGuard<Material> material, const UUID& material_uuid, const std::filesystem::path& default_path
  ) -> bool;

  auto draw_components(this InspectorPanel& self, flecs::entity entity) -> void;
  auto draw_asset_info(this InspectorPanel& self, ReadGuard<Asset> asset) -> void;

  // One component field holding an asset UUID: preview, name, picker, clear, and drop target.
  // Returns true when it pointed the field somewhere else.
  auto draw_asset_field(this InspectorPanel& self, std::string_view label, UUID& uuid) -> bool;

  // Type specific editor for whatever `uuid` names, shown under an asset field and under a file
  // selected in the content browser.
  auto draw_asset_contents(this InspectorPanel& self, const UUID& uuid) -> void;

  auto draw_audio_asset(this InspectorPanel& self, const std::filesystem::path& path, ReadGuard<AudioSource> audio)
    -> void;
  auto draw_script_asset(this InspectorPanel& self, ReadGuard<LuaScript> script) -> void;

private:
  struct ComponentClipboard {
    flecs::entity source_entity;
    flecs::id_t component_id = 0;

    auto is_valid() const -> bool { return source_entity.is_alive() && component_id != 0; }
  };

  ComponentClipboard component_clipboard = {};

  // Which field's picker is up. One browser is shared by every field, so only one can be open.
  const void* asset_picker_field = nullptr;

  // Resolving a selected file to its asset reads (and the first time writes) a meta file, so the
  // answer is kept until the selection moves off it rather than redone every frame.
  std::filesystem::path resolved_asset_path = {};
  UUID resolved_asset_uuid = UUID(nullptr);

  auto draw_component_context_menu(bool& remove_component, flecs::entity entity, flecs::id id) -> void;

  Scene* scene_;
  bool rename_entity_ = false;
};
} // namespace ox
