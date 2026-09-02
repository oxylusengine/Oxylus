#include "AssetManagerPanel.hpp"

#include <algorithm>
#include <icons/IconsMaterialDesignIcons.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <tracy/Tracy.hpp>

#include "Core/App.hpp"
#include "Editor.hpp"
#include "Memory/Stack.hpp"
#include "UI/PayloadData.hpp"
#include "UI/UI.hpp"

namespace ox {
constexpr AssetType FILTERABLE_ASSET_TYPES[] = {
  AssetType::Model,
  AssetType::Texture,
  AssetType::Material,
  AssetType::Scene,
  AssetType::Audio,
  AssetType::Script,
  AssetType::ParticleSystem,
  AssetType::Terrain,
  AssetType::Shader,
  AssetType::Font,
};

constexpr auto asset_type_bit(const AssetType type) -> u32 { return 1u << static_cast<u32>(type); }

constexpr u32 ALL_ASSET_TYPES_MASK = [] {
  auto mask = 0_u32;
  for (const auto type : FILTERABLE_ASSET_TYPES) {
    mask |= asset_type_bit(type);
  }
  return mask;
}();

enum class AssetColumn : i32 { Name = 0, Type, References, Status, UUID };

static auto asset_type_label(const AssetType type) -> std::string_view { return AssetManager::to_asset_type_sv(type); }

static auto asset_name(const Asset& asset) -> std::string {
  auto name = asset.path.filename().string();
  return name.empty() ? asset.uuid.str() : name;
}

// A loaded asset is one the manager holds a payload for; an unloaded one is only a registry entry
// pointing at a file, which is still pickable.
static auto asset_status_label(const Asset& asset) -> const c8* { return asset.is_loaded() ? "Loaded" : "Unloaded"; }

auto asset_type_icon(const AssetType type) -> const c8* {
  switch (type) {
    case AssetType::None          : return ICON_MDI_HELP_CIRCLE_OUTLINE;
    case AssetType::Shader        : return ICON_MDI_IMAGE_FILTER_BLACK_WHITE;
    case AssetType::Model         : return ICON_MDI_VECTOR_POLYGON;
    case AssetType::Texture       : return ICON_MDI_FILE_IMAGE;
    case AssetType::Material      : return ICON_MDI_PALETTE_SWATCH;
    case AssetType::Font          : return ICON_MDI_FORMAT_FONT;
    case AssetType::Scene         : return ICON_MDI_FILE_TREE;
    case AssetType::Audio         : return ICON_MDI_VOLUME_HIGH;
    case AssetType::Script        : return ICON_MDI_LANGUAGE_LUA;
    case AssetType::Terrain       : return ICON_MDI_TERRAIN;
    case AssetType::ParticleSystem: return ICON_MDI_SHIMMER;
  }

  return ICON_MDI_HELP_CIRCLE_OUTLINE;
}

auto AssetBrowser::refresh(this AssetBrowser& self, const AssetType forced_type) -> void {
  ZoneScoped;

  self.assets = App::mod<AssetManager>().get_registry_snapshot();

  const auto mask = forced_type != AssetType::None
                      ? asset_type_bit(forced_type)
                      : (self.type_filter_mask == 0 ? ALL_ASSET_TYPES_MASK : self.type_filter_mask);

  self.visible_assets.clear();
  self.visible_assets.reserve(self.assets.size());
  for (auto index = 0_u32; index < self.assets.size(); ++index) {
    const auto& asset = self.assets[index];
    if (!asset.uuid || (mask & asset_type_bit(asset.type)) == 0) {
      continue;
    }
    if (self.loaded_only && !asset.is_loaded()) {
      continue;
    }
    if (
      self.text_filter.IsActive() && !self.text_filter.PassFilter(asset_name(asset).c_str()) &&
      !self.text_filter.PassFilter(asset.uuid.str().c_str())
    ) {
      continue;
    }

    self.visible_assets.emplace_back(index);
  }
}

auto AssetBrowser::sort_visible(this AssetBrowser& self, const ImGuiTableSortSpecs* specs) -> void {
  ZoneScoped;

  if (!specs || specs->SpecsCount == 0) {
    return;
  }

  const auto& spec = specs->Specs[0];
  const auto ascending = spec.SortDirection != ImGuiSortDirection_Descending;

  std::ranges::stable_sort(self.visible_assets, [&self, &spec, ascending](const u32 lhs_index, const u32 rhs_index) {
    const auto& lhs = self.assets[lhs_index];
    const auto& rhs = self.assets[rhs_index];

    auto order = std::strong_ordering::equal;
    switch (static_cast<AssetColumn>(spec.ColumnUserID)) {
      case AssetColumn::Name      : order = asset_name(lhs) <=> asset_name(rhs); break;
      case AssetColumn::Type      : order = asset_type_label(lhs.type) <=> asset_type_label(rhs.type); break;
      case AssetColumn::References: order = lhs.ref_count <=> rhs.ref_count; break;
      case AssetColumn::Status    : order = lhs.is_loaded() <=> rhs.is_loaded(); break;
      case AssetColumn::UUID      : order = lhs.uuid.str() <=> rhs.uuid.str(); break;
    }

    return ascending ? order < 0 : order > 0;
  });
}

auto AssetBrowser::find_asset(this AssetBrowser& self, const UUID& uuid) -> const Asset* {
  if (!uuid) {
    return nullptr;
  }

  const auto it = std::ranges::find_if(self.assets, [&uuid](const Asset& asset) { return asset.uuid == uuid; });

  return it != self.assets.end() ? &*it : nullptr;
}

auto AssetBrowser::draw_filter_menu(this AssetBrowser& self) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  const auto selected_count = std::popcount(self.type_filter_mask);
  const c8* button_label = nullptr;
  if (selected_count == 0) {
    button_label = stack.format_char("{} All types###AssetFilterButton", ICON_MDI_FILTER_OFF_OUTLINE);
  } else if (selected_count == 1) {
    const auto selected_type = *std::ranges::find_if(FILTERABLE_ASSET_TYPES, [&self](const AssetType type) {
      return (self.type_filter_mask & asset_type_bit(type)) != 0;
    });
    button_label = stack.format_char("{} {}###AssetFilterButton", ICON_MDI_FILTER, asset_type_label(selected_type));
  } else {
    button_label = stack.format_char("{} {} types###AssetFilterButton", ICON_MDI_FILTER, selected_count);
  }

