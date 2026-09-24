#pragma once

#include <imgui.h>

#include "Asset/AssetManager.hpp"
#include "Panels/EditorPanelState.hpp"

namespace ox {
auto asset_type_icon(AssetType type) -> const c8*;

// What an asset is called and where it came from, for every list, field and tooltip that shows one.
// `Asset::path` is what the loader reads, which for anything the importer cooks is the `<uuid>.oxpack`
// in the cache -- a name nobody recognizes -- so these resolve back to the source the import
// recorded and fall back to the registry path for assets that are their own source.
auto asset_display_name(const UUID& uuid, const std::filesystem::path& registry_path) -> std::string;
auto asset_display_path(const UUID& uuid, const std::filesystem::path& registry_path) -> std::filesystem::path;

// Browses the asset registry: the body of the Asset Manager panel, and of the picker windows the
// inspector's asset fields open.
struct AssetBrowser {
  struct Pick {
    UUID uuid = {};
    AssetType type = AssetType::None;
    std::filesystem::path path = {};
  };

  // Registry snapshot for the current frame, plus the indices into it that survived the filters, in
  // the order the table asked for.
  std::vector<Asset> assets = {};
  std::vector<u32> visible_assets = {};

  ImGuiTextFilter text_filter = {};
  u32 type_filter_mask = 0;
  bool loaded_only = false;

  UUID selected_uuid = UUID(nullptr);
  bool scroll_to_selection = false;

  // Draws the browser into the window the caller has open. Returns true when the user committed to
  // the selection, which only the picker acts on.
  auto draw(this AssetBrowser& self, AssetType forced_type, bool picking) -> bool;

  // Returns the asset on the frame the user commits to one, and closes the picker. `current` is
  // highlighted when the picker opens so the field's existing value is easy to find.
  auto render_picker(this AssetBrowser& self, const char* id, bool* open, AssetType filter, const UUID& current = {})
    -> option<Pick>;

  auto refresh(this AssetBrowser& self, AssetType forced_type) -> void;
  auto sort_visible(this AssetBrowser& self, const ImGuiTableSortSpecs* specs) -> void;
  auto find_asset(this AssetBrowser& self, const UUID& uuid) -> const Asset*;

  auto draw_toolbar(this AssetBrowser& self, AssetType forced_type) -> void;
  auto draw_filter_menu(this AssetBrowser& self) -> void;
  // Returns true when a row was activated (double click), which is what commits a pick.
  auto draw_list(this AssetBrowser& self, bool picking, f32 height) -> bool;
  auto draw_details(this AssetBrowser& self, const Asset& asset) -> void;
  auto draw_row_context_menu(this AssetBrowser& self, const Asset& asset) -> void;
};

class AssetManagerPanel : public EditorPanelState {
public:
  AssetManagerPanel();

  auto on_update(this AssetManagerPanel& self) -> void;
  auto on_render(this AssetManagerPanel& self, vuk::ImageAttachment swapchain_attachment) -> void;

private:
  AssetBrowser browser = {};
};
} // namespace ox
