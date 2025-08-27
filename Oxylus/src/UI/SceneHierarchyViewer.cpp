#include "UI/SceneHierarchyViewer.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Core/FileSystem.hpp"
#include "UI/AssetManagerViewer.hpp"
#include "Utils/ImGuiScoped.hpp"

namespace ox {
SceneHierarchyViewer::SceneHierarchyViewer(Scene* scene) : scene_(scene) {}

auto SceneHierarchyViewer::render(const char* id, bool* visible) -> void {
  ZoneScoped;

  ImGuiScoped::StyleVar cellpad(ImGuiStyleVar_CellPadding, {0, 0});

  if (ImGui::Begin(id, visible, ImGuiWindowFlags_NoCollapse)) {
    constexpr ImGuiTableFlags table_flags = ImGuiTableFlags_ContextMenuInBody | ImGuiTableFlags_BordersInner |
                                            ImGuiTableFlags_ScrollY;
    const float line_height = ImGui::GetTextLineHeight();

    float filter_cursor_pos_x = ImGui::GetCursorPosX();
    if (ImGui::TreeNodeEx("Scripts", ImGuiTreeNodeFlags_Framed)) {
      scripts_filter_.Draw("###HierarchyFilter",
                           ImGui::GetContentRegionAvail().x - (ImGui::CalcTextSize(add_icon).x + 20.0f));
      ImGui::SameLine();

      if (ImGui::Button(add_icon))
        ImGui::OpenPopup("scene_h_scripts_context_window");

      if (ImGui::BeginPopupContextWindow("scene_h_scripts_context_window",
                                         ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        draw_scripts_context_menu();
        ImGui::EndPopup();
      }

      if (!scripts_filter_.IsActive()) {
        ImGui::SameLine();
        ImGui::SetCursorPosX(filter_cursor_pos_x + ImGui::GetFontSize() * 0.5f);
        auto search_txt = fmt::format("  {} Search scripts...", search_icon);
        ImGui::TextUnformatted(search_txt.c_str());
      }

      if (ImGui::BeginTable("ScriptsTable", 1, table_flags, ImVec2(0, 100))) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_NoClip);

        for (auto& [uuid, system] : scene_->get_lua_systems()) {
          auto system_name = fs::get_file_name(system->get_path());
          if (scripts_filter_.IsActive() && !scripts_filter_.PassFilter(system_name.c_str())) {
            continue;
          }

          ImGui::TableNextRow();

          ImGui::TableNextColumn();

          bool is_selected = selected_script_ != nullptr && *selected_script_ == uuid;

          ImGuiTreeNodeFlags flags = (is_selected ? ImGuiTreeNodeFlags_Selected : 0);
          flags |= ImGuiTreeNodeFlags_OpenOnArrow;
          flags |= ImGuiTreeNodeFlags_SpanFullWidth;
          flags |= ImGuiTreeNodeFlags_FramePadding;
          flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

          const bool highlight = is_selected;
          if (highlight) {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(header_selected_color));
            ImGui::PushStyleColor(ImGuiCol_Header, header_selected_color);
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, header_selected_color);
          }

          ImGui::TreeNodeEx(uuid.str().c_str(), flags, "%s %s", script_icon, system_name.c_str());

          if (highlight)
            ImGui::PopStyleColor(2);

          // Select
          if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            selected_entity_.reset();
            selected_script_ = &uuid;
          }
          if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            if (opened_script_callback)
              opened_script_callback(uuid);
          }
        }

        if (ImGui::BeginPopupContextWindow("scene_h_scripts_context_window",
                                           ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
          selected_entity_.reset();
          selected_script_ = nullptr;
          draw_scripts_context_menu();
          ImGui::EndPopup();
        }

        ImGui::EndTable();

        if (ImGui::BeginPopupContextItem()) {
          if (ImGui::MenuItem("Reload")) {
            if (selected_script_ && !scene_->is_running()) {
              if (auto lua_system = scene_->get_lua_system(*selected_script_)) {
                lua_system->reload();
              }
            }
          }
          if (ImGui::MenuItem("Remove", "Del")) {
            if (selected_script_) {
              scene_->remove_lua_system(*selected_script_);
            }
          }

          ImGui::EndPopup();
        }

        table_hovered_scripts = ImGui::IsItemHovered();

        if (ImGui::IsItemClicked()) {
          selected_entity_.reset();
          selected_script_ = nullptr;
        }
      }
      ImGui::TreePop();
    }

    filter_cursor_pos_x = ImGui::GetCursorPosX();

    if (ImGui::TreeNodeEx("Entities", ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_DefaultOpen)) {
      entities_filter_.Draw("###HierarchyFilter",
                            ImGui::GetContentRegionAvail().x - (ImGui::CalcTextSize(add_icon).x + 20.0f));
      ImGui::SameLine();

      if (ImGui::Button(add_icon))
        ImGui::OpenPopup("scene_h_entities_context_window");

      if (ImGui::BeginPopupContextWindow("scene_h_entities_context_window",
                                         ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        draw_entities_context_menu();
        ImGui::EndPopup();
      }

      if (!entities_filter_.IsActive()) {
        ImGui::SameLine();
        ImGui::SetCursorPosX(filter_cursor_pos_x + ImGui::GetFontSize() * 0.5f);
        auto search_txt = fmt::format("  {} Search entities...", search_icon);
        ImGui::TextUnformatted(search_txt.c_str());
      }

      const ImVec2 cursor_pos = ImGui::GetCursorPos();
      const ImVec2 region = ImGui::GetContentRegionAvail();
      if (region.x != 0.0f && region.y != 0.0f)
        ImGui::InvisibleButton("##DragDropTargetBehindTable", region);

      ImGui::SetCursorPos(cursor_pos);
      if (ImGui::BeginTable("HierarchyTable", 3, table_flags)) {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_NoClip);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, line_height * 3.0f);

        const auto vis_colon = fmt::format("  {}", visibility_icon_on);
        ImGui::TableSetupColumn(vis_colon.c_str(), ImGuiTableColumnFlags_WidthFixed, line_height * 2.0f);

        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        scene_->world.query_builder<TransformComponent>()
            .with(flecs::Disabled)
            .optional()
            .build()
            .each([this](const flecs::entity e, TransformComponent) {
              if (e.parent() == flecs::entity::null())
                draw_entity_node(e);
            });
        ImGui::PopStyleVar();

        if (ImGui::BeginPopupContextWindow("scene_h_entities_context_window",
                                           ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
          selected_entity_.reset();
          selected_script_ = nullptr;
          draw_entities_context_menu();
          ImGui::EndPopup();
        }

        ImGui::EndTable();

        table_hovered_ = ImGui::IsItemHovered();

        if (ImGui::IsItemClicked()) {
          selected_entity_.reset();
          selected_script_ = nullptr;
        }
      }

      if (ImGui::IsMouseDown(0) && window_hovered_) {
        selected_entity_.reset();
        selected_script_ = nullptr;
      }

      if (dragged_entity_ != flecs::entity::null() && dragged_entity_target_ != flecs::entity::null()) {
        dragged_entity_.child_of(dragged_entity_target_);
        dragged_entity_ = flecs::entity::null();
        dragged_entity_target_ = flecs::entity::null();
      }

      ImGui::TreePop();
    }

    window_hovered_ = ImGui::IsWindowHovered();
  }

  ImGui::End();
}

