#include "InspectorPanel.hpp"

#include <icons/IconsMaterialDesignIcons.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include "Asset/AssetFile.hpp"
#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Core/EventSystem.hpp"
#include "Core/FileSystem.hpp"
#include "Editor.hpp"
#include "EditorTheme.hpp"
#include "Scene/ECSModule/ComponentWrapper.hpp"
#include "UI/PayloadData.hpp"
#include "UI/UI.hpp"

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

InspectorPanel::InspectorPanel() : EditorPanel("Inspector", ICON_MDI_INFORMATION, true), scene_(nullptr) {
  viewer.search_icon = ICON_MDI_MAGNIFY;
  viewer.filter_icon = ICON_MDI_FILTER;

  auto& event_system = App::get_event_system();
  auto& asset_man = App::mod<AssetManager>();

  auto r1 = event_system.subscribe<DialogLoadEvent>([&asset_man](const DialogLoadEvent& e) {
    if (auto imported = asset_man.import_asset(e.path)) {
      if (e.asset_uuid) {
        if (*e.asset_uuid)
          asset_man.unload_asset(*e.asset_uuid);
        *e.asset_uuid = imported;
      }
    }
  });

  auto r2 = event_system.subscribe<DialogSaveEvent>([&asset_man](const DialogSaveEvent& e) {
    asset_man.export_asset(e.asset_uuid, e.path);
  });
}

void InspectorPanel::on_render(vuk::Extent3D extent, vuk::Format format) {
  auto& editor = App::mod<Editor>();
  auto& editor_context = editor.get_context();
  scene_ = editor.get_selected_scene();

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

        auto& asset_man = App::mod<AssetManager>();
        auto meta_file = asset_man.read_meta_file(path);
        if (!meta_file)
          return option<std::monostate>{};

        auto uuid_str_json = meta_file->doc["uuid"].get_string();
        if (uuid_str_json.error())
          return option<std::monostate>{};

        return UUID::from_string(uuid_str_json.value_unsafe()).and_then([this, &asset_man](UUID&& uuid) {
          if (auto* asset = asset_man.get_asset(uuid))
            this->draw_asset_info(asset);
          return option<std::monostate>{};
        });
      });
    });

  on_end();
}

