#include "InspectorPanel.hpp"

#include <ankerl/svector.h>
#include <icons/IconsMaterialDesignIcons.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include "AnimationEditorPanel.hpp"
#include "Asset/AssetFile.hpp"
#include "Asset/AssetManager.hpp"
#include "CinematicEditorPanel.hpp"
#include "Core/App.hpp"
#include "Core/EventSystem.hpp"
#include "Editor.hpp"
#include "Memory/Stack.hpp"
#include "ParticleEditorPanel.hpp"
#include "Scene/EntitySerializer.hpp"
#include "UI/PayloadData.hpp"
#include "UI/UI.hpp"
#include "Utils/AnimationAssets.hpp"
#include "Utils/EditorTheme.hpp"

namespace ox {
static UUID pending_save_material_uuid = {};

struct EntityInspector : IEntitySerializer {
  UndoRedoSystem& undo_redo_system;
  InspectorPanel& inspector_panel;
  bool modified;

  EntityInspector(flecs::world& world_, UndoRedoSystem& undo_redo_system_, InspectorPanel& inspector_panel_)
      : IEntitySerializer(world_),
        undo_redo_system(undo_redo_system_),
        inspector_panel(inspector_panel_),
        modified(false) {}

  auto on_primitive(std::string_view name, Primitive primitive) -> void override {
    std::visit(
      ox::match{
        [](const auto&) {},
        [&](bool* v) {
          auto old_v = *v;
          if (UI::property(name.data(), v)) {
            undo_redo_system //
              .set_merge_enabled(false)
              .execute_command<PropertyChangeCommand<bool>>(v, old_v, *v, std::string(name))
              .set_merge_enabled(true);
            modified = true;
          }
        },
        [&](c8* v) {
          auto old_v = *v;
          if (UI::property(name.data(), v)) {
            undo_redo_system.execute_command<PropertyChangeCommand<c8>>(v, old_v, *v, std::string(name));
            modified = true;
          }
        },
        [&](i8* v) {
          auto old_v = *v;
          if (UI::property(name.data(), v)) {
            undo_redo_system.execute_command<PropertyChangeCommand<i8>>(v, old_v, *v, std::string(name));
            modified = true;
          }
        },
        [&](u8* v) {
          auto old_v = *v;
          if (UI::property(name.data(), v)) {
            undo_redo_system.execute_command<PropertyChangeCommand<u8>>(v, old_v, *v, std::string(name));
            modified = true;
          }
        },
        [&](i16* v) {
          auto old_v = *v;
          if (UI::property(name.data(), v)) {
            undo_redo_system.execute_command<PropertyChangeCommand<i16>>(v, old_v, *v, std::string(name));
            modified = true;
          }
        },
        [&](u16* v) {
          auto old_v = *v;
          if (UI::property(name.data(), v)) {
            undo_redo_system.execute_command<PropertyChangeCommand<u16>>(v, old_v, *v, std::string(name));
            modified = true;
          }
        },
        [&](i32* v) {
          auto old_v = *v;
          if (UI::property(name.data(), v)) {
            undo_redo_system.execute_command<PropertyChangeCommand<i32>>(v, old_v, *v, std::string(name));
            modified = true;
          }
        },
        [&](u32* v) {
          auto old_v = *v;
          if (UI::property(name.data(), v)) {
            undo_redo_system.execute_command<PropertyChangeCommand<u32>>(v, old_v, *v, std::string(name));
            modified = true;
          }
        },
        [&](i64* v) {
          auto old_v = *v;
          if (UI::property(name.data(), v)) {
            undo_redo_system.execute_command<PropertyChangeCommand<i64>>(v, old_v, *v, std::string(name));
            modified = true;
          }
        },
        [&](u64* v) {
          auto old_v = *v;
          if (UI::property(name.data(), v)) {
            undo_redo_system.execute_command<PropertyChangeCommand<u64>>(v, old_v, *v, std::string(name));
            modified = true;
          }
        },
        [&](f32* v) {
          auto old_v = *v;
          if (UI::property(name.data(), v)) {
            undo_redo_system.execute_command<PropertyChangeCommand<f32>>(v, old_v, *v, std::string(name));
            modified = true;
          }
        },
        [&](f64* v) {
          auto old_v = *v;
          if (UI::property(name.data(), v)) {
            undo_redo_system.execute_command<PropertyChangeCommand<f64>>(v, old_v, *v, std::string(name));
            modified = true;
          }
        },
      },
      primitive
    );
  }