  if (UI::button(button_label)) {
    ImGui::OpenPopup("AssetFilterPopup");
  }
  UI::tooltip_hover("Filter the list by asset type");

  if (!ImGui::BeginPopup("AssetFilterPopup")) {
    return;
  }

  ImGui::SeparatorText("Asset types");

  auto mask = self.type_filter_mask;
  for (const auto type : FILTERABLE_ASSET_TYPES) {
    const auto bit = asset_type_bit(type);
    auto checked = (mask & bit) != 0;
    const auto label = stack.format_char(
      "  {} {}###AssetFilterType{}",
      asset_type_icon(type),
      asset_type_label(type),
      static_cast<u32>(type)
    );
    if (ImGui::Checkbox(label, &checked)) {
      mask = checked ? mask | bit : mask & ~bit;
    }
  }

  ImGui::Separator();
  ImGui::Checkbox("Loaded assets only", &self.loaded_only);

  ImGui::Separator();
  if (UI::button(stack.format_char("{} All", ICON_MDI_SELECT_ALL))) {
    mask = ALL_ASSET_TYPES_MASK;
  }
  ImGui::SameLine();
  if (UI::button(stack.format_char("{} Clear", ICON_MDI_FILTER_OFF_OUTLINE))) {
    mask = 0;
  }

  self.type_filter_mask = mask;

  ImGui::EndPopup();
}

auto AssetBrowser::draw_toolbar(this AssetBrowser& self, const AssetType forced_type) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  // A picker is already pinned to one type, so the filter menu would only be able to show nothing.
  if (forced_type == AssetType::None) {
    self.draw_filter_menu();
  } else {
    ImGui::BeginDisabled();
    UI::button(stack.format_char("{} {}", asset_type_icon(forced_type), asset_type_label(forced_type)));
    ImGui::EndDisabled();
  }

  ImGui::SameLine();

  const auto clear_width = ImGui::GetFrameHeight();
  const auto search_cursor_x = ImGui::GetCursorPosX();
  self.text_filter.Draw(
    "###AssetSearch",
    ImGui::GetContentRegionAvail().x - clear_width - ImGui::GetStyle().ItemSpacing.x
  );
  if (!self.text_filter.IsActive()) {
    ImGui::SameLine();
    ImGui::SetCursorPosX(search_cursor_x + ImGui::GetFontSize() * 0.5f);
    ImGui::BeginDisabled();
    ImGui::TextUnformatted(stack.format_char(" {} Search by name or UUID...", ICON_MDI_MAGNIFY));
    ImGui::EndDisabled();
  }

  ImGui::SameLine();
  if (UI::button(stack.format_char("{}###AssetSearchClear", ICON_MDI_CLOSE), {clear_width, clear_width})) {
    self.text_filter.Clear();
  }
  UI::tooltip_hover("Clear the search");
}

