#include "InspectorPanel.hpp"

#include <icons/IconsMaterialDesignIcons.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Core/FileSystem.hpp"
#include "EditorLayer.hpp"
#include "EditorTheme.hpp"
#include "EditorUI.hpp"
#include "Render/ParticleSystem.hpp"
#include "Scene/ECSModule/Core.hpp"
#include "Utils/ColorUtils.hpp"
#include "Utils/PayloadData.hpp"
#include "Utils/StringUtils.hpp"

namespace ox {
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

template <typename T, typename UIFunction>
void draw_component(const char* name, flecs::entity entity, UIFunction ui_function, const bool removable = true) {
  if (!entity.has<T>())
    return;
  auto* component = entity.get_mut<T>();
  if (!component) {
    return;
  }
  static constexpr ImGuiTreeNodeFlags TREE_FLAGS = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth |
                                                   ImGuiTreeNodeFlags_AllowItemOverlap | ImGuiTreeNodeFlags_Framed |
                                                   ImGuiTreeNodeFlags_FramePadding;

  auto& editor_theme = EditorLayer::get()->editor_theme;

  const float line_height = editor_theme.regular_font_size + GImGui->Style.FramePadding.y * 2.0f;

  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + line_height * 0.25f);

  const auto id = typeid(T).hash_code();
  OX_ASSERT(editor_theme.component_icon_map.contains(id));
  std::string name_str = StringUtils::from_char8_t(editor_theme.component_icon_map[id]);
  name_str = name_str.append(name);
  const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(id), TREE_FLAGS, "%s", name_str.c_str());

  bool remove_component = false;
  if (removable) {
    ImGui::PushID(static_cast<int>(id));

    const float frame_height = ImGui::GetFrameHeight();
    ImGui::SameLine(ImGui::GetContentRegionMax().x - frame_height * 1.2f);
    if (UI::button(StringUtils::from_char8_t(ICON_MDI_SETTINGS), ImVec2{frame_height * 1.2f, frame_height}))
      ImGui::OpenPopup("ComponentSettings");

    if (ImGui::BeginPopup("ComponentSettings")) {
      if (ImGui::MenuItem("Remove Component"))
        remove_component = true;

      ImGui::EndPopup();
    }

    ImGui::PopID();
  }

  if (open) {
    ui_function(*component, entity);
    ImGui::TreePop();
  }

  if (remove_component)
    entity.remove<T>();
}

void InspectorPanel::draw_material_properties(Material* material, const UUID& material_uuid, flecs::entity load_event) {
  if (material_uuid) {
    const auto& window = App::get()->get_window();
    static auto uuid_copy = material_uuid;
    static auto load_event_copy = load_event;

    auto uuid_str = fmt::format("UUID: {}", material_uuid.str());
    ImGui::TextUnformatted(uuid_str.c_str());

    auto load_str = fmt::format("{} Load", StringUtils::from_char8_t(ICON_MDI_FILE_UPLOAD));

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

                load_event_copy.emit<Event::DialogLoadEvent>({path});
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
          load_event.emit<Event::DialogLoadEvent>({payload->str});
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

    auto save_str = fmt::format("{} Save", StringUtils::from_char8_t(ICON_MDI_FILE_DOWNLOAD));
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

                load_event_copy.emit<Event::DialogSaveEvent>({path});
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
    if (auto* asset = asset_man->get_asset(material_uuid))
      asset_man->set_material_dirty(asset->material_id);
  }
}

template <typename T>
static void draw_particle_over_lifetime_module(const std::string_view module_name,
                                               OverLifetimeModule<T>& property_module,
                                               bool color = false,
                                               bool rotation = false) {
  static constexpr ImGuiTreeNodeFlags TREE_FLAGS = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth |
                                                   ImGuiTreeNodeFlags_AllowItemOverlap | ImGuiTreeNodeFlags_Framed |
                                                   ImGuiTreeNodeFlags_FramePadding;

  if (ImGui::TreeNodeEx(module_name.data(), TREE_FLAGS, "%s", module_name.data())) {
    UI::begin_properties();
    UI::texture_property("Enabled", &property_module.enabled);

    if (rotation) {
      T degrees = glm::degrees(property_module.start);
      if (UI::property_vector("Start", degrees))
        property_module.start = glm::radians(degrees);

      degrees = glm::degrees(property_module.end);
      if (UI::property_vector("End", degrees))
        property_module.end = glm::radians(degrees);
    } else {
      UI::property_vector("Start", property_module.start, color);
      UI::property_vector("End", property_module.end, color);
    }

    UI::end_properties();

    ImGui::TreePop();
  }
}