  auto on_string(std::string_view name, const c8** str) -> void override {}

  auto on_enum(std::string_view name, ecs_meta_op_kind_t underlying_kind, flecs::entity_t type, void* ptr)
    -> void override {
    UI::begin_property_grid(name.data(), nullptr);

    auto field_names = std::vector<const char*>{};
    world.entity(type).children([&](flecs::entity e) { field_names.push_back(e.name()); });
    auto current = static_cast<i32*>(ptr);
    ImGui::Combo("##enum_field", current, field_names.data(), static_cast<int>(field_names.size()));
    UI::end_property_grid();
  }

  auto on_entity(std::string_view name, flecs::entity* entity) -> void override {}

  auto on_component(std::string_view name, flecs::id_t* component) -> void override {}

  auto on_struct(std::string_view name, flecs::meta::op_t* ops, i32 op_count, void* base) -> void override {
    if (!name.empty()) {
      if (ops->type == world.entity<glm::vec2>()) {
        auto* v = static_cast<glm::vec2*>(base);
        auto old_v = *v;
        if (UI::draw_vec2_control(name.data(), *v)) {
          undo_redo_system.execute_command<PropertyChangeCommand<glm::vec2>>(v, old_v, *v, std::string(name));
          modified = true;
        }
      } else if (ops->type == world.entity<glm::vec3>()) {
        auto* v = static_cast<glm::vec3*>(base);
        auto old_v = *v;
        if (UI::draw_vec3_control(name.data(), *v)) {
          undo_redo_system.execute_command<PropertyChangeCommand<glm::vec3>>(v, old_v, *v, std::string(name));
          modified = true;
        }
      } else if (ops->type == world.entity<glm::vec4>()) {
        auto* v = static_cast<glm::vec4*>(base);
        auto old_v = *v;
        if (UI::property_vector(name.data(), *v)) {
          undo_redo_system.execute_command<PropertyChangeCommand<glm::vec4>>(v, old_v, *v, std::string(name));
          modified = true;
        }
      } else if (ops->type == world.entity<glm::quat>()) {
        auto* v = static_cast<glm::quat*>(base);

        if (!inspector_panel.euler_cache.has_value()) {
          inspector_panel.euler_cache = glm::degrees(glm::eulerAngles(*v));
        }

        auto old_v = *v;
        if (UI::draw_vec3_control(name.data(), *inspector_panel.euler_cache)) {
          auto old_v_cmd = *v;
          *v = glm::quat(glm::radians(inspector_panel.euler_cache.value()));
          undo_redo_system.execute_command<PropertyChangeCommand<glm::quat>>(v, old_v_cmd, *v, std::string(name));

          modified = true;
        }
      } else {
        serialize_ops(ops + 1, op_count - 1, base);
      }
    } else {
      // root level serialization
      serialize_ops(ops + 1, op_count - 1, base);
    }
  }