auto AssetBrowser::draw_row_context_menu(this AssetBrowser& self, const Asset& asset) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  auto& asset_man = App::mod<AssetManager>();

  if (ImGui::MenuItem(stack.format_char("{} Load", ICON_MDI_DOWNLOAD), nullptr, false, !asset.is_loaded())) {
    asset_man.load_asset(asset.uuid);
  }
  if (ImGui::MenuItem(stack.format_char("{} Release reference", ICON_MDI_UPLOAD), nullptr, false, asset.is_loaded())) {
    asset_man.unload_asset(asset.uuid);
  }

  ImGui::Separator();

  if (ImGui::MenuItem(stack.format_char("{} Copy UUID", ICON_MDI_CONTENT_COPY))) {
    ImGui::SetClipboardText(asset.uuid.str().c_str());
  }
  if (ImGui::MenuItem(stack.format_char("{} Copy path", ICON_MDI_CONTENT_COPY))) {
    ImGui::SetClipboardText(asset.path.string().c_str());
  }
}

auto AssetBrowser::draw_list(this AssetBrowser& self, const bool picking) -> bool {
  ZoneScoped;
  memory::ScopedStack stack;

  constexpr ImGuiTableFlags TABLE_FLAGS = ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable |
                                          ImGuiTableFlags_Hideable | ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg |
                                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                          ImGuiTableFlags_SizingStretchProp;

  if (!ImGui::BeginTable("AssetTable", 5, TABLE_FLAGS)) {
    return false;
  }

  ImGui::TableSetupColumn(
    "Name",
    ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch,
    0.0f,
    static_cast<ImGuiID>(AssetColumn::Name)
  );
  ImGui::TableSetupColumn(
    "Type",
    ImGuiTableColumnFlags_WidthFixed,
    UI::scale(110.0f),
    static_cast<ImGuiID>(AssetColumn::Type)
  );
  ImGui::TableSetupColumn(
    "Refs",
    ImGuiTableColumnFlags_WidthFixed,
    UI::scale(45.0f),
    static_cast<ImGuiID>(AssetColumn::References)
  );
  ImGui::TableSetupColumn(
    "Status",
    ImGuiTableColumnFlags_WidthFixed,
    UI::scale(80.0f),
    static_cast<ImGuiID>(AssetColumn::Status)
  );
  ImGui::TableSetupColumn(
    "UUID",
    ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide,
    UI::scale(280.0f),
    static_cast<ImGuiID>(AssetColumn::UUID)
  );
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableHeadersRow();

  if (auto* specs = ImGui::TableGetSortSpecs()) {
    self.sort_visible(specs);
    specs->SpecsDirty = false;
  }

  auto activated = false;

  auto clipper = ImGuiListClipper();
  clipper.Begin(static_cast<i32>(self.visible_assets.size()));

  // The row to scroll to is almost always clipped away, so it has to be forced into the range the
  // clipper actually submits or SetScrollHereY below never runs.
  if (self.scroll_to_selection) {
    const auto row = std::ranges::find_if(self.visible_assets, [&self](const u32 index) {
      return self.assets[index].uuid == self.selected_uuid;
    });
    if (row != self.visible_assets.end()) {
      clipper.IncludeItemByIndex(static_cast<i32>(row - self.visible_assets.begin()));
    } else {
      self.scroll_to_selection = false;
    }
  }

  while (clipper.Step()) {
    for (auto row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
      const auto& asset = self.assets[self.visible_assets[static_cast<usize>(row)]];
      const auto uuid_str = asset.uuid.str();

      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::PushID(uuid_str.c_str());

      constexpr ImGuiSelectableFlags SELECTABLE_FLAGS = ImGuiSelectableFlags_SpanAllColumns |
                                                        ImGuiSelectableFlags_AllowOverlap |
                                                        ImGuiSelectableFlags_AllowDoubleClick;
      const auto is_selected = asset.uuid == self.selected_uuid;
      const auto label = stack.format_char("{}  {}", asset_type_icon(asset.type), asset_name(asset));
      if (ImGui::Selectable(label, is_selected, SELECTABLE_FLAGS, {0.0f, ImGui::GetFrameHeight()})) {
        self.selected_uuid = asset.uuid;
        activated |= ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
      }

      if (self.scroll_to_selection && is_selected) {
        ImGui::SetScrollHereY();
        self.scroll_to_selection = false;
      }

      // Dragging out of the browser is how an asset gets onto a component field without going
      // through the file tree.
      if (!picking && ImGui::BeginDragDropSource()) {
        auto payload = PayloadData(asset.path.string(), asset.uuid);
        ImGui::SetDragDropPayload(PayloadData::DRAG_DROP_TARGET, &payload, payload.size());
        ImGui::TextUnformatted(label);
        ImGui::EndDragDropSource();
      }

      if (ImGui::BeginPopupContextItem("AssetRowContext")) {
        self.selected_uuid = asset.uuid;
        self.draw_row_context_menu(asset);
        ImGui::EndPopup();
      }

      if (ImGui::TableSetColumnIndex(1)) {
        ImGui::TextUnformatted(asset_type_label(asset.type).data());
      }
      if (ImGui::TableSetColumnIndex(2)) {
        // explicit format here because MSVC %lu != clang %lu (its %llu instead)
        ImGui::TextUnformatted(stack.format_char("{}", asset.ref_count));
      }
      if (ImGui::TableSetColumnIndex(3)) {
        if (!asset.is_loaded()) {
          ImGui::BeginDisabled();
        }
        ImGui::TextUnformatted(asset_status_label(asset));
        if (!asset.is_loaded()) {
          ImGui::EndDisabled();
        }
      }
      if (ImGui::TableSetColumnIndex(4)) {
        ImGui::TextUnformatted(uuid_str.c_str());
      }

      ImGui::PopID();
    }
  }

  ImGui::EndTable();

  return activated;
}

