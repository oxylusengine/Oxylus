#include "InspectorPanel.hpp"

#include <icons/IconsMaterialDesignIcons.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include "Asset/AssetFile.hpp"
#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Core/FileSystem.hpp"
#include "EditorLayer.hpp"
#include "EditorTheme.hpp"
#include "EditorUI.hpp"
#include "Scene/ECSModule/ComponentWrapper.hpp"
#include "Utils/PayloadData.hpp"

namespace ox {
static f32 degree_helper(const char* id, f32 value) {
  f32 in_degrees = glm::degrees(value);
  f32 in_radians = value;

  if (ImGui::BeginPopupContextItem(id)) {
    UI::begin_properties();
    if (UI::property("Set in degrees", &in_degrees)) {
      in_radians = glm::radians(in_degrees);
    }
    UI::end_properties();
    ImGui::EndPopup();
  }

  return in_radians;
}

InspectorPanel::InspectorPanel() : EditorPanel("Inspector", ICON_MDI_INFORMATION, true), _scene(nullptr) {}

void InspectorPanel::on_render(vuk::Extent3D extent, vuk::Format format) {
  auto* editor_layer = EditorLayer::get();
  auto& editor_context = editor_layer->get_context();
  _scene = editor_layer->get_selected_scene().get();

  on_begin();

  editor_context.entity
      .and_then([this](flecs::entity e) {
        this->draw_components(e);
        return option<std::monostate>{};
      })
      .or_else([this, &editor_context]() {
        if (editor_context.type != EditorContext::Type::File)
          return option<std::monostate>{};

        return editor_context.str.and_then([this](const std::string& path) {
          if (fs::get_file_extension(path) != "oxasset")
            return option<std::monostate>{};

          auto* asset_man = App::get_asset_manager();
          auto meta_file = asset_man->read_meta_file(path);
          if (!meta_file)
            return option<std::monostate>{};

          auto uuid_str_json = meta_file->doc["uuid"].get_string();
          if (uuid_str_json.error())
            return option<std::monostate>{};

          return UUID::from_string(uuid_str_json.value_unsafe()).and_then([this, asset_man](UUID&& uuid) {
            if (auto* asset = asset_man->get_asset(uuid))
              this->draw_asset_info(asset);
            return option<std::monostate>{};
          });
        });
      });

  on_end();
}

void InspectorPanel::draw_material_properties(Material* material, const UUID& material_uuid, flecs::entity load_event) {
  if (material_uuid) {
    const auto& window = App::get()->get_window();
    static auto uuid_copy = material_uuid;
    static auto load_event_copy = load_event;

    auto uuid_str = fmt::format("UUID: {}", material_uuid.str());
    ImGui::TextUnformatted(uuid_str.c_str());

    auto load_str = fmt::format("{} Load", ICON_MDI_FILE_UPLOAD);

    const float x = ImGui::GetContentRegionAvail().x / 2;
    const float y = ImGui::GetFrameHeight();
    if (UI::button(load_str.c_str(), {x, y})) {
      FileDialogFilter dialog_filters[] = {{.name = "Asset (.oxasset)", .pattern = "oxasset"}};
      window.show_dialog({
          .kind = DialogKind::OpenFile,
          .user_data = &load_event,
          .callback =
              [](void* user_data, const c8* const* files, i32) {
                if (!files || !*files) {
                  return;
                }

                const auto first_path_cstr = *files;
                const auto first_path_len = std::strlen(first_path_cstr);
                auto path = std::string(first_path_cstr, first_path_len);

                load_event_copy.emit<DialogLoadEvent>({path});
              },
          .title = "Open material asset file...",
          .default_path = fs::current_path(),
          .filters = dialog_filters,
          .multi_select = false,
      });
    }
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* imgui_payload = ImGui::AcceptDragDropPayload(PayloadData::DRAG_DROP_SOURCE)) {
        const auto* payload = PayloadData::from_payload(imgui_payload);
        if (const std::string ext = ox::fs::get_file_extension(payload->str); ext == "oxasset") {
          load_event.emit<DialogLoadEvent>({payload->str});
        }
      }
      ImGui::EndDragDropTarget();
    }

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay)) {
      ImGui::BeginTooltip();
      ImGui::Text("You can drag&drop here to load a material.");
      ImGui::EndTooltip();
    }

    ImGui::SameLine();

    auto save_str = fmt::format("{} Save", ICON_MDI_FILE_DOWNLOAD);
    if (UI::button(save_str.c_str(), {x, y})) {
      FileDialogFilter dialog_filters[] = {{.name = "Asset (.oxasset)", .pattern = "oxasset"}};
      window.show_dialog({
          .kind = DialogKind::SaveFile,
          .user_data = nullptr,
          .callback =
              [](void* user_data, const c8* const* files, i32) {
                if (!files || !*files || !uuid_copy) {
                  return;
                }

                const auto first_path_cstr = *files;
                const auto first_path_len = std::strlen(first_path_cstr);
                auto path = std::string(first_path_cstr, first_path_len);

                load_event_copy.emit<DialogSaveEvent>({path});
              },
          .title = "Open material asset file...",
          .default_path = fs::current_path(),
          .filters = dialog_filters,
          .multi_select = false,
      });
    }

    if (ImGui::BeginDragDropSource()) {
      std::string path_str = fmt::format("new_material");
      auto payload = PayloadData(path_str, material_uuid);
      ImGui::SetDragDropPayload(PayloadData::DRAG_DROP_TARGET, &payload, payload.size());
      ImGui::TextUnformatted(path_str.c_str());
      ImGui::EndDragDropSource();
    }

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay)) {
      ImGui::BeginTooltip();
      ImGui::Text("You can drag&drop this into content window to save the material.");
      ImGui::EndTooltip();
    }
  }

  bool dirty = false;

  UI::begin_properties(UI::default_properties_flags);

  const char* alpha_modes[] = {"Opaque", "Mash", "Blend"};
  dirty |= UI::property("Alpha mode", reinterpret_cast<int*>(&material->alpha_mode), alpha_modes, 3);

  const char* samplers[] = {
      "LinearRepeated",
      "LinearClamped",
      "NearestRepeated",
      "NearestClamped",
      "LinearRepeatedAnisotropy",
  };
  dirty |= UI::property("Sampler", reinterpret_cast<int*>(&material->sampling_mode), samplers, 5);

  dirty |= UI::property_vector<glm::vec2>("UV Size", material->uv_size, false, false, nullptr, 0.1f, 0.1f, 10.f);
  dirty |= UI::property_vector<glm::vec2>("UV Offset", material->uv_offset, false, false, nullptr, 0.1f, -10.f, 10.f);

  dirty |= UI::property_vector("Color", material->albedo_color, true, true);

  // NOTE: These properties leak when the old textures are discared.
  // Since in editor you can manually free the old loaded textures
  // for fast iterations I'll leave this behaviour.
  dirty |= UI::texture_property("Albedo", material->albedo_texture);
  dirty |= UI::texture_property("Normal", material->normal_texture);
  dirty |= UI::texture_property("Emissive", material->emissive_texture);
  dirty |= UI::property_vector("Emissive Color", material->emissive_color, true, false);
  dirty |= UI::texture_property("Metallic Roughness", material->metallic_roughness_texture);
  dirty |= UI::property("Roughness Factor", &material->roughness_factor, 0.0f, 1.0f);
  dirty |= UI::property("Metallic Factor", &material->metallic_factor, 0.0f, 1.0f);
  dirty |= UI::texture_property("Occlusion", material->occlusion_texture);

  UI::end_properties();

  if (dirty) {
    auto* asset_man = App::get_asset_manager();
    if (const auto* asset = asset_man->get_asset(material_uuid))
      asset_man->set_material_dirty(asset->material_id);
  }
}