  auto on_opaque_value(
    std::string_view name, flecs::entity_t field_type, void* field_ptr, flecs::entity_t opaque_type, const void* value
  ) -> void override {
    if (field_type == world.entity<UUID>()) {
      auto* uuid = static_cast<UUID*>(field_ptr);
      UI::end_properties();

      ImGui::Separator();
      UI::begin_properties();
      auto uuid_str = uuid->str();
      UI::input_text(name.data(), &uuid_str, ImGuiInputTextFlags_ReadOnly);
      UI::end_properties();

      auto& asset_man = App::mod<AssetManager>();

      ImGui::PushID(field_ptr);
      static bool draw_asset_picker = false;
      if (UI::button(ICON_MDI_CIRCLE_DOUBLE)) {
        draw_asset_picker = !draw_asset_picker;
      }

      if (draw_asset_picker) {
        Asset selected = {};
        AssetType filter = {};
        inspector_panel.viewer.render("Asset Picker", &draw_asset_picker, filter, &selected);

        // NOTE: We don't allow model assets to be loaded this way yet(or ever).
        if (selected.type != AssetType::None && selected.type != AssetType::Model) {
          // NOTE: Don't allow the existing asset to be swapped with a different type of asset.
          auto existing_type = AssetType::None;
          if (auto existing_asset = asset_man.get_asset(*uuid)) {
            existing_type = existing_asset->type;
          }
          const bool is_same_asset = selected.uuid == *uuid;
          const bool is_same_type = existing_type == selected.type;
          const bool is_loaded = asset_man.load_asset(selected.uuid);
          if (!is_same_asset && is_same_type && is_loaded) {
            if (*uuid) {
              asset_man.unload_asset(*uuid);
            }
            *uuid = selected.uuid;
            modified = true;
          }
        }
      }

      ImGui::SameLine();

      const float x = ImGui::GetContentRegionAvail().x;
      const float y = ImGui::GetFrameHeight();
      const auto btn = fmt::format("{} Drop an asset file", ICON_MDI_FILE_UPLOAD);
      UI::button(btn.c_str(), {x, y});
      ImGui::PopID();

      if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* imgui_payload = ImGui::AcceptDragDropPayload(PayloadData::DRAG_DROP_SOURCE)) {
          const auto payload = PayloadData::from_payload(imgui_payload);
          if (payload->get_str().empty())
            return;
          if (auto imported_asset = asset_man.import_asset(payload->str)) {
            // Must not hold a registry read guard while unloading: unload_asset() takes the
            // registry write lock. unload_asset() no-ops on missing/unloaded assets.
            if (*uuid) {
              asset_man.unload_asset(*uuid);
            }
            if (asset_man.load_asset(imported_asset)) {
              *uuid = imported_asset;
              modified = true;
            }
          }
        }
        ImGui::EndDragDropTarget();
      }
      ImGui::Spacing();
      ImGui::Separator();