auto SceneHierarchyViewer::draw_entity_node(flecs::entity entity,
                                            uint32_t depth,
                                            bool force_expand_tree,
                                            bool is_part_of_prefab) -> ImRect {
  ZoneScoped;

  if (entity.has<Hidden>())
    return {0, 0, 0, 0};

  ImGui::TableNextRow();
  ImGui::TableNextColumn();

  const auto child_count = scene_->world.count(flecs::ChildOf, entity);

  if (entities_filter_.IsActive() && !entities_filter_.PassFilter(entity.name().c_str())) {
    entity.children([this](flecs::entity child) { draw_entity_node(child); });
    return {0, 0, 0, 0};
  }

  const auto is_selected = selected_entity_.get().id() == entity.id();

  ImGuiTreeNodeFlags flags = (is_selected ? ImGuiTreeNodeFlags_Selected : 0);
  flags |= ImGuiTreeNodeFlags_OpenOnArrow;
  flags |= ImGuiTreeNodeFlags_SpanFullWidth;
  flags |= ImGuiTreeNodeFlags_FramePadding;

  if (child_count == 0) {
    flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
  }

  const bool highlight = is_selected;
  if (highlight) {
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(header_selected_color));
    ImGui::PushStyleColor(ImGuiCol_Header, header_selected_color);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, header_selected_color);
  }

  if (force_expand_tree)
    ImGui::SetNextItemOpen(true);

  const bool prefab_color_applied = is_part_of_prefab && !is_selected;
  if (prefab_color_applied)
    ImGui::PushStyleColor(ImGuiCol_Text, header_selected_color);

  const bool opened = ImGui::TreeNodeEx(
      reinterpret_cast<void*>(entity.raw_id()), flags, "%s %s", entity_icon, entity.name().c_str());

  if (highlight)
    ImGui::PopStyleColor(2);

  // Select
  if (!ImGui::IsItemToggledOpen() && ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
    selected_entity_.set(entity);
    selected_script_ = nullptr;
  }

  // Expand recursively
  if (ImGui::IsItemToggledOpen() && (ImGui::IsKeyDown(ImGuiKey_LeftAlt) || ImGui::IsKeyDown(ImGuiKey_RightAlt)))
    force_expand_tree = opened;

  bool entity_deleted = false;

  if (ImGui::BeginPopupContextItem()) {
    if (ImGui::MenuItem("Rename", "F2"))
      renaming_entity_ = entity;
    if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
      auto clone_entity = [](flecs::entity entity) -> flecs::entity {
        std::string clone_name = entity.name().c_str();
        while (entity.world().lookup(clone_name.data())) {
          clone_name = fmt::format("{}_clone", clone_name);
        }
        auto cloned_entity = entity.clone(true);
        return cloned_entity.set_name(clone_name.data());
      };

      selected_entity_.set(clone_entity(entity));
      selected_script_ = nullptr;
    }
    if (ImGui::MenuItem("Delete", "Del"))
      entity_deleted = true;

    ImGui::Separator();

    draw_entities_context_menu();

    ImGui::EndPopup();
  }

  ImVec2 vertical_line_start = ImGui::GetCursorScreenPos();
  vertical_line_start.x -= 0.5f;
  vertical_line_start.y -= ImGui::GetFrameHeight() * 0.5f;

  // Drag Drop
  {
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* entity_payload = ImGui::AcceptDragDropPayload("Entity")) {
        dragged_entity_ = *static_cast<flecs::entity*>(entity_payload->Data);
        dragged_entity_target_ = entity;
      } else if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM")) {
        const std::filesystem::path path = std::filesystem::path((const char*)payload->Data);
        if (path.extension() == ".oxprefab") {
          // dragged_entity = EntitySerializer::deserialize_entity_as_prefab(path.string().c_str(), _scene.get());
          // dragged_entity = entity;
        }
      }

      ImGui::EndDragDropTarget();
    }

    if (ImGui::BeginDragDropSource()) {
      ImGui::SetDragDropPayload("Entity", &entity, sizeof(flecs::entity));
      ImGui::TextUnformatted(entity.name().c_str());
      ImGui::EndDragDropSource();
    }
  }

  if (entity.id() == renaming_entity_.id()) {
    static bool renaming = false;
    if (!renaming) {
      renaming = true;
      ImGui::SetKeyboardFocusHere();
    }

    std::string name{entity.name()};
    if (ImGui::InputText("##Tag", &name)) {
      entity.set_name(name.c_str());
    }

    if (ImGui::IsItemDeactivated()) {
      renaming = false;
      renaming_entity_ = flecs::entity::null();
    }
  }

  ImGui::TableNextColumn();

  ImGui::PushStyleColor(ImGuiCol_Button, {0, 0, 0, 0});
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0, 0, 0, 0});
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0, 0, 0, 0});

  const float button_size_x = ImGui::GetContentRegionAvail().x;
  const float frame_height = ImGui::GetFrameHeight();
  ImGui::PushID(entity.name());
  ImGui::Button(is_part_of_prefab ? "Prefab" : "Entity", {button_size_x, frame_height});
  ImGui::PopID();
  // Select
  if (ImGui::IsItemDeactivated() && ImGui::IsItemHovered() && !ImGui::IsItemToggledOpen()) {
    selected_entity_.set(entity);
  }

  ImGui::TableNextColumn();
  // Visibility Toggle
  {
    ImGui::Text("  %s", entity.enabled() ? visibility_icon_on : visibility_icon_off);

    if (ImGui::IsItemHovered() && (ImGui::IsMouseDragging(0) || ImGui::IsItemClicked())) {
      entity.enabled() ? entity.disable() : entity.enable();
    }
  }

  ImGui::PopStyleColor(3);

  if (prefab_color_applied)
    ImGui::PopStyleColor();

  // Open
  const ImRect node_rect = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
  {
    if (opened && !entity_deleted) {
      ImColor tree_line_color;
      depth %= 4;
      switch (depth) {
        case 0 : tree_line_color = ImColor(254, 112, 246); break;
        case 1 : tree_line_color = ImColor(142, 112, 254); break;
        case 2 : tree_line_color = ImColor(112, 180, 254); break;
        case 3 : tree_line_color = ImColor(48, 134, 198); break;
        default: tree_line_color = ImColor(255, 255, 255); break;
      }

      entity.children([this, depth, force_expand_tree, is_part_of_prefab, vertical_line_start, tree_line_color](
                          const flecs::entity child) {
        const float horizontal_tree_line_size = scene_->world.count(flecs::ChildOf, child) > 0 ? 9.f : 18.f;
        // chosen arbitrarily
        const ImRect child_rect = draw_entity_node(child, depth + 1, force_expand_tree, is_part_of_prefab);

        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        ImVec2 vertical_line_end = vertical_line_start;
        constexpr float line_thickness = 1.5f;
        const float midpoint = (child_rect.Min.y + child_rect.Max.y) / 2.0f;
        draw_list->AddLine(ImVec2(vertical_line_start.x, midpoint),
                           ImVec2(vertical_line_start.x + horizontal_tree_line_size, midpoint),
                           tree_line_color,
                           line_thickness);
        vertical_line_end.y = midpoint;
        draw_list->AddLine(vertical_line_start, vertical_line_end, tree_line_color, line_thickness);
      });
    }

    if (opened && child_count > 0)
      ImGui::TreePop();
  }

  // PostProcess Actions
  if (entity_deleted)
    deleted_entity_ = entity;

  return node_rect;
}