void InspectorPanel::draw_components(flecs::entity entity) {
  ZoneScoped;

  if (!entity)
    return;

  auto& undo_redo_system = EditorLayer::get()->undo_redo_system;

  ImGui::PushItemWidth(ImGui::GetWindowWidth() * 0.9f);
  std::string new_name = entity.name().c_str();
  if (_rename_entity)
    ImGui::SetKeyboardFocusHere();
  UI::push_frame_style();
  if (ImGui::InputText("##Tag", &new_name, ImGuiInputTextFlags_EnterReturnsTrue)) {
    entity.set_name(new_name.c_str());
  }
  UI::pop_frame_style();
  ImGui::PopItemWidth();
  ImGui::SameLine();

  if (UI::button(ICON_MDI_PLUS)) {
    ImGui::OpenPopup("Add Component");
  }

  const auto components = _scene->component_db.get_components();

  if (ImGui::BeginPopup("Add Component")) {
    for (auto& component : components) {
      auto component_entity = component.entity();
      auto component_name = component_entity.name();

      if (ImGui::MenuItem(component_name)) {
        if (entity.has(component))
          OX_LOG_WARN("Entity already has same component!");
        else
          entity.add(component);
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }

  for (auto& component : components) {
    auto* entity_component = entity.try_get_mut(component);
    if (!entity_component) {
      continue;
    }
    static constexpr ImGuiTreeNodeFlags TREE_FLAGS = ImGuiTreeNodeFlags_DefaultOpen |
                                                     ImGuiTreeNodeFlags_SpanAvailWidth |
                                                     ImGuiTreeNodeFlags_AllowItemOverlap | ImGuiTreeNodeFlags_Framed |
                                                     ImGuiTreeNodeFlags_FramePadding;

    auto& editor_theme = EditorLayer::get()->editor_theme;

    const float line_height = editor_theme.regular_font_size + GImGui->Style.FramePadding.y * 2.0f;

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + line_height * 0.25f);

    auto component_entity = component.entity();
    auto component_name = component_entity.name();

    std::string name_str = fmt::format(
        "{} {}:{}", ICON_MDI_VIEW_GRID, component_name.c_str(), (u64)component_entity.id());
    const bool open = ImGui::TreeNodeEx(name_str.c_str(), TREE_FLAGS, "%s", name_str.c_str());

    bool remove_component = false;

    ImGui::PushID(name_str.c_str());
    const float frame_height = ImGui::GetFrameHeight();
    ImGui::SameLine(ImGui::GetContentRegionMax().x - frame_height * 1.2f);
    if (UI::button(ICON_MDI_COG, ImVec2{frame_height * 1.2f, frame_height}))
      ImGui::OpenPopup("ComponentSettings");

    if (ImGui::BeginPopup("ComponentSettings")) {
      if (ImGui::MenuItem("Remove Component"))
        remove_component = true;
      if (ImGui::MenuItem("Reset Component"))
        entity.remove(component).add(component);
      ImGui::EndPopup();
    }
    ImGui::PopID();

    if (open) {
      ECS::ComponentWrapper component_wrapped(entity, component);

      component_wrapped.for_each([&](usize&, std::string_view member_name, ECS::ComponentWrapper::Member& member) {
        ImGuiTableFlags properties_flags = UI::default_properties_flags;

        // Special case for Transform Component
        const auto is_transform_component = component_name == "TransformComponent";
        if (is_transform_component)
          properties_flags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV;

        UI::begin_properties(properties_flags);

        std::visit(
            ox::match{
                [](const auto&) {},
                [&](bool* v) {
                  bool old_v = *v;
                  if (UI::property(member_name.data(), v)) {
                    undo_redo_system //
                        ->set_merge_enabled(false)
                        .execute_command<PropertyChangeCommand<bool>>(v, old_v, *v, member_name.data())
                        .set_merge_enabled(true);
                  }
                },
                [&](u16* v) {
                  u16 old_v = *v;
                  if (UI::property(member_name.data(), v))
                    undo_redo_system->execute_command<PropertyChangeCommand<u16>>(v, old_v, *v, member_name.data());
                },
                [&](f32* v) {
                  f32 old_v = *v;
                  if (UI::property(member_name.data(), v))
                    undo_redo_system->execute_command<PropertyChangeCommand<f32>>(v, old_v, *v, member_name.data());
                  *v = degree_helper(member_name.data(), *v);
                },
                [&](f64* v) {
                  f64 old_v = *v;
                  if (UI::property(member_name.data(), v))
                    undo_redo_system->execute_command<PropertyChangeCommand<f64>>(v, old_v, *v, member_name.data());
                  *v = degree_helper(member_name.data(), *v);
                },
                [&](i32* v) {
                  i32 old_v = *v;
                  if (UI::property(member_name.data(), v))
                    undo_redo_system->execute_command<PropertyChangeCommand<i32>>(v, old_v, *v, member_name.data());
                },
                [&](u32* v) {
                  u32 old_v = *v;
                  if (UI::property(member_name.data(), v))
                    undo_redo_system->execute_command<PropertyChangeCommand<u32>>(v, old_v, *v, member_name.data());
                },
                [&](i64* v) {
                  i64 old_v = *v;
                  if (UI::property(member_name.data(), v))
                    undo_redo_system->execute_command<PropertyChangeCommand<i64>>(v, old_v, *v, member_name.data());
                },
                [&](u64* v) {
                  u64 old_v = *v;
                  if (UI::property(member_name.data(), v))
                    undo_redo_system->execute_command<PropertyChangeCommand<u64>>(v, old_v, *v, member_name.data());
                },
                [&](glm::vec2* v) {
                  glm::vec2 old_v = *v;
                  if (UI::property_vector(member_name.data(), *v))
                    undo_redo_system->execute_command<PropertyChangeCommand<glm::vec2>>(
                        v, old_v, *v, member_name.data());
                },
                [&](glm::vec3* v) {
                  if (is_transform_component) {
                    // Display rotation field of transform component as degrees instead of radians
                    if (member_name == "rotation") {
                      glm::vec3 old_v = *v;
                      glm::vec3 rotation = glm::degrees(*v);
                      if (UI::draw_vec3_control(member_name.data(), rotation)) {
                        *v = glm::radians(rotation);
                        undo_redo_system->execute_command<PropertyChangeCommand<glm::vec3>>(
                            v, old_v, *v, member_name.data());
                        entity.modified(component);
                      }
                    } else {
                      glm::vec3 old_v = *v;
                      if (UI::draw_vec3_control(member_name.data(), *v)) {
                        undo_redo_system->execute_command<PropertyChangeCommand<glm::vec3>>(
                            v, old_v, *v, member_name.data());
                        entity.modified(component);
                      }
                    }
                  } else {
                    glm::vec3 old_v = *v;
                    if (UI::property_vector(member_name.data(), *v))
                      undo_redo_system->execute_command<PropertyChangeCommand<glm::vec3>>(
                          v, old_v, *v, member_name.data());
                  }
                },
                [&](glm::vec4* v) {
                  glm::vec4 old_v = *v;
                  if (UI::property_vector(member_name.data(), *v)) {
                    undo_redo_system->execute_command<PropertyChangeCommand<glm::vec4>>(
                        v, old_v, *v, member_name.data());
                    entity.modified(component);
                  }
                },
                [&](glm::quat* v) { /* noop */ },
                [&](glm::mat4* v) { /* noop */ },
                [&](std::string* v) {
                  std::string old_v = *v;
                  if (UI::input_text(member_name.data(), v))
                    undo_redo_system->execute_command<PropertyChangeCommand<std::string>>(
                        v, old_v, *v, member_name.data());
                },
                [&](UUID* uuid) {
                  UI::end_properties();

                  ImGui::Separator();
                  UI::begin_properties();
                  auto uuid_str = uuid->str();
                  UI::input_text(member_name.data(), &uuid_str, ImGuiInputTextFlags_ReadOnly);
                  UI::end_properties();

                  auto* asset_man = App::get_asset_manager();

                  static bool draw_asset_picker = false;
                  if (UI::button(ICON_MDI_CIRCLE_DOUBLE)) {
                    draw_asset_picker = !draw_asset_picker;
                  }

                  if (draw_asset_picker) {
                    ImGui::SetNextWindowSize(
                        ImVec2(ImGui::GetMainViewport()->Size.x / 2, ImGui::GetMainViewport()->Size.y / 2),
                        ImGuiCond_Appearing);
                    UI::center_next_window(ImGuiCond_Appearing);
                    if (ImGui::Begin("Asset Picker", &draw_asset_picker)) {
                      static ankerl::unordered_dense::map<AssetType, bool> asset_type_flags = {
                          {AssetType::Shader, true},
                          {AssetType::Texture, true},
                          {AssetType::Material, true},
                          {AssetType::Font, true},
                          {AssetType::Scene, true},
                          {AssetType::Audio, true},
                          {AssetType::Script, true},
                      };

                      ImGui::Text("Imported Assets");

                      if (ImGui::Button(ICON_MDI_FILTER)) {
                        ImGui::OpenPopup("asset_picker_filter");
                      }
                      if (ImGui::BeginPopup("asset_picker_filter")) {
                        if (ImGui::Button("Select All")) {
                          for (auto&& [type, flag] : asset_type_flags) {
                            flag = true;
                          }
                        }

                        ImGui::SameLine();

                        if (ImGui::Button("Deselect All")) {
                          for (auto&& [type, flag] : asset_type_flags) {
                            flag = false;
                          }
                        }

                        UI::begin_properties();

                        for (auto&& [type, flag] : asset_type_flags) {
                          UI::property(asset_man->to_asset_type_sv(type).data(), &flag);
                        }

                        UI::end_properties();
                        ImGui::EndPopup();
                      }

                      ImGui::SameLine();

                      static ImGuiTextFilter filter = {};
                      const float filter_cursor_pos_x = ImGui::GetCursorPosX();
                      filter.Draw("##asset_filter",
                                  ImGui::GetContentRegionAvail().x - (ImGui::CalcTextSize(ICON_MDI_PLUS, "").x +
                                                                      2.0f * ImGui::GetStyle().FramePadding.x));
                      if (!filter.IsActive()) {
                        ImGui::SameLine();
                        ImGui::SetCursorPosX(filter_cursor_pos_x + ImGui::GetFontSize() * 0.5f);
                        ImGui::TextUnformatted(ICON_MDI_MAGNIFY " Search...");
                      }

                      constexpr ImGuiTableFlags TABLE_FLAGS = ImGuiTableFlags_Resizable | ImGuiTableFlags_Hideable |
                                                              ImGuiTableFlags_Borders |
                                                              ImGuiTableFlags_SizingStretchProp;

                      if (ImGui::BeginChild("##assets_table_window")) {
                        if (ImGui::BeginTable("#assets_table", 3, TABLE_FLAGS)) {
                          const auto& registry = asset_man->registry();
                          for (const auto& asset : registry | std::views::values) {
                            const auto asset_uuid_str = asset.uuid.str();
                            const auto file_name = fs::get_name_with_extension(asset.path);
                            const auto asset_type = asset_man->to_asset_type_sv(asset.type);
                            // NOTE: We don't allow mesh assets to be loaded this way yet(or ever).
                            if (asset.type == AssetType::Mesh) {
                              continue;
                            }
                            if (!asset_type_flags[asset.type]) {
                              continue;
                            }
                            if (!file_name.empty() && !filter.PassFilter(file_name.c_str())) {
                              continue;
                            }

                            ImGui::TableNextRow(ImGuiTableRowFlags_None);

                            ImGui::TableSetColumnIndex(0);
                            ImGui::PushID(asset_uuid_str.c_str());
                            constexpr ImGuiSelectableFlags selectable_flags = ImGuiSelectableFlags_SpanAllColumns |
                                                                              ImGuiSelectableFlags_AllowOverlap |
                                                                              ImGuiSelectableFlags_AllowDoubleClick;
                            if (ImGui::Selectable(asset_type.data(), false, selectable_flags, ImVec2(0.f, 20.f))) {
                              draw_asset_picker = false;
                              // NOTE: Don't allow the existing asset to be swapped with a different type of asset.
                              auto* existing_asset = asset_man->get_asset(*uuid);
                              if (asset.uuid != *uuid && //
                                  (!existing_asset || existing_asset->type == asset.type) &&
                                  asset_man->load_asset(asset.uuid)) {
                                if (*uuid) {
                                  asset_man->unload_asset(*uuid);
                                }
                                *uuid = asset.uuid;
                                entity.modified(component);
                              }
                            }
                            ImGui::PopID();

                            ImGui::TableSetColumnIndex(1);
                            ImGui::TextUnformatted(file_name.c_str());

                            ImGui::TableSetColumnIndex(2);
                            ImGui::TextUnformatted(uuid_str.c_str());
                          }
                          ImGui::EndTable();
                        }
                      }
                      ImGui::EndChild();
                    }
                    ImGui::End();
                  }

                  ImGui::SameLine();

                  const float x = ImGui::GetContentRegionAvail().x;
                  const float y = ImGui::GetFrameHeight();
                  const auto btn = fmt::format("{} Drop an asset file", ICON_MDI_FILE_UPLOAD);
                  if (UI::button(btn.c_str(), {x, y})) {
                  }
                  if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* imgui_payload = ImGui::AcceptDragDropPayload(
                            PayloadData::DRAG_DROP_SOURCE)) {
                      const auto payload = PayloadData::from_payload(imgui_payload);
                      if (payload->get_str().empty())
                        return;
                      if (auto imported_asset = asset_man->import_asset(payload->str)) {
                        if (auto* existing_asset = asset_man->get_asset(*uuid)) {
                          asset_man->unload_asset(existing_asset->uuid);
                        }
                        if (asset_man->load_asset(imported_asset)) {
                          *uuid = imported_asset;
                          entity.modified(component);
                        }
                      }
                    }
                    ImGui::EndDragDropTarget();
                  }
                  ImGui::Spacing();
                  ImGui::Separator();

                  if (auto* asset = asset_man->get_asset(*uuid)) {
                    switch (asset->type) {
                      case ox::AssetType::None: {
                        break;
                      }
                      case AssetType::Shader: {
                        draw_shader_asset(uuid, asset);
                        break;
                      }
                      case AssetType::Mesh: {
                        draw_mesh_asset(uuid, asset);
                        break;
                      }
                      case AssetType::Texture: {
                        draw_texture_asset(uuid, asset);
                        break;
                      }
                      case AssetType::Material: {
                        draw_material_asset(uuid, asset);
                        break;
                      }
                      case AssetType::Font: {
                        draw_font_asset(uuid, asset);
                        break;
                      }
                      case AssetType::Scene: {
                        draw_scene_asset(uuid, asset);
                        break;
                      }
                      case AssetType::Audio: {
                        draw_audio_asset(uuid, asset);
                        break;
                      }
                      case AssetType::Script: {
                        if (draw_script_asset(uuid, asset))
                          entity.modified(component);
                        break;
                      }
                    }
                  }

                  UI::begin_properties();
                },
            },
            member);

        UI::end_properties();
      });
      ImGui::TreePop();
    }

    if (remove_component)
      entity.remove(component);
  }