void
InspectorPanel::draw_material_properties(Material* material, const UUID& material_uuid, std::string_view default_path) {
  if (material_uuid) {
    const auto& window = App::get()->get_window();
    static auto uuid_copy = material_uuid;

    auto uuid_str = fmt::format("UUID: {}", material_uuid.str());
    ImGui::TextUnformatted(uuid_str.c_str());

    auto load_str = fmt::format("{} Load", ICON_MDI_FILE_UPLOAD);

    const float x = ImGui::GetContentRegionAvail().x / 2;
    const float y = ImGui::GetFrameHeight();
    if (UI::button(load_str.c_str(), {x, y})) {
      FileDialogFilter dialog_filters[] = {{.name = "Asset (.oxasset)", .pattern = "oxasset"}};
      window.show_dialog({
        .kind = DialogKind::OpenFile,
        .user_data = nullptr,
        .callback =
          [](void* user_data, const c8* const* files, i32) {
            if (!files || !*files) {
              return;
            }

            const auto first_path_cstr = *files;
            const auto first_path_len = std::strlen(first_path_cstr);
            auto path = std::string(first_path_cstr, first_path_len);

            auto& event_system = App::get_event_system();
            auto r = event_system.emit(DialogLoadEvent{&uuid_copy, path});
          },
        .title = "Open material asset file...",
        .default_path = default_path,
        .filters = dialog_filters,
        .multi_select = false,
      });
    }
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* imgui_payload = ImGui::AcceptDragDropPayload(PayloadData::DRAG_DROP_SOURCE)) {
        const auto* payload = PayloadData::from_payload(imgui_payload);
        if (const std::string ext = fs::get_file_extension(payload->str); ext == "oxasset") {
          auto& event_system = App::get_event_system();
          auto r = event_system.emit(DialogLoadEvent{&uuid_copy, payload->str});
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

            auto& event_system = App::get_event_system();
            auto r = event_system.emit(DialogSaveEvent{uuid_copy, path});
            if (!r.has_value()) {
              OX_LOG_ERROR("{}", r.error().message());
            }
          },
        .title = "Open material asset file...",
        .default_path = default_path,
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

  const auto load_callback = [](const char* label, const UUID& uuid, bool& active) -> UUID {
    Asset selected = {};
    AssetType filter = AssetType::Texture;
    auto name = fmt::format("Asset Picker: {}", label);
    static AssetManagerViewer am;
    am.render(name.c_str(), &active, filter, &selected);

    if (selected.type == AssetType::Texture) {
      auto& asset_man = App::mod<AssetManager>();
      auto* existing_asset = asset_man.get_asset(uuid);
      const bool is_loaded = asset_man.load_asset(selected.uuid);
      if (is_loaded) {
        if (existing_asset) {
          asset_man.unload_asset(uuid);
        }
        return selected.uuid;
      }
    }

    return UUID(nullptr);
  };

  dirty |= UI::texture_property("Albedo", material->albedo_texture, load_callback);
  dirty |= UI::texture_property("Normal", material->normal_texture, load_callback);
  dirty |= UI::texture_property("Emissive", material->emissive_texture, load_callback);
  dirty |= UI::property_vector("Emissive Color", material->emissive_color, true, false);
  dirty |= UI::texture_property("Metallic Roughness", material->metallic_roughness_texture, load_callback);
  dirty |= UI::property("Roughness Factor", &material->roughness_factor, 0.0f, 1.0f);
  dirty |= UI::property("Metallic Factor", &material->metallic_factor, 0.0f, 1.0f);
  dirty |= UI::texture_property("Occlusion", material->occlusion_texture, load_callback);

  UI::end_properties();

  if (dirty) {
    auto& asset_man = App::mod<AssetManager>();
    if (const auto* asset = asset_man.get_asset(material_uuid))
      asset_man.set_material_dirty(asset->material_id);
  }
}

void InspectorPanel::draw_components(flecs::entity entity) {
  ZoneScoped;

  if (!entity)
    return;

  auto& editor = App::mod<Editor>();

  auto& undo_redo_system = editor.undo_redo_system;

  ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - (ImGui::CalcTextSize(ICON_MDI_PLUS).x + 20.0f));
  std::string new_name = entity.name().c_str();
  if (rename_entity_)
    ImGui::SetKeyboardFocusHere();
  UI::push_frame_style();
  if (ImGui::InputText("##Tag", &new_name, ImGuiInputTextFlags_EnterReturnsTrue)) {
    entity.set_name(new_name.c_str());
  }
  UI::pop_frame_style();
  ImGui::PopItemWidth();
  ImGui::SameLine();

  if (UI::button(ICON_MDI_PLUS)) {
    ImGui::OpenPopup("add_component");
  }

  const auto components = scene_->component_db.get_components();

  if (ImGui::BeginPopup("add_component")) {
    static ImGuiTextFilter add_component_filter = {};
    float filter_cursor_pos_x = ImGui::GetCursorPosX();

    if (ImGui::IsWindowAppearing()) {
      ImGui::SetKeyboardFocusHere();
    }
    add_component_filter.Draw("##scripts_filter_", ImGui::GetContentRegionAvail().x);
    if (!add_component_filter.IsActive()) {
      ImGui::SameLine();
      ImGui::SetCursorPosX(filter_cursor_pos_x + ImGui::GetFontSize() * 0.5f);
      auto search_txt = fmt::format("  {} Search components...", ICON_MDI_MAGNIFY);
      ImGui::TextUnformatted(search_txt.c_str());
    }

    for (auto& component : components) {
      auto component_entity = component.entity();
      auto component_name = component_entity.name();

      if (add_component_filter.IsActive() && !add_component_filter.PassFilter(component_name.c_str())) {
        continue;
      }

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

    auto& editor_theme = editor.editor_theme;

    const float line_height = editor_theme.regular_font_size + GImGui->Style.FramePadding.y * 2.0f;

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + line_height * 0.25f);

    auto component_entity = component.entity();
    auto component_name = component_entity.name();

    std::string
      name_str = fmt::format("{} {}:{}", ICON_MDI_VIEW_GRID, component_name.c_str(), (u64)component_entity.id());
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
                undo_redo_system->execute_command<PropertyChangeCommand<glm::vec2>>(v, old_v, *v, member_name.data());
            },
            [&](glm::vec3* v) {
              if (is_transform_component) {
                // Display rotation field of transform component as degrees instead of radians
                if (member_name == "rotation") {
                  glm::vec3 old_v = *v;
                  glm::vec3 rotation = glm::degrees(*v);
                  if (UI::draw_vec3_control(member_name.data(), rotation)) {
                    *v = glm::radians(rotation);
                    undo_redo_system
                      ->execute_command<PropertyChangeCommand<glm::vec3>>(v, old_v, *v, member_name.data());
                    entity.modified(component);
                  }
                } else {
                  glm::vec3 old_v = *v;
                  if (UI::draw_vec3_control(member_name.data(), *v)) {
                    undo_redo_system
                      ->execute_command<PropertyChangeCommand<glm::vec3>>(v, old_v, *v, member_name.data());
                    entity.modified(component);
                  }
                }
              } else {
                glm::vec3 old_v = *v;
                if (UI::property_vector(member_name.data(), *v))
                  undo_redo_system->execute_command<PropertyChangeCommand<glm::vec3>>(v, old_v, *v, member_name.data());
              }
            },
            [&](glm::vec4* v) {
              glm::vec4 old_v = *v;
              if (UI::property_vector(member_name.data(), *v)) {
                undo_redo_system->execute_command<PropertyChangeCommand<glm::vec4>>(v, old_v, *v, member_name.data());
                entity.modified(component);
              }
            },
            [&](glm::quat* v) { /* noop */ },
            [&](glm::mat4* v) { /* noop */ },
            [&](std::string* v) {
              std::string old_v = *v;
              if (UI::input_text(member_name.data(), v))
                undo_redo_system->execute_command<PropertyChangeCommand<std::string>>(v, old_v, *v, member_name.data());
            },
            [&](UUID* uuid) {
              UI::end_properties();

              ImGui::Separator();
              UI::begin_properties();
              auto uuid_str = uuid->str();
              UI::input_text(member_name.data(), &uuid_str, ImGuiInputTextFlags_ReadOnly);
              UI::end_properties();

              auto& asset_man = App::mod<AssetManager>();

              static bool draw_asset_picker = false;
              if (UI::button(ICON_MDI_CIRCLE_DOUBLE)) {
                draw_asset_picker = !draw_asset_picker;
              }

              if (draw_asset_picker) {
                Asset selected = {};
                AssetType filter = {};
                viewer.render("Asset Picker", &draw_asset_picker, filter, &selected);

                // NOTE: We don't allow model assets to be loaded this way yet(or ever).
                if (selected.type != AssetType::None && selected.type != AssetType::Model) {
                  // NOTE: Don't allow the existing asset to be swapped with a different type of asset.
                  auto* existing_asset = asset_man.get_asset(*uuid);
                  const bool is_same_asset = selected.uuid == *uuid;
                  const bool is_same_type = existing_asset->type == selected.type;
                  const bool is_loaded = asset_man.load_asset(selected.uuid);
                  if (!is_same_asset && is_same_type && is_loaded) {
                    if (*uuid) {
                      asset_man.unload_asset(*uuid);
                    }
                    *uuid = selected.uuid;
                    entity.modified(component);
                  }
                }
              }

              ImGui::SameLine();

              const float x = ImGui::GetContentRegionAvail().x;
              const float y = ImGui::GetFrameHeight();
              const auto btn = fmt::format("{} Drop an asset file", ICON_MDI_FILE_UPLOAD);
              if (UI::button(btn.c_str(), {x, y})) {
              }
              if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* imgui_payload = ImGui::AcceptDragDropPayload(PayloadData::DRAG_DROP_SOURCE)) {
                  const auto payload = PayloadData::from_payload(imgui_payload);
                  if (payload->get_str().empty())
                    return;
                  if (auto imported_asset = asset_man.import_asset(payload->str)) {
                    if (auto* existing_asset = asset_man.get_asset(*uuid)) {
                      asset_man.unload_asset(existing_asset->uuid);
                    }
                    if (asset_man.load_asset(imported_asset)) {
                      *uuid = imported_asset;
                      entity.modified(component);
                    }
                  }
                }
                ImGui::EndDragDropTarget();
              }
              ImGui::Spacing();
              ImGui::Separator();

              if (auto* asset = asset_man.get_asset(*uuid)) {
                switch (asset->type) {
                  case ox::AssetType::None: {
                    break;
                  }
                  case AssetType::Shader: {
                    draw_shader_asset(uuid, asset);
                    break;
                  }
                  case AssetType::Model: {
                    draw_model_asset(uuid, asset);
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
          member
        );

        UI::end_properties();
      });
      ImGui::TreePop();
    }

    if (remove_component)
      entity.remove(component);
  }
}