      if (auto asset = asset_man.get_asset(*uuid)) {
        const auto asset_type = asset->type;
        const auto asset_uuid = asset->uuid;
        const auto asset_path = asset->path;
        const auto model_id = asset_type == AssetType::Model ? asset->model_id : ModelID::Invalid;
        const auto material_id = asset_type == AssetType::Material ? asset->material_id : MaterialID::Invalid;
        const auto audio_id = asset_type == AssetType::Audio ? asset->audio_id : AudioID::Invalid;
        const auto script_id = asset_type == AssetType::Script ? asset->script_id : ScriptID::Invalid;
        const auto skeleton_id = asset_type == AssetType::Skeleton ? asset->skeleton_id : SkeletonID::Invalid;
        const auto animation_id = asset_type == AssetType::Animation ? asset->animation_id : AnimationID::Invalid;
        const auto cinematic_id = asset_type == AssetType::Cinematic ? asset->cinematic_id : CinematicID::Invalid;
        asset.reset();

        switch (asset_type) {
          case ox::AssetType::None:
          case AssetType::Shader  : // TODO: Shaders
          case AssetType::Texture : // TODO: Textures
          case AssetType::Font    : // TODO: Fonts
          case AssetType::Scene   : // TODO: Scenes
          case AssetType::Terrain : // TODO: Terrain edits
            break;
          case AssetType::Skeleton: {
            if (auto skeleton = asset_man.get_skeleton(skeleton_id)) {
              UI::begin_properties();
              UI::text("Bones", std::to_string(skeleton->bone_count()));
              UI::end_properties();
            }
            break;
          }
          case AssetType::Animation: {
            memory::ScopedStack stack;

            // the picker shows every clip of a model under the same file name, so offer the
            // sibling clips by name instead
            auto siblings = ankerl::svector<std::pair<const c8*, UUID>, 8>();

            // the model's own list is authoritative and in import order, with the registry as a
            // fallback for when the model itself is not loaded
            auto model_animations = model_animation_clips(find_source_model(asset_uuid));
            if (model_animations.empty()) {
              model_animations = sibling_animation_clips(asset_uuid);
            }

            for (const auto& sibling_uuid : model_animations) {
              // the name has to be copied onto the stack while the guard still holds the lock
              const c8* label = nullptr;
              if (auto sibling = asset_man.get_animation(sibling_uuid)) {
                label = stack.null_terminate_cstr(sibling->name);
              }

              if (label == nullptr) {
                continue;
              }

              siblings.emplace_back(label, sibling_uuid);
            }

            // ImGui wants the labels contiguous
            auto sibling_names = ankerl::svector<const c8*, 8>();
            auto selected_clip = 0;
            for (const auto& [label, sibling_uuid] : siblings) {
              if (sibling_uuid == asset_uuid) {
                selected_clip = static_cast<i32>(sibling_names.size());
              }

              sibling_names.emplace_back(label);
            }

            auto duration = 0.f;
            auto frame_count = 0_u32;
            const c8* clip_name = "";
            if (auto clip = asset_man.get_animation(animation_id)) {
              clip_name = stack.null_terminate_cstr(clip->name);
              duration = clip->duration;
              frame_count = clip->frame_count;
            }

            UI::begin_properties();
            UI::text("Name", clip_name);
            UI::text("Duration", stack.format("{:.2f}s", duration));
            UI::text("Frames", stack.format("{}", frame_count));

            auto chosen = UUID(nullptr);
            if (
              sibling_names.size() > 1 &&
              UI::property("Clip", &selected_clip, sibling_names.data(), static_cast<i32>(sibling_names.size())) &&
              selected_clip >= 0 && selected_clip < static_cast<i32>(siblings.size())
            ) {
              chosen = siblings[static_cast<usize>(selected_clip)].second;
            }
            UI::end_properties();

            if (UI::button(ICON_MDI_ANIMATION_PLAY " Preview")) {
              App::mod<Editor>().editor_panel_registry.get<AnimationEditorPanel>().open_asset(asset_uuid);
            }

            // outside the properties block and after every guard is gone, because load and unload
            // take the registry write lock
            if (chosen && chosen != *uuid && asset_man.load_asset(chosen)) {
              if (*uuid) {
                asset_man.unload_asset(*uuid);
              }

              *uuid = chosen;
              modified = true;
            }

            break;
          }
          case AssetType::ParticleSystem: {
            if (UI::button("Open Particle Editor")) {
              App::mod<Editor>().editor_panel_registry.get<ParticleEditorPanel>().open_asset(asset_uuid);
            }
            break;
          }
          case AssetType::Cinematic: {
            memory::ScopedStack stack;

            if (auto cinematic = asset_man.get_cinematic(cinematic_id)) {
              UI::begin_properties();
              UI::text("Name", cinematic->name);
              UI::text("Duration", stack.format("{:.2f}s", cinematic->duration));
              UI::text("Camera tracks", stack.format("{}", cinematic->camera_tracks.size()));
              UI::text("Property tracks", stack.format("{}", cinematic->property_tracks.size()));
              UI::end_properties();
            }

            if (UI::button(ICON_MDI_MOVIE_OPEN " Edit")) {
              App::mod<Editor>().editor_panel_registry.get<CinematicEditorPanel>().open_asset(asset_uuid);
            }
            break;
          }
          case AssetType::Model: {
            auto model = asset_man.get_model(model_id);
            inspector_panel.draw_model_asset(std::move(model));
            break;
          }
          case AssetType::Material: {
            auto material = asset_man.get_material(material_id);
            auto material_dirty = inspector_panel.draw_material_asset(asset_uuid, asset_path, std::move(material));
            material.reset();
            if (material_dirty) {
              asset_man.set_material_dirty(material_id);
              App::mod<Editor>().thumbnail_manager.invalidate_material(asset_uuid);
            }
            break;
          }
          case AssetType::Audio: {
            auto audio = asset_man.get_audio(audio_id);
            inspector_panel.draw_audio_asset(std::move(audio));
            break;
          }
          case AssetType::Script: {
            auto script = asset_man.get_script(script_id);
            if (inspector_panel.draw_script_asset(asset_uuid, std::move(script))) {
              modified = true;
            }
            break;
          }
        }
      }

      UI::begin_properties();
    }
  }
};