#if 0

  draw_component<RigidbodyComponent>(
      " Rigidbody Component", entity, [](RigidbodyComponent& component, flecs::entity e) {
        UI::begin_properties();

        const char* dofs_strings[] = {
            "None",
            "All",
            "Plane2D",
            "Custom",
        };
        int current_dof_selection = 3;
        switch (component.allowed_dofs) {
          case RigidbodyComponent::AllowedDOFs::None   : current_dof_selection = 0; break;
          case RigidbodyComponent::AllowedDOFs::All    : current_dof_selection = 1; break;
          case RigidbodyComponent::AllowedDOFs::Plane2D: current_dof_selection = 2; break;
          default                                      : current_dof_selection = 3; break;
        }

        if (UI::property("Allowed degree of freedom", &current_dof_selection, dofs_strings, std::size(dofs_strings))) {
          switch (current_dof_selection) {
            case 0: component.allowed_dofs = RigidbodyComponent::AllowedDOFs::None; break;
            case 1: component.allowed_dofs = RigidbodyComponent::AllowedDOFs::All; break;
            case 2: component.allowed_dofs = RigidbodyComponent::AllowedDOFs::Plane2D; break;
          }
        }

        ImGui::Indent();
        UI::begin_property_grid("Allowed positions", nullptr);
        ImGui::CheckboxFlags("x", (u32*)&component.allowed_dofs, (u32)RigidbodyComponent::AllowedDOFs::TranslationX);
        ImGui::SameLine();
        ImGui::CheckboxFlags("y", (u32*)&component.allowed_dofs, (u32)RigidbodyComponent::AllowedDOFs::TranslationY);
        ImGui::SameLine();
        ImGui::CheckboxFlags("z", (u32*)&component.allowed_dofs, (u32)RigidbodyComponent::AllowedDOFs::TranslationZ);
        UI::end_property_grid();

        UI::begin_property_grid("Allowed rotations", nullptr);
        ImGui::CheckboxFlags("x", (u32*)&component.allowed_dofs, (u32)RigidbodyComponent::AllowedDOFs::RotationX);
        ImGui::SameLine();
        ImGui::CheckboxFlags("y", (u32*)&component.allowed_dofs, (u32)RigidbodyComponent::AllowedDOFs::RotationY);
        ImGui::SameLine();
        ImGui::CheckboxFlags("z", (u32*)&component.allowed_dofs, (u32)RigidbodyComponent::AllowedDOFs::RotationZ);
        UI::end_property_grid();
        ImGui::Unindent();

        const char* body_type_strings[] = {"Static", "Kinematic", "Dynamic"};
        int body_type = static_cast<int>(component.type);
        if (UI::property("Body Type", &body_type, body_type_strings, 3))
          component.type = static_cast<RigidbodyComponent::BodyType>(body_type);

        UI::property("Allow Sleep", &component.allow_sleep);
        UI::property("Awake", &component.awake);
        if (component.type == RigidbodyComponent::BodyType::Dynamic) {
          UI::property("Mass", &component.mass, 0.01f, 10000.0f);
          UI::property("Linear Drag", &component.linear_drag);
          UI::property("Angular Drag", &component.angular_drag);
          UI::property("Gravity Scale", &component.gravity_scale);
          UI::property("Continuous", &component.continuous);
          UI::property("Interpolation", &component.interpolation);

          component.linear_drag = glm::max(component.linear_drag, 0.0f);
          component.angular_drag = glm::max(component.angular_drag, 0.0f);
        }

        UI::property("Is Sensor", &component.is_sensor);
        UI::end_properties();
      });