void InspectorPanel::draw_asset_info(Asset* asset) {
  ZoneScoped;
  auto& asset_man = App::mod<AssetManager>();
  auto type_str = asset_man.to_asset_type_sv(asset->type);
  auto uuid_str = asset->uuid.str();
  auto name = fs::get_name_with_extension(asset->path);

  ImGui::SeparatorText("Asset");
  ImGui::Indent();
  UI::begin_properties(ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit);
  UI::text("Type", type_str);
  UI::input_text("UUID", &uuid_str, ImGuiInputTextFlags_ReadOnly);
  UI::input_text("File", &name, ImGuiInputTextFlags_ReadOnly);
  UI::input_text("Path", &asset->path, ImGuiInputTextFlags_ReadOnly);
  UI::end_properties();

  if (asset->type == AssetType::Material) {
    if (auto* mat = asset_man.get_material(asset->uuid)) {
      ImGui::SeparatorText("Material");
      draw_material_properties(mat, asset->uuid, asset->path);
    }
  }
}

void InspectorPanel::draw_shader_asset(UUID* uuid, Asset* asset) {}

void InspectorPanel::draw_model_asset(UUID* uuid, Asset* asset) {
  ZoneScoped;

  auto& asset_man = App::mod<AssetManager>();
  if (auto* model = asset_man.get_model(*uuid)) {
    for (auto& mat_uuid : model->materials) {
      static constexpr ImGuiTreeNodeFlags TREE_FLAGS = ImGuiTreeNodeFlags_DefaultOpen |
                                                       ImGuiTreeNodeFlags_SpanAvailWidth |
                                                       ImGuiTreeNodeFlags_AllowItemOverlap | ImGuiTreeNodeFlags_Framed |
                                                       ImGuiTreeNodeFlags_FramePadding;

      if (auto* material = asset_man.get_material(mat_uuid)) {
        const auto mat_uuid_str = mat_uuid.str();
        if (ImGui::TreeNodeEx(mat_uuid_str.c_str(), TREE_FLAGS, "%s", mat_uuid_str.c_str())) {
          draw_material_properties(material, mat_uuid, asset->path);
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

  auto& asset_man = App::mod<AssetManager>();

  if (auto* material = asset_man.get_material(*uuid)) {
    draw_material_properties(material, *uuid, asset->path);
  }
}

void InspectorPanel::draw_font_asset(UUID* uuid, Asset* asset) {}

void InspectorPanel::draw_scene_asset(UUID* uuid, Asset* asset) {}

void InspectorPanel::draw_audio_asset(UUID* uuid, Asset* asset) {
  ZoneScoped;

  auto& asset_man = App::mod<AssetManager>();

  auto* audio_asset = asset_man.get_audio(*uuid);
  if (!audio_asset)
    return;

  auto& audio_engine = App::mod<AudioEngine>();

  ImGui::Spacing();
  if (UI::button(ICON_MDI_PLAY "Play "))
    audio_engine.play_source(audio_asset->get_source());
  ImGui::SameLine();
  if (UI::button(ICON_MDI_PAUSE "Pause "))
    audio_engine.pause_source(audio_asset->get_source());
  ImGui::SameLine();
  if (UI::button(ICON_MDI_STOP "Stop "))
    audio_engine.stop_source(audio_asset->get_source());
  ImGui::Spacing();
}

bool InspectorPanel::draw_script_asset(UUID* uuid, Asset* asset) {
  ZoneScoped;

  auto& asset_man = App::mod<AssetManager>();

  auto* script_asset = asset_man.get_script(*uuid);
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
      asset_man.unload_asset(*uuid);
    *uuid = UUID(nullptr);
  }

  return false;
}
} // namespace ox