InspectorPanel::InspectorPanel() : EditorPanelState("Inspector", ICON_MDI_INFORMATION, true), scene_(nullptr) {
  viewer.search_icon = ICON_MDI_MAGNIFY;
  viewer.filter_icon = ICON_MDI_FILTER;

  auto& event_system = App::get_event_system();
  auto& asset_man = App::mod<AssetManager>();

  auto r = event_system.subscribe<DialogSaveEvent>([&asset_man](const DialogSaveEvent& e) {
    if (!asset_man.export_asset(e.asset_uuid, e.path)) {
      OX_LOG_ERROR("Couldn't save asset {} to {}!", e.asset_uuid.str(), e.path);
      return;
    }

    App::mod<Editor>().thumbnail_manager.invalidate_material(e.asset_uuid);
  });
}

void InspectorPanel::on_render(this InspectorPanel& self, vuk::ImageAttachment swapchain_attachment) {
  auto& editor = App::mod<Editor>();
  self.scene_ = editor.get_selected_scene();

  self.on_begin();

  self.handle_editor_context();

  self.on_end();
}

auto InspectorPanel::handle_editor_context(this InspectorPanel& self) -> void {
  ZoneScoped;

  auto& editor = App::mod<Editor>();
  auto& editor_context = editor.get_context();

  if (editor_context.entity) {
    auto e = editor_context.entity.value();

    if (e != self.last_edited_entity) {
      self.euler_cache.reset();
      self.last_edited_entity = e;
    }

    self.draw_components(e);
  } else if (editor_context.type == EditorContext::Type::File) {
    if (!editor_context.str.has_value()) {
      return;
    }

    auto& asset_man = App::mod<AssetManager>();

    auto path = std::filesystem::path(editor_context.str.value());
    std::unique_ptr<AssetManager::AssetMetaFile> meta_file = nullptr;

    if (path.extension() == ".oxasset") {
      meta_file = asset_man.read_meta_file(path);
    } else {
      meta_file = asset_man.read_meta_file_from_asset(path);
    }

    if (!meta_file) {
      return;
    }

    auto uuid_str_json = meta_file->doc["uuid"].get_string();
    if (uuid_str_json.error())
      return;

    auto uuid_from_str = UUID::from_string(uuid_str_json.value_unsafe());
    if (uuid_from_str.has_value()) {
      if (auto asset = asset_man.get_asset(*uuid_from_str))
        self.draw_asset_info(std::move(asset));
    }
  }
}