auto SceneHierarchyViewer::draw_entities_context_menu() -> void {
  ZoneScoped;

  const bool has_context = selected_entity_.get() != flecs::entity::null();

  flecs::entity to_select = flecs::entity::null();

  ImGuiScoped::StyleVar styleVar1(ImGuiStyleVar_ItemInnerSpacing, {0, 5});
  ImGuiScoped::StyleVar styleVar2(ImGuiStyleVar_ItemSpacing, {1, 5});
  if (ImGui::BeginMenu("Create")) {
    if (ImGui::MenuItem("New Entity")) {
      to_select = scene_->create_entity("entity", true);
    }

    auto* asset_man = App::get_asset_manager();

    if (ImGui::MenuItem("Sprite")) {
      to_select = scene_->create_entity("sprite", true).add<SpriteComponent>();
      to_select.get_mut<SpriteComponent>().material = asset_man->create_asset(AssetType::Material, {});
      asset_man->load_material(to_select.get_mut<SpriteComponent>().material, Material{});
    }

    if (ImGui::MenuItem("Camera")) {
      to_select = scene_->create_entity("camera", true);
      to_select.add<CameraComponent>().get_mut<TransformComponent>().rotation.y = glm::radians(-90.f);
    }

    if (ImGui::BeginMenu("Light")) {
      if (ImGui::MenuItem("Light")) {
        to_select = scene_->create_entity("light", true).add<LightComponent>();
      }
      if (ImGui::MenuItem("Sun")) {
        to_select = scene_->create_entity("sun", true)
                        .set<LightComponent>(LightComponent{.type = LightComponent::Directional, .intensity = 10.f})
                        .add<AtmosphereComponent>();
      }
      ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Audio")) {
      if (ImGui::MenuItem("Audio Source")) {
        to_select = scene_->create_entity("audio_source", true).add<AudioSourceComponent>();
        ImGui::CloseCurrentPopup();
      }
      if (ImGui::MenuItem("Audio Listener")) {
        to_select = scene_->create_entity("audio_listener", true).add<AudioListenerComponent>();
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Effects")) {
      if (ImGui::MenuItem("Particle System")) {
        to_select = scene_->create_entity("particle_system", true).add<ParticleSystemComponent>();
      }
      ImGui::EndMenu();
    }

    ImGui::EndMenu();
  }

  if (has_context && to_select != flecs::entity::null())
    to_select.child_of(selected_entity_.get());

  if (to_select != flecs::entity::null()) {
    selected_entity_.set(to_select);
    selected_script_ = nullptr;
  }
}

auto SceneHierarchyViewer::draw_scripts_context_menu() -> void {
  ZoneScoped;

  Asset selected = {};
  asset_manager_viewer.render("AssetManagerViewer", nullptr, AssetType::Script, &selected);

  if (selected.type == AssetType::Script) {
    auto* asset_man = App::get_asset_manager();
    if (!selected.is_loaded())
      asset_man->load_asset(selected.uuid);

    scene_->add_lua_system(selected.uuid);
    ImGui::CloseCurrentPopup();
  }
}

auto SceneHierarchyViewer::on_selected_entity_callback(const std::function<void(flecs::entity)> callback) -> void {
  ZoneScoped;
  selected_entity_.on_selected_entity_callback = callback;
}

auto SceneHierarchyViewer::on_selected_entity_reset_callback(const std::function<void()> callback) -> void {
  ZoneScoped;
  selected_entity_.on_selected_entity_reset_callback = callback;
}

auto SceneHierarchyViewer::set_scene(Scene* scene) -> void {
  ZoneScoped;
  scene_ = scene;
  selected_entity_.reset();
  selected_script_ = nullptr;
  renaming_entity_ = {};
  dragged_entity_ = {};
  dragged_entity_target_ = {};
  deleted_entity_ = {};
}

auto SceneHierarchyViewer::get_scene() -> Scene* {
  ZoneScoped;
  return scene_;
};
} // namespace ox