#endif
}

void InspectorPanel::draw_asset_info(Asset* asset) {
  ZoneScoped;
  auto* asset_man = App::get_asset_manager();
  auto type_str = asset_man->to_asset_type_sv(asset->type);
  auto uuid_str = asset->uuid.str();

  ImGui::SeparatorText("Asset");
  ImGui::Indent();
  UI::begin_properties(ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit);
  UI::text("Asset Type", type_str);
  UI::input_text("Asset UUID", &uuid_str, ImGuiInputTextFlags_ReadOnly);
  UI::input_text("Asset Path", &asset->path, ImGuiInputTextFlags_ReadOnly);
  UI::end_properties();
}

void InspectorPanel::draw_shader_asset(UUID* uuid, Asset* asset) {}

void InspectorPanel::draw_mesh_asset(UUID* uuid, Asset* asset) {
  ZoneScoped;

  auto load_event = _scene->world.entity("ox_mesh_material_load_event");
  auto* asset_man = App::get_asset_manager();
  if (auto* mesh = asset_man->get_mesh(*uuid)) {
    for (auto& mat_uuid : mesh->materials) {
      static constexpr ImGuiTreeNodeFlags TREE_FLAGS = ImGuiTreeNodeFlags_DefaultOpen |
                                                       ImGuiTreeNodeFlags_SpanAvailWidth |
                                                       ImGuiTreeNodeFlags_AllowItemOverlap | ImGuiTreeNodeFlags_Framed |
                                                       ImGuiTreeNodeFlags_FramePadding;

      if (auto* material = asset_man->get_material(mat_uuid)) {
        const auto mat_uuid_str = mat_uuid.str();
        if (ImGui::TreeNodeEx(mat_uuid_str.c_str(), TREE_FLAGS, "%s", mat_uuid_str.c_str())) {
          draw_material_properties(material, mat_uuid, load_event);
          ImGui::TreePop();
        }
      }
    }
  }
}