auto InspectorPanel::draw_material_properties(
  ReadGuard<Material> material, const UUID& material_uuid, const std::filesystem::path& default_path
) -> bool {
  if (material_uuid) {
    const auto& window = App::get_window();

    auto uuid_str = fmt::format("UUID: {}", material_uuid.str());
    ImGui::TextUnformatted(uuid_str.c_str());

    const float x = ImGui::GetContentRegionAvail().x / 2;
    const float y = ImGui::GetFrameHeight();

    const auto has_own_file = AssetManager::owns_meta_file(default_path);

    const auto open_save_as_dialog = [&window, &material_uuid, &default_path] {
      pending_save_material_uuid = material_uuid;

      FileDialogFilter dialog_filters[] = {{.name = "Asset (.oxasset)", .pattern = "oxasset"}};
      window.show_dialog({
        .kind = DialogKind::SaveFile,
        .user_data = nullptr,
        .callback =
          [](void*, const c8* const* files, i32) {
            if (!files || !*files || !pending_save_material_uuid) {
              return;
            }

            const auto first_path_cstr = *files;
            const auto first_path_len = std::strlen(first_path_cstr);
            auto path = std::string(first_path_cstr, first_path_len);

            auto& event_system = App::get_event_system();
            auto r = event_system.emit(DialogSaveEvent{pending_save_material_uuid, path});
            if (!r.has_value()) {
              OX_LOG_ERROR("{}", r.error().message());
            }
          },
        .title = "Save material asset file...",
        .default_path = default_path,
        .filters = dialog_filters,
        .multi_select = false,
      });
    };

    auto save_str = fmt::format("{} Save", ICON_MDI_CONTENT_SAVE);
    if (UI::button(save_str.c_str(), {x, y})) {
      if (has_own_file) {
        auto& event_system = App::get_event_system();
        auto r = event_system.emit(DialogSaveEvent{material_uuid, default_path});
        if (!r.has_value()) {
          OX_LOG_ERROR("{}", r.error().message());
        }
      } else {
        open_save_as_dialog();
      }
    }

    if (ImGui::BeginDragDropSource()) {
      std::string path_str = default_path.empty() ? "new_material" : default_path.filename().string();
      auto payload = PayloadData(path_str, material_uuid);
      ImGui::SetDragDropPayload(PayloadData::DRAG_DROP_TARGET, &payload, payload.size());
      ImGui::TextUnformatted(path_str.c_str());
      ImGui::EndDragDropSource();
    }

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay)) {
      ImGui::BeginTooltip();
      if (has_own_file) {
        ImGui::Text("Writes the material back to its own asset file.");
      } else {
        ImGui::Text("This material lives inside another asset and asks where to write a copy.");
      }
      ImGui::Text("You can drag&drop this into content window to save a copy.");
      ImGui::EndTooltip();
    }

    ImGui::SameLine();

    auto save_as_str = fmt::format("{} Save As", ICON_MDI_CONTENT_SAVE_EDIT);
    if (UI::button(save_as_str.c_str(), {x, y})) {
      open_save_as_dialog();
    }
  }

  bool dirty = false;

  UI::begin_properties(UI::default_properties_flags);

  const char* alpha_modes[] = {"Opaque", "Mask", "Blend"};
  dirty |= UI::property("Alpha mode", reinterpret_cast<int*>(&material->alpha_mode), alpha_modes, 3);
  if (material->alpha_mode == AlphaMode::Mask) {
    dirty |= UI::property("Alpha cutoff", &material->alpha_cutoff, 0.0f, 1.0f);
  }

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

  const auto load_callback = [](bool is_srgb) {
    return [is_srgb](const char* label, const UUID& uuid, bool& active) -> UUID {
      Asset selected = {};
      AssetType filter = AssetType::Texture;
      auto name = fmt::format("Asset Picker: {}", label);
      static AssetManagerViewer am;
      am.render(name.c_str(), &active, filter, &selected);

      if (selected.type == AssetType::Texture) {
        auto& asset_man = App::mod<AssetManager>();
        const bool is_loaded = asset_man.load_asset(selected.uuid, TextureLoadInfo{.is_srgb = is_srgb});
        if (is_loaded) {
          if (uuid) {
            asset_man.unload_asset(uuid);
          }
          return selected.uuid;
        }
      }

      return UUID(nullptr);
    };
  };

  const auto texture_slot = [&load_callback](const char* label, UUID& uuid, bool is_srgb) -> bool {
    return UI::texture_property(label, uuid, is_srgb, load_callback(is_srgb));
  };

  dirty |= texture_slot("Albedo", material->albedo_texture, true);
  dirty |= texture_slot("Normal", material->normal_texture, false);
  dirty |= UI::property("Flip Normal Y", &material->flip_normal_y, "Enable for DirectX-convention normal maps.");
  dirty |= texture_slot("Emissive", material->emissive_texture, true);
  dirty |= UI::property_vector("Emissive Color", material->emissive_color, true, false);
  dirty |= texture_slot("Metallic Roughness", material->metallic_roughness_texture, false);
  dirty |= UI::property("Roughness Factor", &material->roughness_factor, 0.0f, 1.0f);
  dirty |= UI::property("Metallic Factor", &material->metallic_factor, 0.0f, 1.0f);
  dirty |= texture_slot("Occlusion", material->occlusion_texture, false);

  UI::end_properties();

  return dirty;
}

void InspectorPanel::draw_component_context_menu(bool& remove_component, flecs::entity entity, flecs::id fid) {
  ZoneScoped;

  memory::ScopedStack stack;
  auto remove_component_txt = stack.format("{} Remove Component", ICON_MDI_MINUS);
  auto reset_component_txt = stack.format("{} Reset Component", ICON_MDI_RELOAD);
  auto copy_component_txt = stack.format("{} Copy Component", ICON_MDI_CONTENT_COPY);
  auto paste_component_txt = stack.format("{} Paste Component", ICON_MDI_CONTENT_PASTE);

  if (ImGui::MenuItem(remove_component_txt.data())) {
    remove_component = true;
  }
  if (ImGui::MenuItem(reset_component_txt.data())) {
    entity.remove(fid).add(fid);
  }
  if (ImGui::MenuItem(copy_component_txt.data())) {
    component_clipboard.source_entity = entity;
    component_clipboard.component_id = fid;
  }
  if (ImGui::MenuItem(paste_component_txt.data())) {
    if (component_clipboard.is_valid() && component_clipboard.source_entity != entity) {
      auto& editor = App::mod<Editor>();
      auto& undo_redo_system = editor.undo_redo_system;
      undo_redo_system->execute_command<ComponentCopyCommand>(
        component_clipboard.source_entity,
        entity,
        component_clipboard.component_id,
        "Paste Component"
      );
    }
  }
}