template <typename T>
static void draw_particle_by_speed_module(const std::string_view module_name,
                                          BySpeedModule<T>& property_module,
                                          bool color = false,
                                          bool rotation = false) {
  static constexpr ImGuiTreeNodeFlags TREE_FLAGS = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth |
                                                   ImGuiTreeNodeFlags_AllowItemOverlap | ImGuiTreeNodeFlags_Framed |
                                                   ImGuiTreeNodeFlags_FramePadding;

  if (ImGui::TreeNodeEx(module_name.data(), TREE_FLAGS, "%s", module_name.data())) {
    UI::begin_properties();
    UI::texture_property("Enabled", &property_module.enabled);

    if (rotation) {
      T degrees = glm::degrees(property_module.start);
      if (UI::property_vector("Start", degrees))
        property_module.start = glm::radians(degrees);

      degrees = glm::degrees(property_module.end);
      if (UI::property_vector("End", degrees))
        property_module.end = glm::radians(degrees);
    } else {
      UI::property_vector("Start", property_module.start, color);
      UI::property_vector("End", property_module.end, color);
    }

    UI::texture_property("Min Speed", &property_module.min_speed);
    UI::texture_property("Max Speed", &property_module.max_speed);
    UI::end_properties();
    ImGui::TreePop();
  }
}

template <typename Component>
void draw_add_component(const flecs::entity entity, const char* name) {
  if (ImGui::MenuItem(name)) {
    if (entity.has<Component>())
      OX_LOG_WARN("Entity already has same component!");
    else
      entity.add<Component>();
    ImGui::CloseCurrentPopup();
  }
}