void InspectorPanel::draw_texture_asset(UUID* uuid, Asset* asset) {}

void InspectorPanel::draw_material_asset(UUID* uuid, Asset* asset) {
  ZoneScoped;

  ImGui::SeparatorText("Material");

  auto load_event = _scene->world.entity("sprite_material_load_event");
  load_event.observe<DialogLoadEvent>([uuid](DialogLoadEvent& en) {
    auto* asset_man = App::get_asset_manager();
    if (auto imported = asset_man->import_asset(en.path)) {
      *uuid = imported;
      asset_man->unload_asset(*uuid);
    }
  });

  load_event.observe<DialogSaveEvent>([uuid](DialogSaveEvent& en) {
    auto* asset_man = App::get_asset_manager();
    asset_man->export_asset(*uuid, en.path);
  });

  if (auto* material = App::get_asset_manager()->get_material(*uuid)) {
    draw_material_properties(material, *uuid, load_event);
  }
}

void InspectorPanel::draw_font_asset(UUID* uuid, Asset* asset) {}

void InspectorPanel::draw_scene_asset(UUID* uuid, Asset* asset) {}

void InspectorPanel::draw_audio_asset(UUID* uuid, Asset* asset) {
  ZoneScoped;

  auto* asset_man = App::get_asset_manager();

  auto* audio_asset = asset_man->get_audio(*uuid);
  if (!audio_asset)
    return;

  auto* audio_engine = App::get_system<AudioEngine>(EngineSystems::AudioEngine);

  ImGui::Spacing();
  if (UI::button(ICON_MDI_PLAY "Play "))
    audio_engine->play_source(audio_asset->get_source());
  ImGui::SameLine();
  if (UI::button(ICON_MDI_PAUSE "Pause "))
    audio_engine->pause_source(audio_asset->get_source());
  ImGui::SameLine();
  if (UI::button(ICON_MDI_STOP "Stop "))
    audio_engine->stop_source(audio_asset->get_source());
  ImGui::Spacing();
}

bool InspectorPanel::draw_script_asset(UUID* uuid, Asset* asset) {
  ZoneScoped;

  auto* asset_man = App::get_asset_manager();

  auto* script_asset = asset_man->get_script(*uuid);
  if (!script_asset)
    return false;

  auto script_path = script_asset->get_path();
  UI::begin_properties(ImGuiTableFlags_SizingFixedFit);
  UI::text("File Name:", fs::get_file_name(script_path));
  UI::input_text("Path:", &script_path, ImGuiInputTextFlags_ReadOnly);
  UI::end_properties();
  auto rld_str = fmt::format("{} Reload", ICON_MDI_REFRESH);
  if (UI::button(rld_str.c_str())) {
    script_asset->reload();
    return true;
  }
  ImGui::SameLine();
  auto rmv_str = fmt::format("{} Remove", ICON_MDI_TRASH_CAN);
  if (UI::button(rmv_str.c_str())) {
    if (uuid)
      asset_man->unload_asset(*uuid);
    *uuid = UUID(nullptr);
  }

  return false;
}
} // namespace ox