void InspectorPanel::draw_components(this InspectorPanel& self, flecs::entity entity) {
  ZoneScoped;

  if (!entity || !self.scene_)
    return;

  auto& editor = App::mod<Editor>();
  auto& undo_redo_system = editor.undo_redo_system;

  ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - (ImGui::CalcTextSize(ICON_MDI_PLUS).x + 20.0f));
  std::string new_name = entity.name().c_str();
  if (self.rename_entity_)
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

  const auto components = self.scene_->component_db.get_components();

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

  entity.each([&](flecs::id fid) {
    memory::ScopedStack stack;
    static constexpr ImGuiTreeNodeFlags TREE_FLAGS = ImGuiTreeNodeFlags_DefaultOpen |
                                                     ImGuiTreeNodeFlags_SpanAvailWidth |
                                                     ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_Framed |
                                                     ImGuiTreeNodeFlags_FramePadding;
    const float line_height = editor.editor_theme.regular_font_size + GImGui->Style.FramePadding.y * 2.0f;

    auto ty = fid.type_id();
    if (!ty) {
      return;
    }

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + line_height * 0.25f);

    bool remove_component = false;

    auto component_name = ty.name();
    auto name_cstr = stack.format_char("{} {}:{}", ICON_MDI_VIEW_GRID, component_name.c_str(), fid.raw_id());
    const bool open = ImGui::TreeNodeEx(name_cstr, TREE_FLAGS, "%s", name_cstr);
    if (ImGui::BeginPopupContextItem()) {
      self.draw_component_context_menu(remove_component, entity, fid);
      ImGui::EndPopup();
    }

    ImGui::PushID(name_cstr);
    const float frame_height = ImGui::GetFrameHeight();
    ImGui::SameLine(ImGui::GetContentRegionMax().x - frame_height * 1.2f);
    if (UI::button(ICON_MDI_COG, ImVec2{frame_height * 1.2f, frame_height})) {
      ImGui::OpenPopup("ComponentSettings");
    }

    if (ImGui::BeginPopup("ComponentSettings")) {
      self.draw_component_context_menu(remove_component, entity, fid);
      ImGui::EndPopup();
    }
    ImGui::PopID();

    if (open && ty.has<flecs::TypeSerializer>()) {
      ImGuiTableFlags properties_flags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV;

      UI::begin_properties(properties_flags);

      auto world = entity.world();
      auto inspector = EntityInspector(world, *undo_redo_system.get(), self);
      auto* component = entity.get_mut(fid);
      inspector.serialize(ty, component);
      if (inspector.modified) {
        entity.modified(fid);
      }

      UI::end_properties();
      ImGui::TreePop();
    }

    if (remove_component) {
      if (fid == entity.world().component<TransformComponent>().type_id()) {
        OX_LOG_ERROR("Can't remove TransformComponent!");
      } else {
        entity.remove(fid);
      }
    }
  });
}