void InspectorPanel::draw_components(const flecs::entity entity) {
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

  if (UI::button(StringUtils::from_char8_t(ICON_MDI_PLUS))) {
    ImGui::OpenPopup("Add Component");
  }
  if (ImGui::BeginPopup("Add Component")) {
    // TODO: Use flecs reflections for easy iteration over components.
    draw_add_component<MeshComponent>(entity, "Mesh Renderer");
    draw_add_component<AudioSourceComponent>(entity, "Audio Source");
    draw_add_component<AudioListenerComponent>(entity, "Audio Listener");
    draw_add_component<LightComponent>(entity, "Light");
    draw_add_component<ParticleSystemComponent>(entity, "Particle System");
    draw_add_component<CameraComponent>(entity, "Camera");
    draw_add_component<RigidbodyComponent>(entity, "Rigidbody");
    draw_add_component<BoxColliderComponent>(entity, "Box Collider");
    draw_add_component<SphereColliderComponent>(entity, "Sphere Collider");
    draw_add_component<CapsuleColliderComponent>(entity, "Capsule Collider");
    draw_add_component<TaperedCapsuleColliderComponent>(entity, "Tapered Capsule Collider");
    draw_add_component<CylinderColliderComponent>(entity, "Cylinder Collider");
    draw_add_component<MeshColliderComponent>(entity, "Mesh Collider");
    draw_add_component<CharacterControllerComponent>(entity, "Character Controller");
    draw_add_component<LuaScriptComponent>(entity, "Lua Script Component");
    draw_add_component<SpriteComponent>(entity, "Sprite Component");
    draw_add_component<SpriteAnimationComponent>(entity, "Sprite Animation Component");
    draw_add_component<AtmosphereComponent>(entity, "Atmosphere Component");
    draw_add_component<AutoExposureComponent>(entity, "Auto Exposure Component");
    ImGui::EndPopup();
  }

  draw_component<TransformComponent>(
      " Transform Component", entity, [](TransformComponent& component, flecs::entity e) {
        UI::begin_properties(ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV);
        if (UI::draw_vec3_control("Translation", component.position)) {
          e.modified<TransformComponent>();
        }
        glm::vec3 rotation = glm::degrees(component.rotation);
        if (UI::draw_vec3_control("Rotation", rotation)) {
          component.rotation = glm::radians(rotation);
          e.modified<TransformComponent>();
        }
        if (UI::draw_vec3_control("Scale", component.scale, nullptr, 1.0f)) {
          e.modified<TransformComponent>();
        }
        UI::end_properties();
      });

  draw_component<MeshComponent>(" Mesh Component", entity, [](MeshComponent& component, flecs::entity e) {
    UI::begin_properties();
    auto mesh_uuid_str = component.mesh_uuid.str();
    UI::input_text("Mesh UUID", &mesh_uuid_str, ImGuiInputTextFlags_ReadOnly);
    UI::text("Mesh Index", std::to_string(component.mesh_index));
    UI::property("Cast shadows", &component.cast_shadows);
    UI::end_properties();

    auto load_event = App::get()->world.entity("mesh_material_load_event");
    auto* asset_man = App::get_asset_manager();
    if (auto* mesh = asset_man->get_mesh(component.mesh_uuid)) {
      for (auto& mat_uuid : mesh->materials) {
        static constexpr ImGuiTreeNodeFlags TREE_FLAGS = ImGuiTreeNodeFlags_DefaultOpen |
                                                         ImGuiTreeNodeFlags_SpanAvailWidth |
                                                         ImGuiTreeNodeFlags_AllowItemOverlap |
                                                         ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_FramePadding;

        if (auto* material = asset_man->get_material(mat_uuid)) {
          const auto mat_uuid_str = mat_uuid.str();
          if (ImGui::TreeNodeEx(mat_uuid_str.c_str(), TREE_FLAGS, "%s", mat_uuid_str.c_str())) {
            draw_material_properties(material, mat_uuid, load_event);
            ImGui::TreePop();
          }
        }
      }
    }
  });

  draw_component<SpriteComponent>(" Sprite Component", entity, [](SpriteComponent& component, flecs::entity e) {
    UI::begin_properties();
    UI::property("Layer", &component.layer);
    UI::property("SortY", &component.sort_y);
    UI::property("FlipX", &component.flip_x);
    UI::end_properties();

    ImGui::SeparatorText("Material");

    auto load_event = App::get()->world.entity("sprite_material_load_event");
    load_event.observe<Event::DialogLoadEvent>([&component](Event::DialogLoadEvent& en) {
      auto* asset_man = App::get_asset_manager();
      if (auto imported = asset_man->import_asset(en.path)) {
        component.material = imported;
        asset_man->unload_asset(component.material);
      }
    });

    load_event.observe<Event::DialogSaveEvent>([&component](Event::DialogSaveEvent& en) {
      auto* asset_man = App::get_asset_manager();
      asset_man->export_asset(component.material, en.path);
    });

    if (auto* material = App::get_asset_manager()->get_material(component.material)) {
      draw_material_properties(material, component.material, load_event);
    }
  });

  draw_component<SpriteAnimationComponent>(
      " Sprite Animation Component", entity, [](SpriteAnimationComponent& component, flecs::entity e) {
        UI::begin_properties();
        if (UI::property("Number of frames", &component.num_frames))
          component.reset();
        if (UI::property("Loop", &component.loop))
          component.reset();
        if (UI::property("Inverted", &component.inverted))
          component.reset();
        if (UI::property("Frames per second", &component.fps))
          component.reset();
        if (UI::property("Columns", &component.columns))
          component.reset();
        if (UI::draw_vec2_control("Frame size", component.frame_size))
          component.reset();
        const float x = ImGui::GetContentRegionAvail().x;
        const float y = ImGui::GetFrameHeight();
        ImGui::Spacing();
        if (UI::button("Auto", {x, y})) {
          if (const auto* sc = e.get<SpriteComponent>()) {
            auto asset_man = App::get_asset_manager();
            if (const auto* material = asset_man->get_material(sc->material)) {
              if (const auto* texture = asset_man->get_texture(material->albedo_texture)) {
                component.set_frame_size(texture->get_extent().width, texture->get_extent().height);
              }
            }
          }
        }
        UI::end_properties();
      });

  draw_component<AutoExposureComponent>(
      " Auto Exposure Component", entity, [](AutoExposureComponent& component, flecs::entity e) {
        UI::begin_properties();
        UI::property("Min Exposure", &component.min_exposure);
        UI::property("Max Exposure", &component.max_exposure);
        UI::property("Adaptation Speed", &component.adaptation_speed);
        UI::property("EV100 Bias", &component.ev100_bias);
        UI::end_properties();
      });

  draw_component<AudioSourceComponent>(
      " Audio Source Component", entity, [](AudioSourceComponent& component, flecs::entity e) {
        auto* asset_man = App::get_asset_manager();
        auto* asset = asset_man->get_asset(component.audio_source);

        const std::string filepath = (asset && asset->is_loaded())
                                         ? asset->path
                                         : fmt::format("{} Drop an audio file",
                                                       StringUtils::from_char8_t(ICON_MDI_FILE_UPLOAD));

        const float x = ImGui::GetContentRegionAvail().x;
        const float y = ImGui::GetFrameHeight();
        if (UI::button(filepath.c_str(), {x, y})) {
          const auto& window = App::get()->get_window();
          FileDialogFilter dialog_filters[] = {{.name = "Audio file(.mp3, .wav, .flac)", .pattern = "mp3;wav;flac"}};
          window.show_dialog({
              .kind = DialogKind::OpenFile,
              .user_data = &component,
              .callback =
                  [](void* user_data, const c8* const* files, i32) {
                    auto* comp = static_cast<AudioSourceComponent*>(user_data);

                    if (!files || !*files) {
                      return;
                    }

                    const auto first_path_cstr = *files;
                    const auto first_path_len = std::strlen(first_path_cstr);
                    auto path = ::fs::path(std::string(first_path_cstr, first_path_len));
                    if (const std::string ext = path.extension().string();
                        ext == ".mp3" || ext == ".wav" || ext == ".flac") {
                      auto* am = App::get_asset_manager();
                      comp->audio_source = am->create_asset(AssetType::Audio, path.string());
                      am->load_asset(comp->audio_source);
                    }
                  },
              .title = "Open audio file...",
              .default_path = fs::current_path(),
              .filters = dialog_filters,
              .multi_select = false,
          });
        }
        if (ImGui::BeginDragDropTarget()) {
          if (const ImGuiPayload* imgui_payload = ImGui::AcceptDragDropPayload(PayloadData::DRAG_DROP_SOURCE)) {
            const auto payload = PayloadData::from_payload(imgui_payload);
            const auto payload_str = payload->get_str();
            if (payload_str.empty())
              return;
            if (fs::get_file_extension(payload_str) == "oxasset") {
              if (auto imported = asset_man->import_asset(payload_str)) {
                component.audio_source = imported;
                asset_man->unload_asset(component.audio_source);
              }
            }
          }
          ImGui::EndDragDropTarget();
        }
        ImGui::Spacing();

        auto* audio_asset = asset_man->get_audio(component.audio_source);
        if (!audio_asset)
          return;

        auto* audio_engine = App::get_system<AudioEngine>(EngineSystems::AudioEngine);

        UI::begin_properties();
        if (UI::property("Volume Multiplier", &component.volume))
          audio_engine->set_source_volume(audio_asset->get_source(), component.volume);
        if (UI::property("Pitch Multiplier", &component.pitch))
          audio_engine->set_source_pitch(audio_asset->get_source(), component.pitch);
        if (UI::property("Looping", &component.looping))
          audio_engine->set_source_looping(audio_asset->get_source(), component.looping);
        UI::property("Play On Awake", &component.play_on_awake);
        UI::end_properties();

        ImGui::Spacing();
        if (UI::button(StringUtils::from_char8_t(ICON_MDI_PLAY "Play ")))
          audio_engine->play_source(audio_asset->get_source());
        ImGui::SameLine();
        if (UI::button(StringUtils::from_char8_t(ICON_MDI_PAUSE "Pause ")))
          audio_engine->pause_source(audio_asset->get_source());
        ImGui::SameLine();
        if (UI::button(StringUtils::from_char8_t(ICON_MDI_STOP "Stop ")))
          audio_engine->stop_source(audio_asset->get_source());
        ImGui::Spacing();

        UI::begin_properties();
        UI::property("Spatialization", &component.spatialization);

        if (component.spatialization) {
          ImGui::Indent();
          const char* attenuation_type_strings[] = {"None", "Inverse", "Linear", "Exponential"};
          int attenuation_type = static_cast<int>(component.attenuation_model);
          if (UI::property("Attenuation Model", &attenuation_type, attenuation_type_strings, 4)) {
            component.attenuation_model = static_cast<AttenuationModelType>(attenuation_type);
            audio_engine->set_source_attenuation_model(audio_asset->get_source(), component.attenuation_model);
          }
          if (UI::property("Roll Off", &component.roll_off))
            audio_engine->set_source_roll_off(audio_asset->get_source(), component.roll_off);
          if (UI::property("Min Gain", &component.min_gain))
            audio_engine->set_source_min_gain(audio_asset->get_source(), component.min_gain);
          if (UI::property("Max Gain", &component.max_gain))
            audio_engine->set_source_max_gain(audio_asset->get_source(), component.max_gain);
          if (UI::property("Min Distance", &component.min_distance))
            audio_engine->set_source_min_distance(audio_asset->get_source(), component.min_distance);
          if (UI::property("Max Distance", &component.max_distance))
            audio_engine->set_source_max_distance(audio_asset->get_source(), component.max_distance);
          float degrees = glm::degrees(component.cone_inner_angle);
          if (UI::property("Cone Inner Angle", &degrees))
            component.cone_inner_angle = glm::radians(degrees);
          degrees = glm::degrees(component.cone_outer_angle);
          if (UI::property("Cone Outer Angle", &degrees))
            component.cone_outer_angle = glm::radians(degrees);
          UI::property("Cone Outer Gain", &component.cone_outer_gain);
          UI::property("Doppler Factor", &component.doppler_factor);
          audio_engine->set_source_cone(audio_asset->get_source(),
                                        component.cone_inner_angle,
                                        component.cone_outer_angle,
                                        component.cone_outer_gain);
          ImGui::Unindent();
        }
        UI::end_properties();
      });

  draw_component<AudioListenerComponent>(
      " Audio Listener Component", entity, [](AudioListenerComponent& component, flecs::entity e) {
        UI::begin_properties();
        UI::property("Active", &component.active);
        float degrees = glm::degrees(component.cone_inner_angle);
        if (UI::property("Cone Inner Angle", &degrees))
          component.cone_inner_angle = glm::radians(degrees);
        degrees = glm::degrees(component.cone_outer_angle);
        if (UI::property("Cone Outer Angle", &degrees))
          component.cone_outer_angle = glm::radians(degrees);
        UI::property("Cone Outer Gain", &component.cone_outer_gain);

        auto* audio_engine = App::get_system<AudioEngine>(EngineSystems::AudioEngine);
        audio_engine->set_listener_cone(component.listener_index,
                                        component.cone_inner_angle,
                                        component.cone_outer_angle,
                                        component.cone_outer_gain);
        UI::end_properties();
      });

  draw_component<LightComponent>(" Light Component", entity, [](LightComponent& component, flecs::entity e) {
    UI::begin_properties();
    const char* light_type_strings[] = {"Directional", "Point", "Spot"};
    int light_type = component.type;
    if (UI::property("Light Type", &light_type, light_type_strings, 3))
      component.type = static_cast<LightComponent::LightType>(light_type);

    if (UI::property("Color Temperature Mode", &component.color_temperature_mode) && component.color_temperature_mode) {
      ColorUtils::TempratureToColor(component.temperature, component.color);
    }

    if (component.color_temperature_mode) {
      if (UI::property<u32>("Temperature (K)", &component.temperature, 1000, 40000))
        ColorUtils::TempratureToColor(component.temperature, component.color);
    } else {
      UI::property_vector("Color", component.color, true);
    }

    UI::property("Intensity", &component.intensity, 0.0f, 100.0f);

    if (component.type != LightComponent::Directional) {
      UI::property("Range", &component.range, 0.0f, 100.0f);
      UI::property("Radius", &component.radius, 0.0f, 100.0f);
      UI::property("Length", &component.length, 0.0f, 100.0f);
    }

    if (component.type == LightComponent::Spot) {
      UI::property("Outer Cone Angle", &component.outer_cone_angle, 0.0f, 100.0f);
      UI::property("Inner Cone Angle", &component.inner_cone_angle, 0.0f, 100.0f);
    }

    UI::property("Cast Shadows", &component.cast_shadows);

    const ankerl::unordered_dense::map<u32, int> res_map = {{0, 0}, {512, 1}, {1024, 2}, {2048, 3}};
    const ankerl::unordered_dense::map<int, u32> id_map = {{0, 0}, {1, 512}, {2, 1024}, {3, 2048}};

    const char* res_strings[] = {"Auto", "512", "1024", "2048"};
    int idx = res_map.at(component.shadow_map_res);
    if (UI::property("Shadow Resolution", &idx, res_strings, 4)) {
      component.shadow_map_res = id_map.at(idx);
    }

    if (component.type == LightComponent::Directional) {
      for (u32 i = 0; i < (u32)component.cascade_distances.size(); ++i)
        UI::property(fmt::format("Cascade {}", i).c_str(), &component.cascade_distances[i]);
    }

    UI::end_properties();
  });

  draw_component<AtmosphereComponent>(
      " Atmosphere Component", entity, [](AtmosphereComponent& component, flecs::entity e) {
        UI::begin_properties();

        UI::property_vector("Rayleigh Scattering", component.rayleigh_scattering, false);
        UI::property("Rayleigh Density", &component.rayleigh_density, 0.0f, 10.0f);
        UI::property_vector("Mie Scattering", component.mie_scattering, false);
        UI::property("Mie Density", &component.mie_density, 0.0f, 2.0f);
        UI::property("Mie Extinction", &component.mie_extinction, 0.0f, 5.0f);
        UI::property("Mie Asymmetry", &component.mie_asymmetry, 0.0f, 5.0f);
        UI::property_vector("Ozone Absorption", component.ozone_absorption, false);
        UI::property("Ozone Height", &component.ozone_height, 0.0f, 30.0f);
        UI::property("Ozone Thickness", &component.ozone_thickness, 0.0f, 20.0f);
        UI::property("Aerial Perspective Start KM", &component.aerial_perspective_start_km, 0.0f, 100.0f);

        UI::end_properties();
      });

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

  draw_component<BoxColliderComponent>(" Box Collider", entity, [](BoxColliderComponent& component, flecs::entity e) {
    UI::begin_properties();
    UI::property_vector("Size", component.size);
    UI::property_vector("Offset", component.offset);
    UI::property("Density", &component.density);
    UI::property("Friction", &component.friction, 0.0f, 1.0f);
    UI::property("Restitution", &component.restitution, 0.0f, 1.0f);
    UI::end_properties();

    component.density = glm::max(component.density, 0.001f);
  });

  draw_component<SphereColliderComponent>(
      " Sphere Collider", entity, [](SphereColliderComponent& component, flecs::entity e) {
        UI::begin_properties();
        UI::property("Radius", &component.radius);
        UI::property_vector("Offset", component.offset);
        UI::property("Density", &component.density);
        UI::property("Friction", &component.friction, 0.0f, 1.0f);
        UI::property("Restitution", &component.restitution, 0.0f, 1.0f);
        UI::end_properties();

        component.density = glm::max(component.density, 0.001f);
      });

  draw_component<CapsuleColliderComponent>(
      " Capsule Collider", entity, [](CapsuleColliderComponent& component, flecs::entity e) {
        UI::begin_properties();
        UI::property("Height", &component.height);
        UI::property("Radius", &component.radius);
        UI::property_vector("Offset", component.offset);
        UI::property("Density", &component.density);
        UI::property("Friction", &component.friction, 0.0f, 1.0f);
        UI::property("Restitution", &component.restitution, 0.0f, 1.0f);
        UI::end_properties();

        component.density = glm::max(component.density, 0.001f);
      });

  draw_component<TaperedCapsuleColliderComponent>(
      " Tapered Capsule Collider", entity, [](TaperedCapsuleColliderComponent& component, flecs::entity e) {
        UI::begin_properties();
        UI::property("Height", &component.height);
        UI::property("Top Radius", &component.top_radius);
        UI::property("Bottom Radius", &component.bottom_radius);
        UI::property_vector("Offset", component.offset);
        UI::property("Density", &component.density);
        UI::property("Friction", &component.friction, 0.0f, 1.0f);
        UI::property("Restitution", &component.restitution, 0.0f, 1.0f);
        UI::end_properties();

        component.density = glm::max(component.density, 0.001f);
      });

  draw_component<CylinderColliderComponent>(
      " Cylinder Collider", entity, [](CylinderColliderComponent& component, flecs::entity e) {
        UI::begin_properties();
        UI::property("Height", &component.height);
        UI::property("Radius", &component.radius);
        UI::property_vector("Offset", component.offset);
        UI::property("Density", &component.density);
        UI::property("Friction", &component.friction, 0.0f, 1.0f);
        UI::property("Restitution", &component.restitution, 0.0f, 1.0f);
        UI::end_properties();

        component.density = glm::max(component.density, 0.001f);
      });

  draw_component<MeshColliderComponent>(
      " Mesh Collider", entity, [](MeshColliderComponent& component, flecs::entity e) {
        UI::begin_properties();
        UI::property_vector("Offset", component.offset);
        UI::property("Friction", &component.friction, 0.0f, 1.0f);
        UI::property("Restitution", &component.restitution, 0.0f, 1.0f);
        UI::end_properties();
      });

  draw_component<CharacterControllerComponent>(
      " Character Controller", entity, [](CharacterControllerComponent& component, flecs::entity e) {
        UI::begin_properties();
        UI::property("CharacterHeightStanding", &component.character_height_standing);
        UI::property("CharacterRadiusStanding", &component.character_radius_standing);
        UI::property("CharacterHeightCrouching", &component.character_height_crouching);
        UI::property("CharacterRadiusCrouching", &component.character_radius_crouching);

        // Movement
        UI::property("ControlMovementDuringJump", &component.control_movement_during_jump);
        UI::property("JumpForce", &component.jump_force);

        UI::property("Friction", &component.friction, 0.0f, 1.0f);
        UI::property("CollisionTolerance", &component.collision_tolerance);
        UI::end_properties();
      });

  draw_component<CameraComponent>("Camera Component", entity, [](CameraComponent& component, flecs::entity e) {
    const auto is_perspective = component.projection == CameraComponent::Projection::Perspective;
    UI::begin_properties();

    const char* proj_strs[] = {"Perspective", "Orthographic"};
    int proj = static_cast<int>(component.projection);
    if (UI::property("Projection", &proj, proj_strs, 2))
      component.projection = static_cast<CameraComponent::Projection>(proj);

    if (is_perspective) {
      UI::property("FOV", &component.fov);
      UI::property("Near Clip", &component.near_clip);
      UI::property("Far Clip", &component.far_clip);
    } else {
      UI::property("Zoom", &component.zoom);
    }

    UI::end_properties();
  });

  draw_component<LuaScriptComponent>(
      "Lua Script Component", entity, [](LuaScriptComponent& component, flecs::entity e) {
        const float filter_cursor_pos_x = ImGui::GetCursorPosX();
        ImGuiTextFilter name_filter;

        name_filter.Draw("##scripts_filter",
                         ImGui::GetContentRegionAvail().x -
                             (UI::get_icon_button_size(ICON_MDI_PLUS, "").x + 2.0f * ImGui::GetStyle().FramePadding.x));

        if (!name_filter.IsActive()) {
          ImGui::SameLine();
          ImGui::SetCursorPosX(filter_cursor_pos_x + ImGui::GetFontSize() * 0.5f);
          ImGui::TextUnformatted(StringUtils::from_char8_t(ICON_MDI_MAGNIFY " Search..."));
        }

        for (u32 i = 0; i < (u32)component.lua_systems.size(); i++) {
          const auto& system = component.lua_systems[i];
          auto name = fs::get_file_name(system->get_path());
          if (name_filter.PassFilter(name.c_str())) {
            ImGui::PushID(i);
            constexpr ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanFullWidth |
                                                 ImGuiTreeNodeFlags_FramePadding;
            if (ImGui::TreeNodeEx(name.c_str(), flags, "%s", name.c_str())) {
              auto rld_str = fmt::format("{} Reload", StringUtils::from_char8_t(ICON_MDI_REFRESH));
              if (UI::button(rld_str.c_str()))
                system->reload();
              ImGui::SameLine();
              auto rmv_str = fmt::format("{} Remove", StringUtils::from_char8_t(ICON_MDI_TRASH_CAN));
              if (UI::button(rmv_str.c_str()))
                component.lua_systems.erase(component.lua_systems.begin() + i);
              ImGui::TreePop();
            }
            ImGui::PopID();
          }
        }

        const float x = ImGui::GetContentRegionAvail().x;
        const float y = ImGui::GetFrameHeight();
        const auto btn = fmt::format("{} Drop a script file", StringUtils::from_char8_t(ICON_MDI_FILE_UPLOAD));
        if (UI::button(btn.c_str(), {x, y})) {
          const auto& window = App::get()->get_window();
          FileDialogFilter dialog_filters[] = {{.name = "Lua file(.lua)", .pattern = "lua"}};
          window.show_dialog({
              .kind = DialogKind::OpenFile,
              .user_data = &component,
              .callback =
                  [](void* user_data, const c8* const* files, i32) {
                    auto* user_data_comp = static_cast<LuaScriptComponent*>(user_data);
                    if (!files || !*files) {
                      return;
                    }

                    const auto first_path_cstr = *files;
                    const auto first_path_len = std::strlen(first_path_cstr);
                    const auto p_str = std::string(first_path_cstr, first_path_len);
                    if (fs::get_file_extension(p_str) == "lua") {
                      user_data_comp->lua_systems.emplace_back(std::make_shared<LuaSystem>(p_str));
                    }
                  },
              .title = "Open lua file...",
              .default_path = fs::current_path(),
              .filters = dialog_filters,
              .multi_select = false,
          });
        }
        if (ImGui::BeginDragDropTarget()) {
          if (const ImGuiPayload* imgui_payload = ImGui::AcceptDragDropPayload(PayloadData::DRAG_DROP_SOURCE)) {
            const auto payload = PayloadData::from_payload(imgui_payload);
            if (payload->get_str().empty())
              return;
            if (fs::get_file_extension(payload->get_str()) == "lua") {
              auto* asset_man = App::get_asset_manager();
              if (auto imported = asset_man->import_asset(payload->str)) {
                component.script_uuid = imported;
                asset_man->unload_asset(component.script_uuid);
              }
            }
          }
          ImGui::EndDragDropTarget();
        }
      });

#if 0
  draw_component<ParticleSystemComponent>(
      "Particle System Component", entity, [](const ParticleSystemComponent& component, flecs::entity e) {
    auto& props = component.system->get_properties();

    ImGui::Text("Active Particles Count: %u", component.system->get_active_particle_count());
    ImGui::BeginDisabled(props.looping);
    if (UI::button(StringUtils::from_char8_t(ICON_MDI_PLAY)))
      component.system->play();
    ImGui::SameLine();
    if (UI::button(StringUtils::from_char8_t(ICON_MDI_STOP)))
      component.system->stop();
    ImGui::EndDisabled();

    ImGui::Separator();

    UI::begin_properties();
    UI::texture_property("Duration", &props.duration);
    if (UI::texture_property("Looping", &props.looping)) {
      if (props.looping)
        component.system->play();
    }
    UI::texture_property("Start Delay", &props.start_delay);
    UI::texture_property("Start Lifetime", &props.start_lifetime);
    UI::property_vector("Start Velocity", props.start_velocity);
    UI::property_vector("Start Color", props.start_color, true);
    UI::property_vector("Start Size", props.start_size);
    UI::property_vector("Start Rotation", props.start_rotation);
    UI::texture_property("Gravity Modifier", &props.gravity_modifier);
    UI::texture_property("Simulation Speed", &props.simulation_speed);
    UI::texture_property("Play On Awake", &props.play_on_awake);
    UI::texture_property("Max Particles", &props.max_particles);
    UI::end_properties();

    ImGui::Separator();

    UI::begin_properties();
    UI::texture_property("Rate Over Time", &props.rate_over_time);
    UI::texture_property("Rate Over Distance", &props.rate_over_distance);
    UI::texture_property("Burst Count", &props.burst_count);
    UI::texture_property("Burst Time", &props.burst_time);
    UI::property_vector("Position Start", props.position_start);
    UI::property_vector("Position End", props.position_end);
    // OxUI::Property("Texture", props.Texture); //TODO:
    UI::end_properties();

    draw_particle_over_lifetime_module("Velocity Over Lifetime", props.velocity_over_lifetime);
    draw_particle_over_lifetime_module("Force Over Lifetime", props.force_over_lifetime);
    draw_particle_over_lifetime_module("Color Over Lifetime", props.color_over_lifetime, true);
    draw_particle_by_speed_module("Color By Speed", props.color_by_speed, true);
    draw_particle_over_lifetime_module("Size Over Lifetime", props.size_over_lifetime);
    draw_particle_by_speed_module("Size By Speed", props.size_by_speed);
    draw_particle_over_lifetime_module("Rotation Over Lifetime", props.rotation_over_lifetime, false, true);
    draw_particle_by_speed_module("Rotation By Speed", props.rotation_by_speed, false, true);
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
} // namespace ox