auto AssetBrowser::draw_details(this AssetBrowser& self, const Asset& asset) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  auto& asset_man = App::mod<AssetManager>();

  const auto preview_size = ImGui::GetContentRegionAvail().x;
  auto thumbnail = App::mod<Editor>().thumbnail_manager.get_thumbnail(asset.type, asset.path, asset.uuid);
  if (thumbnail) {
    UI::image(thumbnail, {preview_size, preview_size});
  } else {
    // Centered type glyph, scaled to fill roughly the same box a thumbnail would.
    const auto glyph = asset_type_icon(asset.type);
    const auto font_size = ox::max(preview_size * 0.5f, ImGui::GetFontSize());
    ImGui::PushFont(nullptr, font_size);
    const auto glyph_width = ImGui::CalcTextSize(glyph).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ox::max((preview_size - glyph_width) * 0.5f, 0.0f));
    ImGui::BeginDisabled();
    ImGui::TextUnformatted(glyph);
    ImGui::EndDisabled();
    ImGui::PopFont();
  }

  auto name = asset_name(asset);
  auto path_str = asset.path.string();
  auto uuid_str = asset.uuid.str();

  UI::begin_properties(ImGuiTableFlags_SizingStretchProp);
  UI::text("Name", name);
  UI::text("Type", asset_type_label(asset.type));
  UI::text("Status", asset_status_label(asset));
  UI::text("References", stack.format("{}", asset.ref_count));
  UI::input_text("UUID", &uuid_str, ImGuiInputTextFlags_ReadOnly);
  UI::input_text("Path", &path_str, ImGuiInputTextFlags_ReadOnly);
  UI::end_properties();

  const auto button_width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
  const auto button_size = ImVec2(button_width, ImGui::GetFrameHeight());

  ImGui::BeginDisabled(asset.is_loaded());
  if (UI::button(stack.format_char("{} Load", ICON_MDI_DOWNLOAD), button_size, "Load the asset and take a reference")) {
    asset_man.load_asset(asset.uuid);
  }
  ImGui::EndDisabled();

  ImGui::SameLine();

  ImGui::BeginDisabled(!asset.is_loaded());
  if (
    UI::button(
      stack.format_char("{} Release", ICON_MDI_UPLOAD),
      button_size,
      "Give back one reference; the payload is freed when the last one goes"
    )
  ) {
    asset_man.unload_asset(asset.uuid);
  }
  ImGui::EndDisabled();

  if (UI::button(stack.format_char("{} Copy UUID", ICON_MDI_CONTENT_COPY), {ImGui::GetContentRegionAvail().x, 0.0f})) {
    ImGui::SetClipboardText(uuid_str.c_str());
  }
}