auto InspectorPanel::draw_asset_info(this InspectorPanel& self, ReadGuard<Asset> asset) -> void {
  ZoneScoped;
  auto& editor = App::mod<Editor>();
  auto& asset_man = App::mod<AssetManager>();

  const auto asset_type = asset->type;
  const auto asset_uuid = asset->uuid;
  const auto asset_path = asset->path;
  asset.reset();

  auto type_str = asset_man.to_asset_type_sv(asset_type);
  auto uuid_str = asset_uuid.str();
  auto name = asset_path.filename().string();
  auto path_str = asset_path.string();

  ImGui::SeparatorText("Asset");
  ImGui::Indent();

  auto thumbnail_image = TextureView{};
  if (asset_type == AssetType::Texture) {
    thumbnail_image = editor.thumbnail_manager.get_thumbnail_texture(path_str);
  } else if (asset_type == AssetType::Model) {
    thumbnail_image = editor.thumbnail_manager.get_thumbnail_model(path_str);
  } else if (asset_type == AssetType::Material) {
    thumbnail_image = editor.thumbnail_manager.get_thumbnail_material(asset_uuid);
  } else if (asset_type == AssetType::Terrain) {
    thumbnail_image = editor.thumbnail_manager.get_thumbnail_terrain(path_str);
  }
  const auto region = ImGui::GetContentRegionAvail();
  auto content_width = region.x - ImGui::GetStyle().IndentSpacing;
  if (thumbnail_image) {
    UI::image(thumbnail_image, {content_width, content_width});
  } else {
    ImGui::PushFont(nullptr, 32.f);
    ImGui::Text("No thumbnail");
    ImGui::PopFont();
  }

  UI::begin_properties(ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit);
  UI::text("Type", type_str);
  UI::input_text("UUID", &uuid_str, ImGuiInputTextFlags_ReadOnly);
  UI::input_text("File", &name, ImGuiInputTextFlags_ReadOnly);
  UI::input_text("Path", &path_str, ImGuiInputTextFlags_ReadOnly);
  UI::end_properties();

  ImGui::Unindent();

  if (asset_type == AssetType::Material) {
    if (!asset_man.is_loaded(asset_uuid)) {
      asset_man.load_asset(asset_uuid);
    }

    auto mat = asset_man.get_material(asset_uuid);
    if (mat) {
      ImGui::SeparatorText("Material");
      const auto material_dirty = draw_material_properties(std::move(mat), asset_uuid, asset_path);
      mat.reset();
      if (material_dirty) {
        asset_man.set_material_dirty(asset_uuid);
        editor.thumbnail_manager.invalidate_material(asset_uuid);
      }
    } else {
      ImGui::SeparatorText("Material");
      ImGui::TextUnformatted("Couldn't load material.");
    }
  }
}

auto InspectorPanel::draw_model_asset(this InspectorPanel& self, ReadGuard<Model> model) -> void {
  ZoneScoped;

  if (!model) {
    return;
  }
}

auto InspectorPanel::draw_material_asset(
  this InspectorPanel& self, const UUID& uuid, const std::filesystem::path& path, ReadGuard<Material> material
) -> bool {
  ZoneScoped;

  ImGui::SeparatorText("Material");

  if (material) {
    return draw_material_properties(std::move(material), uuid, path);
  } else {
    ImGui::Text("No Material");
  }

  return false;
}

void InspectorPanel::draw_audio_asset(this InspectorPanel& self, ReadGuard<AudioSource> audio) {
  ZoneScoped;

  auto& audio_engine = App::mod<AudioEngine>();

  ImGui::Spacing();
  if (UI::button(ICON_MDI_PLAY "Play "))
    audio_engine.play_source(audio->get_source());
  ImGui::SameLine();
  if (UI::button(ICON_MDI_PAUSE "Pause "))
    audio_engine.pause_source(audio->get_source());
  ImGui::SameLine();
  if (UI::button(ICON_MDI_STOP "Stop "))
    audio_engine.stop_source(audio->get_source());
  ImGui::Spacing();
}

bool InspectorPanel::draw_script_asset(this InspectorPanel& self, const UUID& uuid, ReadGuard<LuaScript> script) {
  ZoneScoped;
  memory::ScopedStack stack;

  auto& asset_man = App::mod<AssetManager>();

  if (!script)
    return false;

  auto script_path_filename = stack.format("{}", script->path.filename());
  auto script_path_str = script->path.string();
  UI::begin_properties(ImGuiTableFlags_SizingFixedFit);
  UI::text("File Name:", script_path_filename);
  UI::input_text("Path:", &script_path_str, ImGuiInputTextFlags_ReadOnly);
  UI::end_properties();
  auto* rmv_str = stack.format_char("{} Remove", ICON_MDI_TRASH_CAN);
  if (UI::button(rmv_str)) {
    if (uuid)
      asset_man.unload_asset(uuid);
  }

  return false;
}
} // namespace ox