// Shared body of the browser and the picker: toolbar, split list/details, and a status line. The
// return value only means something in picker mode, where it says the user committed.
auto AssetBrowser::draw(this AssetBrowser& self, const AssetType forced_type, const bool picking) -> bool {
  ZoneScoped;
  memory::ScopedStack stack;

  self.refresh(forced_type);
  self.draw_toolbar(forced_type);

  auto committed = false;

  const auto footer_height = picking ? ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing()
                                     : ImGui::GetTextLineHeightWithSpacing();
  const auto body_height = ox::max(ImGui::GetContentRegionAvail().y - footer_height, ImGui::GetFrameHeight());

  if (
    ImGui::BeginTable(
      "AssetViewerSplit",
      2,
      ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings,
      {0.0f, body_height}
    )
  ) {
    ImGui::TableSetupColumn("List", ImGuiTableColumnFlags_WidthStretch, 0.7f);
    ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch, 0.3f);

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    committed = self.draw_list(picking);

    ImGui::TableSetColumnIndex(1);
    if (ImGui::BeginChild("AssetDetails", {0.0f, 0.0f})) {
      if (const auto* selected = self.find_asset(self.selected_uuid)) {
        self.draw_details(*selected);
      } else {
        ImGui::TextDisabled("Select an asset to inspect it.");
      }
    }
    ImGui::EndChild();

    ImGui::EndTable();
  }

  ImGui::BeginDisabled();
  ImGui::TextUnformatted(stack.format_char("{} of {} assets", self.visible_assets.size(), self.assets.size()));
  ImGui::EndDisabled();

  return committed;
}

auto AssetBrowser::render_picker(
  this AssetBrowser& self, const char* id, bool* open, const AssetType filter, const UUID& current
) -> option<Pick> {
  ZoneScoped;
  memory::ScopedStack stack;

  if (open && !*open) {
    return nullopt;
  }

  // Opening onto the field's current value is the first thing a picker should show.
  if (current && !self.selected_uuid) {
    self.selected_uuid = current;
    self.scroll_to_selection = true;
  }

  auto picked = option<Pick>(nullopt);

  ImGui::SetNextWindowSize(UI::scale({860.0f, 520.0f}), ImGuiCond_Appearing);
  UI::center_next_window(ImGuiCond_Appearing);
  if (ImGui::Begin(id, open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) {
    auto commit = self.draw(filter, true);

    const auto* selected = self.find_asset(self.selected_uuid);

    const auto button_width = UI::scale(110.0f);
    ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - button_width * 2.0f - ImGui::GetStyle().ItemSpacing.x);

    ImGui::BeginDisabled(selected == nullptr);
    if (UI::button(stack.format_char("{} Select", ICON_MDI_CHECK), {button_width, 0.0f})) {
      commit = true;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    auto cancel = UI::button(stack.format_char("{} Cancel", ICON_MDI_CLOSE), {button_width, 0.0f});

    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
      commit |= ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
      cancel |= ImGui::IsKeyPressed(ImGuiKey_Escape);
    }

    if (commit && selected) {
      picked = Pick{.uuid = selected->uuid, .type = selected->type, .path = selected->path};
    }

    if (cancel || picked.has_value()) {
      self.selected_uuid = UUID(nullptr);
      if (open) {
        *open = false;
      }
    }
  }
  ImGui::End();

  return picked;
}

AssetManagerPanel::AssetManagerPanel() : EditorPanelState("Asset Manager", ICON_MDI_FOLDER_SYNC, false) {}

auto AssetManagerPanel::on_update(this AssetManagerPanel& self) -> void {}

auto AssetManagerPanel::on_render(this AssetManagerPanel& self, vuk::ImageAttachment swapchain_attachment) -> void {
  ZoneScoped;

  ImGui::SetNextWindowSize(UI::scale({900.0f, 560.0f}), ImGuiCond_FirstUseEver);
  if (ImGui::Begin(self.id.c_str(), &self.visible, ImGuiWindowFlags_NoCollapse)) {
    self.browser.draw(AssetType::None, false);
  }
  ImGui::End();
}
} // namespace ox
