#include "ViewportPanel.hpp"

#include <ImGuizmo.h>
#include <cmath>
#include <glm/gtx/matrix_decompose.hpp>
#include <icons/IconsMaterialDesignIcons.h>
#include <imgui.h>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Core/Enum.hpp"
#include "Core/Input.hpp"
#include "Editor.hpp"
#include "Render/Camera.hpp"
#include "Render/RenderContext.hpp"
#include "Render/Utils/VukCommon.hpp"
#include "Scene/Components.hpp"
#include "UI/ImGuiRenderer.hpp"
#include "UI/PayloadData.hpp"
#include "UI/RmlUI.hpp"
#include "UI/UI.hpp"
#include "Utils/EditorGrid.hpp"
#include "Utils/OxMath.hpp"

namespace ox {
struct GizmoInfo {
  f32 icon_size;
  f32 width;
  f32 height;
  f32 xpos;
  f32 ypos;
  glm::mat4 view_proj;
  Frustum frustum;
};
template <typename T, typename Func>
void show_component_gizmo(const GizmoInfo& gizmo_info, const std::string& name, Scene* scene, Func&& icon_select_func) {
  auto& editor = App::mod<Editor>();
  auto& editor_theme = editor.editor_theme;
  const auto scaled_icon_size = UI::scale(gizmo_info.icon_size);

  scene->world.query_builder<T>().build().each([&](flecs::entity entity, const T& component) {
    const glm::vec3 pos = Scene::get_world_transform(entity)[3];

    if (entity.has<Hidden>())
      return;

    if (gizmo_info.frustum.is_inside(pos) == (u32)Intersection::Outside)
      return;

    const glm::vec2 screen_pos = math::world_to_screen(
      pos,
      gizmo_info.view_proj,
      gizmo_info.width,
      gizmo_info.height,
      gizmo_info.xpos,
      gizmo_info.ypos
    );
    ImGui::SetCursorPos({screen_pos.x - scaled_icon_size * 0.5f, screen_pos.y - scaled_icon_size * 0.5f});
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.7f, 0.7f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.1f, 0.1f, 0.1f, 0.1f));

    ImGui::PushFont(nullptr, gizmo_info.icon_size);
    ImGui::PushID(static_cast<i32>(entity.id()));
    const char* icon = icon_select_func(editor_theme.component_icon_map.at(typeid(T).hash_code()), component);
    if (ImGui::Button(icon, {scaled_icon_size, scaled_icon_size})) {
      auto& editor_context = editor.get_context();
      editor_context.reset(EditorContext::Type::Entity, nullopt, entity);
    }
    ImGui::PopID();
    ImGui::PopFont();

    ImGui::PopStyleColor(2);

    UI::tooltip_hover(name.data());
  });
}

ViewportPanel::ViewportPanel() : EditorPanelState("Viewport", ICON_MDI_TERRAIN, true) {
  ZoneScoped;

  auto& render_context = App::get_rendercontext();
  auto& runtime = *render_context.runtime;
  if (!runtime.is_pipeline_available("entity_mouse_picking")) {
    auto& vfs = App::get_vfs();
    auto shaders_dir = vfs.resolve_physical_dir(VFS::APP_DIR, "Shaders");
    auto shader_file = AssetFile::unpack(shaders_dir / "editor.oxpack");
    if (!shader_file.has_value()) {
      return;
    }

    for (const auto& entry : shader_file->entries) {
      const auto* pipeline_data = std::get_if<ShaderPipelineData>(&entry.data);
      if (!pipeline_data) {
        continue;
      }

      render_context.create_pipeline(*pipeline_data);
    }
  }
}

ViewportPanel::~ViewportPanel() {
  auto& event_system = App::get_event_system();
  if (editor_scene) {
    std::ignore = event_system.emit<Editor::SceneStopEvent>(Editor::SceneStopEvent(editor_scene->get_id()));
  }
}

void ViewportPanel::on_render(this ViewportPanel& self, vuk::ImageAttachment swapchain_attachment) {
  ZoneScoped;

  constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_MenuBar;

  auto& editor = App::mod<Editor>();

  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.1f, 0.1f, 0.1f, 1.f));
  if (self.on_begin(flags)) {
    if (auto s = self.editor_scene->get_scene()) {
      self.runtime_console.set_scene_cvar_system(reinterpret_cast<CVarSystem*>(&s->renderer_cvar));
      self.runtime_console.render();
    }

    if (ImGui::BeginPopupContextItem("viewport context")) {
      if (ImGui::MenuItem("Unload Scene")) {
        self.editor_scene = nullptr;
        self.set_name("Viewport");
        editor.reset_current_docking_layout();
      }
      ImGui::EndPopup();
    }

    bool viewport_settings_popup = false;
    bool gizmo_settings_popup = false;
    bool snap_settings_popup = false;
    bool terrain_brush_settings_popup = false;
    bool sound_settings_popup = false;
    ImVec2 start_cursor_pos = ImGui::GetCursorPos();

    if (ImGui::BeginMenuBar()) {
      if (!self.editor_scene->is_playing()) {
        if (ImGui::MenuItem(ICON_MDI_CONTENT_SAVE)) {
          editor.save_scene();
        }
        UI::tooltip_hover("Save scene");
        if (ImGui::MenuItem(ICON_MDI_CONTENT_SAVE_MOVE)) {
          editor.save_scene_as();
        }
        UI::tooltip_hover("Save scene as");
        if (ImGui::MenuItem(ICON_MDI_COG)) {
          viewport_settings_popup = true;
        }
      }
      if (ImGui::MenuItem(ICON_MDI_INFORMATION, nullptr, self.draw_scene_stats)) {
        self.draw_scene_stats = !self.draw_scene_stats;
      }
      if (ImGui::MenuItem(ICON_MDI_SPHERE, nullptr, gizmo_settings_popup)) {
        gizmo_settings_popup = true;
      }
      if (ImGui::MenuItem(ICON_MDI_MAGNET, nullptr, snap_settings_popup)) {
        snap_settings_popup = true;
      }
      if (ImGui::MenuItem(ICON_MDI_BRUSH, nullptr, self.terrain_brush_enabled)) {
        terrain_brush_settings_popup = true;
      }

      auto& audio_engine = App::mod<AudioEngine>();
      auto sound_muted = audio_engine.get_device_volume() <= 0.f;
      const char* sound_icon = sound_muted ? ICON_MDI_VOLUME_MUTE : ICON_MDI_VOLUME_HIGH;
      if (ImGui::MenuItem(sound_icon, nullptr, sound_settings_popup)) {
        audio_engine.set_device_volume(sound_muted ? 100.f : 0.f);
      }
      UI::tooltip_hover("Left-click to toggle mute, right-click for settings");
      if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        sound_settings_popup = true;
      }

      ImGui::EndMenuBar();
    }

    ImGuiWindow* imgui_window = ImGui::GetCurrentWindow();
    ImRect menu_bar_rect = imgui_window->MenuBarRect();
    self.is_menubar_hovered = ImGui::IsMouseHoveringRect(menu_bar_rect.Min, menu_bar_rect.Max);

    self.draw_stats_overlay(self.draw_scene_stats);

    if (viewport_settings_popup)
      ImGui::OpenPopup("viewport_settings");

    ImGui::SetNextWindowSize(UI::scale(ImVec2(345.0f, 0.0f)));
    if (ImGui::BeginPopup("viewport_settings")) {
      self.draw_settings_panel();
      ImGui::EndPopup();
    }

    if (gizmo_settings_popup)
      ImGui::OpenPopup("gizmo_settings");

    ImGui::SetNextWindowSize(UI::scale(ImVec2(325.0f, 0.0f)));
    if (ImGui::BeginPopup("gizmo_settings")) {
      self.draw_gizmo_settings_panel();
      ImGui::EndPopup();
    }

    if (snap_settings_popup)
      ImGui::OpenPopup("snap_settings");

    ImGui::SetNextWindowSize(UI::scale(ImVec2(325.0f, 0.0f)));
    if (ImGui::BeginPopup("snap_settings")) {
      self.draw_snap_settings_panel();
      ImGui::EndPopup();
    }

    if (terrain_brush_settings_popup)
      ImGui::OpenPopup("terrain_brush_settings");

    ImGui::SetNextWindowSize(UI::scale(ImVec2(345.0f, 0.0f)));
    if (ImGui::BeginPopup("terrain_brush_settings")) {
      self.draw_terrain_brush_settings_panel();
      ImGui::EndPopup();
    }

    if (sound_settings_popup)
      ImGui::OpenPopup("sound_settings", ImGuiPopupFlags_MouseButtonRight);
    ImGui::SetNextWindowSize(UI::scale(ImVec2(225.0f, 0.0f)));
    if (ImGui::BeginPopup("sound_settings")) {
      self.draw_sound_settings_panel();
      ImGui::EndPopup();
    }

    const ImVec2 viewport_min_region = ImGui::GetWindowContentRegionMin();
    const ImVec2 viewport_max_region = ImGui::GetWindowContentRegionMax();
    self.viewport_position = ImGui::GetWindowPos();

    self.viewport_size = ImGui::GetContentRegionAvail();
    self.render_size = self.viewport_size;
    self.viewport_offset = {};

    // aspect ratio constraints
    if (self.viewport_aspect_ratio != AspectRatio::Auto) {
      float target_aspect = 0.0f;
      switch (self.viewport_aspect_ratio) {
        case AspectRatio::_16x9 : target_aspect = 16.0f / 9.0f; break;
        case AspectRatio::_16x10: target_aspect = 16.0f / 10.0f; break;
        case AspectRatio::_3x2  : target_aspect = 3.0f / 2.0f; break;
        case AspectRatio::_4x3  : target_aspect = 4.0f / 3.0f; break;
        case AspectRatio::_21x9 : target_aspect = 21.0f / 9.0f; break;
        case AspectRatio::_32x9 : target_aspect = 32.0f / 9.0f; break;
        case AspectRatio::_9x16 : target_aspect = 9.0f / 16.0f; break;
        default                 : break;
      }

      const float window_aspect = self.viewport_size.x / self.viewport_size.y;

      if (window_aspect > target_aspect) {
        self.render_size.x = self.viewport_size.y * target_aspect;
        self.viewport_offset.x = (self.viewport_size.x - self.render_size.x) * 0.5f;
      } else {
        self.render_size.y = self.viewport_size.x / target_aspect;
        self.viewport_offset.y = (self.viewport_size.y - self.render_size.y) * 0.5f;
      }
    }

    const auto render_scale = editor.editor_cvar.cvar_scale_viewport_size_with_content_scale.as_bool()
                                ? ImGui::GetIO().DisplayFramebufferScale.x
                                : static_cast<f32>(
                                    1u << static_cast<u32>(editor.editor_cvar.cvar_viewport_scale_amount.get())
                                  ); // 0->1, 1->2, 2->4, 3->8
    self.scaled_render_size = {
      std::round(self.render_size.x * render_scale),
      std::round(self.render_size.y * render_scale),
    };
    self.viewport_bounds_[0] = {
      viewport_min_region.x + self.viewport_position.x + self.viewport_offset.x,
      viewport_min_region.y + self.viewport_position.y + self.viewport_offset.y
    };
    self.viewport_bounds_[1] = {
      self.viewport_bounds_[0].x + self.render_size.x,
      self.viewport_bounds_[0].y + self.render_size.y
    };

    self.is_viewport_focused = ImGui::IsWindowFocused();
    self.is_viewport_hovered = ImGui::IsWindowHovered();

    if (!self.editor_scene) {
      const auto warning_text = "No scene!";
      const auto text_width = ImGui::CalcTextSize(warning_text).x;
      ImGui::SetCursorPosX((self.viewport_size.x - text_width) * 0.5f);
      ImGui::SetCursorPosY(self.viewport_size.y * 0.5f);
      ImGui::Text(warning_text);

      self.on_end();

      return;
    }

    auto renderer_instance = self.editor_scene->get_scene()->get_renderer_instance();
    if (!renderer_instance) {
      const auto warning_text = "No scene render output!";
      const auto text_width = ImGui::CalcTextSize(warning_text).x;
      ImGui::SetCursorPosX((self.viewport_size.x - text_width) * 0.5f);
      ImGui::SetCursorPosY(self.viewport_size.y * 0.5f);
      ImGui::Text(warning_text);
    } else {
      constexpr auto get_mouse_texel_coords =
        [](glm::uvec2 render_s, ImVec2 window_pos, ImVec2 content_min, ImVec2 content_max, ImVec2 mouse_pos)
        -> glm::uvec2 {
        ImVec2 rendered_min = {window_pos.x + content_min.x, window_pos.y + content_min.y};
        ImVec2 rendered_max = {window_pos.x + content_max.x, window_pos.y + content_max.y};
        ImVec2 rendered_size = {rendered_max.x - rendered_min.x, rendered_max.y - rendered_min.y};

        if (
          mouse_pos.x < rendered_min.x || mouse_pos.x > rendered_max.x || mouse_pos.y < rendered_min.y ||
          mouse_pos.y > rendered_max.y
        ) {
          return glm::uvec2(~0_u32);
        }

        glm::vec2 mouse_rel = {mouse_pos.x - rendered_min.x, mouse_pos.y - rendered_min.y};

        return glm::uvec2{
          static_cast<u32>((mouse_rel.x / rendered_size.x) * render_s.x),
          static_cast<u32>((mouse_rel.y / rendered_size.y) * render_s.y)
        };
      };

      auto mouse_pos = ImGui::GetMousePos();

      ImVec2 corrected_min_region = {
        viewport_min_region.x + self.viewport_offset.x,
        viewport_min_region.y + self.viewport_offset.y
      };
      ImVec2 corrected_max_region = {
        corrected_min_region.x + self.render_size.x,
        corrected_min_region.y + self.render_size.y
      };

      glm::uvec2 picking_texel = get_mouse_texel_coords(
        {self.scaled_render_size.x, self.scaled_render_size.y},
        self.viewport_position,
        corrected_min_region,
        corrected_max_region,
        mouse_pos
      );
      if (self.mouse_picking_enabled && !self.terrain_brush_enabled) {
        self.mouse_picking_stages(renderer_instance, picking_texel);
      }

      self.update_terrain_brush(
        picking_texel.x == ~0_u32
          ? glm::vec2(-1.0f)
          : glm::vec2(picking_texel) / glm::vec2(self.scaled_render_size.x, self.scaled_render_size.y)
      );

      if (editor.editor_cvar.cvar_draw_grid.as_bool()) {
        add_editor_grid_stage(*renderer_instance, editor.editor_cvar.cvar_draw_grid_distance.get());
      }

      auto viewport_attachment_info = swapchain_attachment;
      viewport_attachment_info.extent = vuk::Extent3D{
        static_cast<u32>(self.scaled_render_size.x),
        static_cast<u32>(self.scaled_render_size.y),
        1u,
      };
      auto viewport_attachment = vuk::declare_ia("viewport", viewport_attachment_info);
      viewport_attachment = vuk::clear_image(std::move(viewport_attachment), vuk::Black<f32>);

      auto scene_view_image = self.editor_scene->get_scene()->render(
        std::move(viewport_attachment),
        glm::ivec2{static_cast<i32>(self.viewport_bounds_[0].x), static_cast<i32>(self.viewport_bounds_[0].y)},
        glm::ivec2{static_cast<i32>(self.render_size.x), static_cast<i32>(self.render_size.y)},
        glm::ivec2{static_cast<i32>(self.scaled_render_size.x), static_cast<i32>(self.scaled_render_size.y)},
        self.is_viewport_focused
      );

      ImGui::SetCursorPos(
        {ImGui::GetCursorPosX() + self.viewport_offset.x, ImGui::GetCursorPosY() + self.viewport_offset.y}
      );
      UI::image(std::move(scene_view_image), ImVec2{self.render_size.x, self.render_size.y});

      self.drag_drop();
    }

    if (!self.editor_scene->is_playing()) {
      if (self.editor_camera.is_alive() && self.editor_camera.has<CameraComponent>()) {
        self.editor_camera.enable();

        if (!self.terrain_brush_enabled) {
          self.draw_gizmos();
        }
      }
      self.transform_gizmos_button_group(start_cursor_pos);
    }

    self.scene_button_group(start_cursor_pos);

    self.is_ui_capturing_mouse = ImGui::IsAnyItemHovered() || ImGui::IsAnyItemActive() ||
                                 ImGui::IsPopupOpen(
                                   nullptr,
                                   ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel
                                 );
  } else {
    self.is_viewport_focused = false;
    self.is_viewport_hovered = false;
  }
  ImGui::PopStyleColor();

  self.on_end();
}

auto ViewportPanel::on_update(this ViewportPanel& self) -> void {
  if (
    !self.editor_scene || !self.is_viewport_hovered || self.editor_scene->get_scene()->is_running() ||
    !self.editor_camera.has<CameraComponent>()
  ) {
    return;
  }

  auto& editor = App::mod<Editor>();

  const f32 dt = static_cast<f32>(App::get_timestep().get_seconds());

  auto& cam = self.editor_camera.get_mut<CameraComponent>();
  auto& tc = self.editor_camera.get_mut<TransformComponent>();
  const glm::vec3 position = tc.position;
  const glm::vec2 yaw_pitch = self.camera_yaw_pitch;
  glm::vec3 final_position = position;
  glm::vec2 final_yaw_pitch = yaw_pitch;

  const auto is_ortho = cam.projection == CameraComponent::Projection::Orthographic;
  if (is_ortho) {
    final_position = {0.0f, 0.0f, 0.0f};
    final_yaw_pitch = {0.f, 0.f};
  }

  const auto& window = App::get_window();

  auto& input_sys = App::mod<Input>();
  if (input_sys.get_key_pressed(ScanCode::F)) {
    auto& editor_context = editor.get_context();
    if (editor_context.entity.has_value()) {
      const auto entity_tc = editor_context.entity->get<TransformComponent>();
      auto final_pos = entity_tc.position + cam.forward;
      final_pos += -5.0f * cam.forward;
      final_position = final_pos;
    }
  }

  const auto actual_sens = editor.editor_cvar.cvar_camera_sens.get() / 10.f;
  const auto smoothed_sens = actual_sens * 100.f;
  const auto camera_sens = editor.editor_cvar.cvar_camera_smooth.get() ? smoothed_sens : actual_sens;

  const auto actual_speed = editor.editor_cvar.cvar_camera_speed.get();
  const auto smoothed_speed = actual_speed * 100.f;
  const auto camera_speed = editor.editor_cvar.cvar_camera_smooth.get() ? smoothed_speed : actual_speed;

  if ((input_sys.get_mouse_held(MouseCode::Middle) || input_sys.get_mouse_held(MouseCode::Right)) && !is_ortho) {
    const glm::vec2 new_mouse_position = input_sys.get_mouse_position_rel();
    window.set_cursor_override(WindowCursor::Crosshair);

    if (input_sys.get_mouse_moved()) {
      const glm::vec2 change = new_mouse_position * camera_sens;
      final_yaw_pitch.x -= change.x;
      final_yaw_pitch.y = glm::clamp(final_yaw_pitch.y - change.y, glm::radians(-89.9f), glm::radians(89.9f));
    }

    const float max_move_speed = camera_speed * (ImGui::IsKeyDown(ImGuiKey_LeftShift) ? 3.0f : 1.0f) * dt;

    if (input_sys.get_key_held(ScanCode::W))
      final_position += cam.forward * max_move_speed;
    else if (input_sys.get_key_held(ScanCode::S))
      final_position -= cam.forward * max_move_speed;
    if (input_sys.get_key_held(ScanCode::D))
      final_position += cam.right * max_move_speed;
    else if (input_sys.get_key_held(ScanCode::A))
      final_position -= cam.right * max_move_speed;

    if (input_sys.get_key_held(ScanCode::Q)) {
      final_position.y -= max_move_speed;
    } else if (input_sys.get_key_held(ScanCode::E)) {
      final_position.y += max_move_speed;
    }
  }
  // Panning
  else if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
    const glm::vec2 new_mouse_position = input_sys.get_mouse_position_rel();
    window.set_cursor_override(WindowCursor::ResizeAll);

    const glm::vec2 change = (new_mouse_position - self.locked_mouse_position) * 1.f;

    if (input_sys.get_mouse_moved()) {
      const float max_move_speed = camera_speed * (ImGui::IsKeyDown(ImGuiKey_LeftShift) ? 3.0f : 1.0f) * dt;
      final_position += cam.forward * change.y * max_move_speed;
      final_position += cam.right * change.x * max_move_speed;
    }
  }

  const glm::vec3 damped_position =
    math::smooth_damp(position, final_position, self.translation_velocity, self.translation_dampening, 1000.0f, dt);
  const glm::vec2 damped_yaw_pitch =
    math::smooth_damp(yaw_pitch, final_yaw_pitch, self.rotation_velocity, self.rotation_dampening, 1000.0f, dt);

  const bool smooth = editor.editor_cvar.cvar_camera_smooth.as_bool();
  tc.position = smooth ? damped_position : final_position;
  self.camera_yaw_pitch = smooth ? damped_yaw_pitch : final_yaw_pitch;
  tc.rotation = glm::quat(glm::vec3(self.camera_yaw_pitch.y, self.camera_yaw_pitch.x, 0.0f));
  cam.zoom = static_cast<float>(editor.editor_cvar.cvar_camera_zoom.get());
}

auto ViewportPanel::set_context(this ViewportPanel& self, const std::shared_ptr<EditorScene>& scene) -> void {
  OX_CHECK_NULL(scene);

  self.editor_scene = scene;

  self.set_name(fmt::format("Viewport:{}", scene->get_scene()->scene_name));

  if (!scene->is_playing()) {
    self.editor_camera = self.editor_scene->get_scene()->create_entity("editor_camera", false);
    self.editor_camera.add<CameraComponent>().add<Hidden>();
  }

  auto& event_system = App::get_event_system();
  std::ignore = event_system.emit<Editor::ViewportSceneLoadEvent>(Editor::ViewportSceneLoadEvent{});
}

auto ViewportPanel::drag_drop(this const ViewportPanel& self) -> void {
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* imgui_payload = ImGui::AcceptDragDropPayload(PayloadData::DRAG_DROP_SOURCE)) {
      const auto* payload = PayloadData::from_payload(imgui_payload);
      const auto path = payload->get_path();
      if (path.extension() == ".gltf" || path.extension() == ".glb") {
        auto& job_man = App::get_job_manager();
        job_man.push_job_name("ViewportPanel_ImportModel");
        job_man.submit(Job::create([path, scene = self.editor_scene->get_scene()]() {
          auto asset = App::mod<AssetManager>().import_asset(path);
          if (!asset) {
            return;
          }

          App::defer_to_next_frame([scene, asset]() { scene->create_model_entity_async(asset); });
        }));
        job_man.pop_job_name();
      }
    }

    ImGui::EndDragDropTarget();
  }
}

auto ViewportPanel::draw_stats_overlay(this const ViewportPanel& self, bool draw) -> void {
  if (!self.performance_overlay_visible || !self.editor_scene)
    return;
  auto work_pos = ImVec2(self.viewport_position.x, self.viewport_position.y);
  auto work_size = ImVec2(self.viewport_size.x, self.viewport_size.y);
  const auto padding = UI::scale(ImVec2(15.0f, 55.0f));

  ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                                  ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                  ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
  ImVec2 window_pos, window_pos_pivot;
  window_pos.x = work_pos.x + work_size.x - padding.x;
  window_pos.y = work_pos.y + padding.y;
  window_pos_pivot.x = 1.0f;
  window_pos_pivot.y = 0.0f;
  ImGui::SetNextWindowPos(window_pos, ImGuiCond_Always, window_pos_pivot);
  ImGui::SetNextWindowBgAlpha(0.35f);
  ImGui::SetNextWindowSize(draw ? UI::scale(ImVec2(220.0f, 0.0f)) : UI::scale(ImVec2(120.0f, 5.0f)), ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, UI::scale(2.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  auto overlay_id = fmt::format("{}_overlay", self.get_id());
  if (ImGui::Begin(overlay_id.c_str(), nullptr, window_flags)) {
    ImGui::Text("%.1f FPS (%.1f ms)", ImGui::GetIO().Framerate, 1000.0f / ImGui::GetIO().Framerate);
    if (draw) {
      ImGui::Text("Scripts in scene: %zu", self.editor_scene->get_scene()->get_lua_systems().size());
      const auto transform_entities_count = self.editor_scene->get_scene()->world.count<TransformComponent>();
      ImGui::Text("Entities with transforms: %d", transform_entities_count);
      const auto mesh_entities_count = self.editor_scene->get_scene()->world.count<MeshComponent>();
      ImGui::Text("Entities with mesh: %d", mesh_entities_count);
      const auto light_entities_count = self.editor_scene->get_scene()->world.count<LightComponent>();
      ImGui::Text("Entities with light: %d", light_entities_count);
      const auto sprite_entities_count = self.editor_scene->get_scene()->world.count<SpriteComponent>();
      ImGui::Text("Entities with sprite: %d", sprite_entities_count);
      const auto particle_emitter_count = self.editor_scene->get_scene()->world.count<ParticleSystemComponent>();
      ImGui::Text("Particle emitters: %d", particle_emitter_count);
      const auto rigidbody_entities_count = self.editor_scene->get_scene()->world.count<RigidBodyComponent>();
      ImGui::Text("Entities with rigidbody: %d", rigidbody_entities_count);
      const auto audio_entities_count = self.editor_scene->get_scene()->world.count<AudioSourceComponent>();
      ImGui::Text("Entities with audio: %d", audio_entities_count);
    }
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
}

auto ViewportPanel::draw_settings_panel(this ViewportPanel& self) -> void {
  ZoneScoped;

  i32 open_action = -1;

  auto is_scene_valid = self.editor_scene->is_valid();

  auto& context_cvar = App::get_rendercontext().context_cvar;
  auto& editor_cvar = App::mod<Editor>().editor_cvar;

  if (UI::button("Expand All")) {
    open_action = 1;
  }
  ImGui::SameLine();
  if (UI::button("Collapse All")) {
    open_action = 0;
  }
  ImGui::SameLine();
  if (UI::button("Reset to defaults")) {
    if (is_scene_valid) {
      auto& cvar_sys = self.editor_scene->get_scene()->renderer_cvar;
      cvar_sys.cvar_enable_debug_renderer.set_default();
      cvar_sys.cvar_enable_physics_debug_renderer.set_default();
      cvar_sys.cvar_draw_bounding_boxes.set_default();
      cvar_sys.cvar_draw_camera_frustum.get_default();
      cvar_sys.cvar_bloom_enable.set_default();
      cvar_sys.cvar_bloom_threshold.set_default();
      cvar_sys.cvar_bloom_soft_threshold.set_default();
      cvar_sys.cvar_bloom_radius.set_default();
      cvar_sys.cvar_bloom_intensity.set_default();
      cvar_sys.cvar_bloom_clamp.set_default();
      cvar_sys.cvar_fxaa_enable.set_default();
      cvar_sys.cvar_vbgtao_quality_level.set_default();
      cvar_sys.cvar_vbgtao_radius.set_default();
      cvar_sys.cvar_vbgtao_thickness.set_default();
      cvar_sys.cvar_vbgtao_final_power.set_default();
      cvar_sys.cvar_rtao_enable.set_default();
      cvar_sys.cvar_rtao_ray_count.set_default();
      cvar_sys.cvar_rtao_radius.set_default();
      cvar_sys.cvar_rtao_power.set_default();
      cvar_sys.cvar_contact_shadows_enabled.set_default();
      cvar_sys.cvar_contact_shadows_steps.set_default();
      cvar_sys.cvar_contact_shadows_thickness.set_default();
      cvar_sys.cvar_contact_shadows_length.set_default();
    }
    editor_cvar.cvar_camera_sens.set_default();
    editor_cvar.cvar_camera_speed.set_default();
    editor_cvar.cvar_camera_smooth.set_default();
    editor_cvar.cvar_camera_zoom.set_default();
  }

  constexpr ImGuiTreeNodeFlags TREE_FLAGS = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowOverlap |
                                            ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_FramePadding;

  if (open_action != -1)
    ImGui::SetNextItemOpen(open_action != 0);
  if (ImGui::TreeNodeEx("Renderer", TREE_FLAGS, "%s", "Renderer")) {
    auto& render_context = App::get_rendercontext();
    ImGui::Text("GPU: %s", render_context.device_name.c_str());
    ImGui::Text(
      "Swapchain: %dx%d",
      static_cast<u32>(render_context.swapchain_extent.x),
      static_cast<u32>(render_context.swapchain_extent.y)
    );
    auto& window = App::get_window();
    ImGui::Text(
      "Window: %dx%d@%.1fhz x%.1f",
      window.get_logical_width(),
      window.get_logical_height(),
      window.get_refresh_rate(),
      window.get_dpi_scale()
    );
    if (UI::begin_properties(UI::default_properties_flags, true, 0.3f)) {
      UI::property("VSync", context_cvar.cvar_vsync.get_ptr_bool());
      const auto has_mesh_shaders = render_context.features & RenderContext::Feature::MeshShaders;
      ImGui::BeginDisabled(!has_mesh_shaders);
      UI::property(
        "Mesh shaders",
        context_cvar.cvar_mesh_shaders.get_ptr_bool(),
        has_mesh_shaders ? "Draw geometry with the mesh shader pipeline instead of the compute one"
                         : "This device does not support VK_EXT_mesh_shader"
      );
      ImGui::EndDisabled();
      UI::end_properties();
    }

    if (open_action != -1)
      ImGui::SetNextItemOpen(open_action != 0);
    if (is_scene_valid) {
      auto& cvar_sys = self.editor_scene->get_scene()->renderer_cvar;
      if (ImGui::TreeNodeEx("Debug", TREE_FLAGS, "%s", "Debug")) {
        if (UI::begin_properties(UI::default_properties_flags, true, 0.3f)) {
          UI::property("Enable debug renderer", cvar_sys.cvar_enable_debug_renderer.get_ptr_bool());
          ImGui::Indent();
          ImGui::BeginDisabled(!cvar_sys.cvar_enable_debug_renderer.as_bool());
          UI::property("Draw bounding boxes", cvar_sys.cvar_draw_bounding_boxes.get_ptr_bool());
          UI::property("Draw camera frustum", cvar_sys.cvar_draw_camera_frustum.get_ptr_bool());
          const char* debug_views[] = {
            "None",
            "Triangles",
            "Meshlets",
            "Overdraw",
            "Materials",
            "Mesh Instances",
            "Mesh Lods",
            "Albdeo",
            "Normal",
            "Emissive",
            "Metallic",
            "Roughness",
            "Baked Occlusion",
            "GTAO",
            "Geometric Normal",
            "Virtual Shadowmaps",
            "Virtual Shadowmaps (Point/Spot)",
            "DDGI Probes"
          };
          UI::property(
            "Debug View",
            cvar_sys.cvar_debug_view.get_ptr(),
            debug_views,
            static_cast<i32>(ox::count_of(debug_views))
          );
          ImGui::EndDisabled();
          ImGui::Unindent();

          UI::property("Enable physics debug renderer", cvar_sys.cvar_enable_physics_debug_renderer.get_ptr_bool());
          UI::property("Freeze culling frustum", cvar_sys.cvar_freeze_culling_frustum.get_ptr_bool());
          UI::property("Enable frustum culling", cvar_sys.cvar_culling_frustum.get_ptr_bool());
          UI::property("Enable occlusion culling", cvar_sys.cvar_culling_occlusion.get_ptr_bool());
          UI::property("Enable triangle culling", cvar_sys.cvar_culling_triangle.get_ptr_bool());
          UI::end_properties();
        }

        ImGui::TreePop();
      }

      if (open_action != -1)
        ImGui::SetNextItemOpen(open_action != 0);
      if (ImGui::TreeNodeEx("Bloom", TREE_FLAGS, "%s", "Bloom")) {
        if (UI::begin_properties(UI::default_properties_flags, true, 0.3f)) {
          UI::property("Enabled", cvar_sys.cvar_bloom_enable.get_ptr_bool());
          UI::property<float>("Threshold", cvar_sys.cvar_bloom_threshold.get_ptr(), 0.0f, 100.0f);
          UI::property<float>("Soft Threshold", cvar_sys.cvar_bloom_soft_threshold.get_ptr(), 0.0f, 1.0f);
          UI::property<float>("Radius", cvar_sys.cvar_bloom_radius.get_ptr(), 0.0f, 1.0f);
          UI::property<float>("Intensity", cvar_sys.cvar_bloom_intensity.get_ptr(), 0.0f, 1.0f);
          UI::property<float>("Clamp", cvar_sys.cvar_bloom_clamp.get_ptr(), 1.0f, 64.0f);
          UI::end_properties();
        }
        ImGui::TreePop();
      }

      if (open_action != -1)
        ImGui::SetNextItemOpen(open_action != 0);
      if (ImGui::TreeNodeEx("FXAA", TREE_FLAGS, "%s", "FXAA")) {
        if (UI::begin_properties(UI::default_properties_flags, true, 0.3f)) {
          UI::property("Enabled", cvar_sys.cvar_fxaa_enable.get_ptr_bool());
          UI::end_properties();
        }
        ImGui::TreePop();
      }

      if (open_action != -1)
        ImGui::SetNextItemOpen(open_action != 0);
      if (ImGui::TreeNodeEx("GTAO", TREE_FLAGS, "%s", "GTAO")) {
        if (UI::begin_properties(UI::default_properties_flags, true, 0.3f)) {
          UI::property("Enabled", cvar_sys.cvar_vbgtao_enable.get_ptr_bool());
          const char* quality_levels[4] = {"Low", "Medium", "High", "Ultra"};
          UI::property("Quality Level", cvar_sys.cvar_vbgtao_quality_level.get_ptr(), quality_levels, 4);
          UI::property<float>("Radius", cvar_sys.cvar_vbgtao_radius.get_ptr(), 0.1f, 5.f);
          UI::property<float>("Thickness", cvar_sys.cvar_vbgtao_thickness.get_ptr(), 0.0f, 5.f);
          UI::property<float>("Final Power", cvar_sys.cvar_vbgtao_final_power.get_ptr(), 0.f, 10.f);
          UI::end_properties();
        }
        ImGui::TreePop();
      }

      if (open_action != -1)
        ImGui::SetNextItemOpen(open_action != 0);
      if (ImGui::TreeNodeEx("RTAO", TREE_FLAGS, "%s", "RTAO (DO NOT USE, TESTING ONLY)")) {
        const auto has_ray_tracing = render_context.features & RenderContext::Feature::RayTracing;
        ImGui::BeginDisabled(!has_ray_tracing);
        if (UI::begin_properties(UI::default_properties_flags, true, 0.3f)) {
          UI::property(
            "Enabled",
            cvar_sys.cvar_rtao_enable.get_ptr_bool(),
            has_ray_tracing ? "Trace occlusion rays against the scene TLAS instead of running GTAO"
                            : "This device does not support ray queries"
          );
          UI::property("Ray Count", cvar_sys.cvar_rtao_ray_count.get_ptr(), 1, 32);
          UI::property<float>("Radius", cvar_sys.cvar_rtao_radius.get_ptr(), 0.05f, 20.f);
          UI::property<float>("Power", cvar_sys.cvar_rtao_power.get_ptr(), 0.f, 10.f);
          UI::end_properties();
        }
        ImGui::EndDisabled();
        ImGui::TreePop();
      }

      if (open_action != -1)
        ImGui::SetNextItemOpen(open_action != 0);
      if (ImGui::TreeNodeEx("DDGI", TREE_FLAGS, "%s", "DDGI")) {
        const auto has_ray_tracing = (render_context.features & RenderContext::Feature::RayTracing) &&
                                     (render_context.features & RenderContext::Feature::RayTracingPipeline);
        ImGui::BeginDisabled(!has_ray_tracing);
        if (UI::begin_properties(UI::default_properties_flags, true, 0.3f)) {
          UI::property(
            "Enabled",
            cvar_sys.cvar_ddgi_enable.get_ptr_bool(),
            has_ray_tracing ? "Light probe volumes gather indirect diffuse by tracing the scene TLAS"
                            : "This device does not support ray tracing pipelines"
          );
          UI::property(
            "Rays Per Probe",
            cvar_sys.cvar_ddgi_rays_per_probe.get_ptr(),
            8,
            512,
            1.0f,
            "More rays converge faster and flicker less, at a linear cost"
          );
          UI::property<float>(
            "Max Ray Distance (Cascade 0)",
            cvar_sys.cvar_ddgi_max_ray_distance.get_ptr(),
            1.f,
            500.f
          );
          UI::property<float>(
            "Max Ray Radiance",
            cvar_sys.cvar_ddgi_max_ray_radiance.get_ptr(),
            0.1f,
            100.f,
            "Luminance cap per probe ray. Lower it to stop a bright emitter from making probes flicker"
          );
          UI::property<float>(
            "Hysteresis",
            cvar_sys.cvar_ddgi_hysteresis.get_ptr(),
            0.f,
            0.99f,
            "How much of a probe's history survives each update"
          );
          UI::property(
            "Max Update Interval",
            cvar_sys.cvar_ddgi_update_max_interval.get_ptr(),
            1,
            64,
            1.0f,
            "Most frames a probe may go without being retraced. 1 retraces every probe every frame"
          );
          UI::property<float>(
            "Full Rate Distance",
            cvar_sys.cvar_ddgi_update_full_rate_distance.get_ptr(),
            1.f,
            200.f,
            "Probes within this distance of the camera retrace every frame"
          );
          UI::property(
            "Distance Culling",
            cvar_sys.cvar_ddgi_distance_culling.get_ptr_bool(),
            "Trace mesh-distant probes only for staggered rechecks and reuse cached probe radiance for far ray hits"
          );
          UI::property(
            "Probe Relocation",
            cvar_sys.cvar_ddgi_probe_relocation.get_ptr_bool(),
            "Move probes out of geometry they are buried in, and drop the ones that stay stuck"
          );
          UI::property<float>(
            "Min Frontface Distance",
            cvar_sys.cvar_ddgi_min_frontface_distance.get_ptr(),
            0.f,
            5.f,
            "How far relocation keeps a probe off a surface, in world units"
          );
          UI::property<float>(
            "Shadow Bias",
            cvar_sys.cvar_ddgi_shadow_ray_offset.get_ptr(),
            0.f,
            1.f,
            "Offsets a probe ray hit for ray-traced and virtual-shadow-map visibility tests, in world units"
          );
          UI::property<float>(
            "Normal Bias",
            cvar_sys.cvar_ddgi_normal_bias.get_ptr(),
            0.f,
            1.f,
            "Offsets the probe lookup along the surface normal, in probe spacings"
          );
          UI::property<float>(
            "View Bias",
            cvar_sys.cvar_ddgi_view_bias.get_ptr(),
            0.f,
            1.f,
            "Offsets the probe lookup toward the camera, in probe spacings"
          );
          UI::property<float>(
            "Max Brightness Step",
            cvar_sys.cvar_ddgi_max_brightness_step.get_ptr(),
            0.f,
            2.f,
            "How far a probe may brighten in one update before the step is quartered, tames emissive flicker"
          );
          UI::property<float>("Intensity", cvar_sys.cvar_ddgi_intensity.get_ptr(), 0.f, 10.f);
          UI::property<float>(
            "Debug Probe Radius",
            cvar_sys.cvar_ddgi_probe_debug_radius.get_ptr(),
            0.01f,
            2.f,
            "Size of the spheres drawn by the DDGI Probes debug view"
          );
          UI::end_properties();
        }
        ImGui::EndDisabled();
        ImGui::TreePop();
      }

      if (open_action != -1)
        ImGui::SetNextItemOpen(open_action != 0);
      if (ImGui::TreeNodeEx("Contact Shadows", TREE_FLAGS, "%s", "Contact Shadows")) {
        if (UI::begin_properties(UI::default_properties_flags, true, 0.3f)) {
          UI::property("Enabled", cvar_sys.cvar_contact_shadows_enabled.get_ptr_bool());
          UI::property("Steps", cvar_sys.cvar_contact_shadows_steps.get_ptr(), 1, 64);
          UI::property<float>("Thickness", cvar_sys.cvar_contact_shadows_thickness.get_ptr(), 0.0, 5);
          UI::property<float>("Length", cvar_sys.cvar_contact_shadows_length.get_ptr(), 0.0, 5);
          UI::end_properties();
        }
        ImGui::TreePop();
      }
    }

    ImGui::TreePop();
  }

  if (open_action != -1)
    ImGui::SetNextItemOpen(open_action != 0);
  if (ImGui::TreeNodeEx("Viewport", TREE_FLAGS, "%s", "Viewport")) {
    auto resolution = fmt::format("Viewport Resolution: {}x{}", self.scaled_render_size.x, self.scaled_render_size.y);
    ImGui::TextUnformatted(resolution.c_str());
    if (UI::begin_properties(UI::default_properties_flags, true, 0.3f)) {
      UI::property(
        "Scale Viewport With Pixel Density",
        editor_cvar.cvar_scale_viewport_size_with_content_scale.get_ptr_bool()
      );
      const char* scale_amounts[4] = {
        "1x",
        "2x",
        "4x",
        "8x",
      };
      ImGui::BeginDisabled(editor_cvar.cvar_scale_viewport_size_with_content_scale.as_bool());
      UI::property("Scale Viewport", (editor_cvar.cvar_viewport_scale_amount.get_ptr()), scale_amounts, 4);
      ImGui::EndDisabled();
      const char* aspect_ratios[8] = {
        "Auto",
        "16x9",
        "16x10",
        "3x2",
        "4x3",
        "21x9",
        "32x9",
        "9x16",
      };
      UI::property("Aspect Ratio", ((i32*)&self.viewport_aspect_ratio), aspect_ratios, 8);
      UI::end_properties();
    }

    if (open_action != -1)
      ImGui::SetNextItemOpen(open_action != 0);
    if (ImGui::TreeNodeEx("Camera", TREE_FLAGS, "%s", "Camera")) {
      if (UI::begin_properties(UI::default_properties_flags, true, 0.3f)) {
        UI::property<float>("Camera sensitivity", editor_cvar.cvar_camera_sens.get_ptr(), 0.01f, 20.0f);
        UI::property<float>("Movement speed", editor_cvar.cvar_camera_speed.get_ptr(), 0.1f, 100.0f);
        UI::property("Smooth camera", editor_cvar.cvar_camera_smooth.get_ptr_bool());
        UI::property("Camera zoom", editor_cvar.cvar_camera_zoom.get_ptr(), 1, 100);
        UI::end_properties();
      }

      ImGui::TreePop();
    }

    ImGui::TreePop();
  }
}

auto ViewportPanel::draw_gizmo_settings_panel(this ViewportPanel& self) -> void {
  auto& editor_cvar = App::mod<Editor>().editor_cvar;

  if (UI::begin_properties(UI::default_properties_flags, true, 0.3f)) {
    UI::property("Draw grid", editor_cvar.cvar_draw_grid.get_ptr_bool());
    UI::property<f32>("Grid distance", editor_cvar.cvar_draw_grid_distance.get_ptr(), 10.f, 10000.0f);

    UI::property("Draw Component Gizmos", &self.draw_component_gizmos);
    UI::property("Component Gizmos Size", &self.gizmo_icon_size);
    UI::property("Entity Highlighting", &self.draw_entity_highlighting);
    UI::end_properties();
  }
}

auto ViewportPanel::draw_snap_settings_panel(this ViewportPanel& self) -> void {
  if (UI::begin_properties(UI::default_properties_flags, true, 0.3f)) {
    UI::property("Use Snap", &self.use_snap);
    UI::property<f32>("Snap Step", &self.snap_amount, 0.5f, 100.0f, nullptr, 0.5f, "%.1f");
    UI::property<f32>("Rotate Snap Step", &self.snap_amount, 45.f, 360.0f, nullptr, 0.5f, "%.1f");
    UI::end_properties();
  }
}

auto ViewportPanel::draw_terrain_brush_settings_panel(this ViewportPanel& self) -> void {
  auto& brush = self.terrain_brush;
  auto* scene = self.editor_scene ? self.editor_scene->get_scene().get() : nullptr;
  const auto* terrain = scene ? scene->terrain.get() : nullptr;

  if (UI::begin_properties(UI::default_properties_flags, true, 0.3f)) {
    UI::property("Enabled", &self.terrain_brush_enabled);

    const char* modes[] = {"Raise", "Smooth", "Flatten", "Noise", "Paint Layer"};
    UI::property("Mode", reinterpret_cast<int*>(&brush.mode), modes, 5);

    UI::property<f32>("Radius", &brush.radius_world, 0.5f, 4096.0f, "Brush footprint in world units.", 0.5f, "%.1f m");

    const auto displaces = brush.mode == GPU::TerrainBrushMode::Raise || brush.mode == GPU::TerrainBrushMode::Noise;
    if (displaces) {
      UI::property<f32>(
        "Rate",
        &brush.height_rate,
        0.05f,
        64.0f,
        "How far the surface moves per second of stroke, in world units.",
        0.05f,
        "%.2f m/s"
      );
    } else {
      UI::property<f32>(
        "Rate",
        &brush.blend_rate,
        0.05f,
        16.0f,
        "How fast the stroke converges on its target, per second.",
        0.05f,
        "%.2f /s"
      );
    }

    UI::property<
      f32>("Falloff", &brush.falloff, 1.0f, 8.0f, "Higher values tighten the brush toward its center.", 0.02f, "%.2f");

    if (brush.mode == GPU::TerrainBrushMode::Flatten) {
      const auto min_height = terrain ? terrain->base_height() : 0.0f;
      const auto max_height = terrain ? min_height + terrain->height_scale() : 1.0f;
      UI::property<f32>(
        "Target Height",
        &brush.flatten_height_world,
        min_height,
        max_height,
        "World-space height the stroke flattens toward.",
        0.25f,
        "%.2f m"
      );
    }

    if (brush.mode == GPU::TerrainBrushMode::PaintLayer) {
      const char* layers[] = {"Grass", "Rock", "Drainage", "Snow"};
      UI::property("Layer", reinterpret_cast<int*>(&brush.layer), layers, 4);
    }

    UI::end_properties();
  }

  ImGui::TextWrapped("Left click to paint, hold Shift to invert a Raise stroke.");

  ImGui::BeginDisabled(terrain == nullptr);
  if (ImGui::Button("Reset Sculpt & Paint")) {
    scene->clear_terrain_edits();
  }
  ImGui::EndDisabled();
  UI::tooltip_hover("Discards every brush stroke and rebuilds the terrain from its parameters alone.");
}

auto ViewportPanel::update_terrain_brush(this ViewportPanel& self, glm::vec2 viewport_uv) -> void {
  ZoneScoped;

  auto* scene = self.editor_scene ? self.editor_scene->get_scene().get() : nullptr;
  if (scene == nullptr || scene->terrain == nullptr) {
    return;
  }

  // The tunables live on the panel; only the per-frame cursor state is filled in here.
  auto& brush = scene->terrain->brush;
  brush = self.terrain_brush;
  brush.active = false;
  brush.painting = false;

  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    self.terrain_stroke_active = false;
  }

  const auto tool_ready = self.terrain_brush_enabled && !self.editor_scene->is_playing() &&
                          self.editor_camera.is_alive() && self.editor_camera.has<CameraComponent>();
  if (!tool_ready) {
    self.terrain_stroke_active = false;
    return;
  }

  // Dragging on the viewport hands ImGui an active id, which flips `IsWindowHovered` off and
  // `IsAnyItemActive` on for every frame after the click. So the hover gate only decides whether a
  // stroke may *begin*; once begun it runs until the button comes back up.
  const auto can_begin = self.is_viewport_hovered && !self.is_ui_capturing_mouse && !self.is_menubar_hovered &&
                         !ImGuizmo::IsUsing() && !ImGuizmo::IsOver();
  if (can_begin && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    self.terrain_stroke_active = true;
  }

  // Leaving the rendered area gives us no ray to trace, but the stroke stays latched so coming back
  // resumes it rather than forcing another click.
  if (viewport_uv.x < 0.0f || viewport_uv.x > 1.0f || viewport_uv.y < 0.0f || viewport_uv.y > 1.0f) {
    return;
  }

  const auto& camera = self.editor_camera.get<CameraComponent>();

  // The projection is reversed-Z and already carries the Vulkan Y flip, so the near plane is at
  // ndc z = 1 and ndc y follows the cursor directly.
  const auto ndc = viewport_uv * 2.0f - 1.0f;
  auto near_point = camera.get_inverse_projection_view() * glm::vec4(ndc, 1.0f, 1.0f);
  near_point /= near_point.w;

  brush.ray_origin = camera.position;
  brush.ray_direction = glm::normalize(glm::vec3(near_point) - camera.position);
  brush.active = can_begin || self.terrain_stroke_active;
  brush.painting = self.terrain_stroke_active;
  brush.invert = ImGui::GetIO().KeyShift;
}

auto ViewportPanel::draw_sound_settings_panel(this ViewportPanel& self) -> void {
  ZoneScoped;

  if (UI::begin_properties(UI::default_properties_flags, true, 0.3f)) {
    auto& audio_engine = App::mod<AudioEngine>();
    self.volume_level = audio_engine.get_device_volume();
    if (UI::property<f32>("Volume", &self.volume_level, 0.0f, 100.0f, nullptr, 1.0f, "%.0f")) {
      audio_engine.set_device_volume(self.volume_level);
    }
    UI::end_properties();
  }
}

void ViewportPanel::draw_gizmos(this ViewportPanel& self) {
  ZoneScoped;

  auto& editor = App::mod<Editor>();
  auto& editor_context = editor.get_context();
  auto& undo_redo_system = editor.undo_redo_system;

  const auto& cam = self.editor_camera.get<CameraComponent>();
  auto projection = cam.get_projection_matrix();
  projection[1][1] *= -1;
  glm::mat4 view_proj = projection * cam.get_view_matrix();
  const Frustum& frustum = Camera::get_frustum(cam, cam.position);

  if (self.draw_component_gizmos) {
    const GizmoInfo gizmo_info = {
      self.gizmo_icon_size,
      self.render_size.x,
      self.render_size.y,
      self.viewport_offset.x,
      self.viewport_offset.y,
      view_proj,
      frustum,
    };
    show_component_gizmo<LightComponent>(
      gizmo_info,
      "LightComponent",
      self.editor_scene->get_scene().get(),
      [](const char* component_icon, const LightComponent& c) {
        switch (c.type) {
          case LightComponent::Directional: return ICON_MDI_WEATHER_SUNNY;
          case LightComponent::Spot       : return ICON_MDI_SPOTLIGHT;
          case LightComponent::Point      : return component_icon;
        }

        return component_icon;
      }
    );
    show_component_gizmo<AudioSourceComponent>(
      gizmo_info,
      "AudioSourceComponent",
      self.editor_scene->get_scene().get(),
      [](const char* component_icon, const AudioSourceComponent& c) { return component_icon; }
    );
    show_component_gizmo<AudioListenerComponent>(
      gizmo_info,
      "AudioListenerComponent",
      self.editor_scene->get_scene().get(),
      [](const char* component_icon, const AudioListenerComponent& c) { return component_icon; }
    );
    show_component_gizmo<CameraComponent>(
      gizmo_info,
      "CameraComponent",
      self.editor_scene->get_scene().get(),
      [](const char* component_icon, const CameraComponent& c) { return component_icon; }
    );
    show_component_gizmo<ParticleSystemComponent>(
      gizmo_info,
      "ParticleSystemComponent",
      self.editor_scene->get_scene().get(),
      [](const char* component_icon, const ParticleSystemComponent& c) { return component_icon; }
    );
  }

  const flecs::entity selected_entity = editor_context.entity.value_or(flecs::entity::null());

  auto& input_sys = App::mod<Input>();
  if (input_sys.get_key_held(ScanCode::LeftControl)) {
    if (input_sys.get_key_pressed(ScanCode::Q)) {
      if (!ImGuizmo::IsUsing())
        self.gizmo_type = -1;
    }
    if (input_sys.get_key_pressed(ScanCode::W)) {
      if (!ImGuizmo::IsUsing())
        self.gizmo_type = ImGuizmo::OPERATION::TRANSLATE;
    }
    if (input_sys.get_key_pressed(ScanCode::E)) {
      if (!ImGuizmo::IsUsing())
        self.gizmo_type = ImGuizmo::OPERATION::ROTATE;
    }
    if (input_sys.get_key_pressed(ScanCode::R)) {
      if (!ImGuizmo::IsUsing())
        self.gizmo_type = ImGuizmo::OPERATION::SCALE;
    }
  }

  if (selected_entity == flecs::entity::null() || !self.editor_camera.has<CameraComponent>())
    return;

  if (self.gizmo_type == -1)
    return;

  if (auto* tc = selected_entity.try_get_mut<TransformComponent>()) {
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(
      self.viewport_bounds_[0].x,
      self.viewport_bounds_[0].y,
      self.viewport_bounds_[1].x - self.viewport_bounds_[0].x,
      self.viewport_bounds_[1].y - self.viewport_bounds_[0].y
    );

    auto camera_projection = cam.get_projection_matrix();
    camera_projection[1][1] *= -1;

    const glm::mat4& camera_view = cam.get_view_matrix();

    glm::mat4 transform = Scene::get_world_transform(selected_entity);

    // Snapping
    const bool snap = input_sys.get_key_held(ScanCode::LeftControl) || self.use_snap;
    float snap_value = self.snap_amount;
    if (self.gizmo_type == ImGuizmo::OPERATION::ROTATE)
      snap_value = self.rotate_snap_amount;

    const float snap_values[3] = {snap_value, snap_value, snap_value};

    const auto is_ortho = cam.projection == CameraComponent::Projection::Orthographic;
    ImGuizmo::SetOrthographic(is_ortho);
    if (self.gizmo_mode == ImGuizmo::OPERATION::TRANSLATE && is_ortho)
      self.gizmo_mode = ImGuizmo::OPERATION::TRANSLATE_X | ImGuizmo::OPERATION::TRANSLATE_Y;

    auto delta_mat = glm::mat4(1.0f);
    ImGuizmo::Manipulate(
      value_ptr(camera_view),
      value_ptr(camera_projection),
      static_cast<ImGuizmo::OPERATION>(self.gizmo_type),
      static_cast<ImGuizmo::MODE>(self.gizmo_mode),
      value_ptr(transform),
      glm::value_ptr(delta_mat),
      snap ? snap_values : nullptr
    );

    if (ImGuizmo::IsUsing()) {
      glm::vec3 delta_translation;
      glm::quat delta_rotation;
      glm::vec3 delta_scale;
      glm::vec3 skew;
      glm::vec4 perspective;

      if (glm::decompose(delta_mat, delta_scale, delta_rotation, delta_translation, skew, perspective)) {
        const flecs::entity parent = selected_entity.parent();
        const glm::mat4 parent_world = parent != flecs::entity::null() //
                                         ? Scene::get_world_transform(parent)
                                         : glm::mat4(1.0f);

        const glm::mat4 inv_parent = glm::inverse(parent_world);
        if (self.gizmo_type == ImGuizmo::TRANSLATE) {
          tc->position += glm::vec3(inv_parent * glm::vec4(delta_translation, 0.0f));
        } else if (self.gizmo_type == ImGuizmo::ROTATE) {
          tc->rotation = glm::quat_cast(inv_parent) * delta_rotation * tc->rotation;
        } else if (self.gizmo_type == ImGuizmo::SCALE) {
          tc->scale *= delta_scale;
        }

        auto old_tc = *tc;
        undo_redo_system->execute_command<ComponentChangeCommand<TransformComponent>>(
          selected_entity,
          tc,
          old_tc,
          *tc,
          "gizmo transform"
        );

        selected_entity.modified<TransformComponent>();
      }
    }
  }
}

// The visbuffer stores terrain as a single reserved instance id with no meshlet behind it, so the
// transform of the entity owning the terrain has to be supplied to the shaders separately.
static auto get_terrain_transform_index(const Scene* scene) -> u32 {
  if (scene == nullptr || !scene->terrain_entity) {
    return ~0_u32;
  }

  auto transform_id = scene->get_entity_transform_id(scene->terrain_entity);
  if (!transform_id.has_value()) {
    return ~0_u32;
  }

  return SlotMap_decode_id(*transform_id).index;
}

static auto pick_entity(EditorScene* s, u32 transform_index) -> void {
  ZoneScoped;

  auto& editor = App::mod<Editor>();
  if (transform_index != ~0_u32) {
    if (s->get_scene()->transform_index_entities_map.contains(transform_index)) {
      auto& editor_context = editor.get_context();

      // first pick the parent if parent is already picked then pick the actual entity
      auto entity = s->get_scene()->transform_index_entities_map.at(transform_index);
      auto top_parent = entity;
      while (top_parent.parent() != flecs::entity::null()) {
        top_parent = top_parent.parent();
      }
      if (editor_context.entity.has_value()) {
        if (editor_context.entity.value() == top_parent) {
          top_parent = entity;
        }
      }

      editor_context.reset(EditorContext::Type::Entity, nullopt, top_parent);
    }
  } else {
    auto& editor_context = editor.get_context();
    editor_context.reset();
  }
}

auto highlight_mask_stage(
  RenderStageContext& ctx, const std::vector<u32>& transform_indices, u32 terrain_transform_index
) -> void {
  ZoneScoped;

  auto selected_count = static_cast<u32>(transform_indices.size());

  auto mask_generation_pass = vuk::make_pass(
    "stencil_mask",
    [selected_count, terrain_transform_index](
      vuk::CommandBuffer& cmd_list,
      VUK_IA(vuk::eComputeWrite) mask,
      VUK_IA(vuk::eComputeSampled) visbuffer,
      VUK_BA(vuk::eComputeRead) meshlet_instances,
      VUK_BA(vuk::eComputeRead) mesh_instances,
      VUK_BA(vuk::eComputeRead) transform_indices_buffer_
    ) {
      cmd_list.bind_compute_pipeline("highlight_mask_generate")
        .bind_buffer(0, 0, meshlet_instances)
        .bind_buffer(0, 1, mesh_instances)
        .bind_image(0, 2, visbuffer)
        .bind_image(0, 3, mask)
        .bind_buffer(0, 4, transform_indices_buffer_)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(selected_count, terrain_transform_index))
        .dispatch_invocations_per_pixel(mask);

      return std::make_tuple(mask, visbuffer, meshlet_instances, mesh_instances, transform_indices_buffer_);
    }
  );

  auto depth_attachment = ctx.get_image_resource("depth_attachment");
  auto visbuffer_attachment = ctx.get_image_resource("visbuffer_attachment");
  auto meshlet_instances_buffer = ctx.get_buffer_resource("meshlet_instances_buffer");
  auto mesh_instances_buffer = ctx.get_buffer_resource("mesh_instances_buffer");

  auto silhouette_mask = vuk::declare_ia(
    "selection_silhouette_mask",
    {.usage = vuk::ImageUsageFlagBits::eStorage | vuk::ImageUsageFlagBits::eSampled,
     .format = vuk::Format::eR8Unorm,
     .sample_count = vuk::Samples::e1,
     .base_level = 0,
     .level_count = 1,
     .base_layer = 0,
     .layer_count = 1}
  );
  silhouette_mask.same_extent_as(visbuffer_attachment);
  silhouette_mask = vuk::clear_image(std::move(silhouette_mask), vuk::Black<f32>);

  size_t buffer_size = transform_indices.size() * sizeof(u32);
  auto transform_indices_buffer = ctx.render_context.alloc_transient_buffer(vuk::MemoryUsage::eCPUtoGPU, buffer_size);
  std::memcpy(transform_indices_buffer->mapped_ptr, transform_indices.data(), buffer_size);

  std::tie(
    silhouette_mask,
    visbuffer_attachment,
    meshlet_instances_buffer,
    mesh_instances_buffer,
    transform_indices_buffer
  ) =
    mask_generation_pass(
      std::move(silhouette_mask),
      std::move(visbuffer_attachment),
      std::move(meshlet_instances_buffer),
      std::move(mesh_instances_buffer),
      std::move(transform_indices_buffer)
    );

  ctx.set_shared_image_resource("silhouette_mask", std::move(silhouette_mask))
    .set_image_resource("depth_attachment", std::move(depth_attachment))
    .set_image_resource("visbuffer_attachment", std::move(visbuffer_attachment))
    .set_buffer_resource("meshlet_instances_buffer", std::move(meshlet_instances_buffer))
    .set_buffer_resource("mesh_instances_buffer", std::move(mesh_instances_buffer));
}

auto highlight_composite_stage(RenderStageContext& ctx, vuk::Value<vuk::ImageAttachment> original_result_attachment)
  -> option<vuk::Value<vuk::ImageAttachment>> {
  ZoneScoped;

  auto silhouette_mask = ctx.get_shared_image_resource("silhouette_mask");
  if (!silhouette_mask.has_value()) {
    return nullopt;
  }

  auto depth_attachment = ctx.get_image_resource("depth_attachment");

  auto outline_composite_output = vuk::declare_ia(
    "outlined_composite",
    {.usage = vuk::ImageUsageFlagBits::eColorAttachment | vuk::ImageUsageFlagBits::eSampled,
     .sample_count = vuk::Samples::e1}
  );
  outline_composite_output.same_format_as(original_result_attachment);
  outline_composite_output.same_shape_as(original_result_attachment);
  outline_composite_output = vuk::clear_image(std::move(outline_composite_output), vuk::Black<f32>);

  auto temp_mask = vuk::declare_ia(
    "temp_horiz_dilated_mask",
    {.usage = vuk::ImageUsageFlagBits::eStorage | vuk::ImageUsageFlagBits::eSampled,
     .format = vuk::Format::eR8Unorm,
     .sample_count = vuk::Samples::e1}
  );
  temp_mask.same_shape_as(outline_composite_output);

  auto horiz_dilate_pass = vuk::make_pass(
    "horizontal_dilate_pass",
    [](vuk::CommandBuffer& cmd_list, VUK_IA(vuk::eComputeSampled) input_mask, VUK_IA(vuk::eComputeWrite) output_mask) {
      struct PC {
        glm::uvec2 resolution;
        i32 radius = 8;
      } push_block;

      push_block.resolution = glm::uvec2(input_mask->extent.width, input_mask->extent.height);

      cmd_list.bind_compute_pipeline("highlight_dilate_horizontal")
        .bind_image(0, 0, input_mask)
        .bind_image(0, 1, output_mask)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(push_block))
        .dispatch_invocations_per_pixel(output_mask);

      return std::make_tuple(input_mask, output_mask);
    }
  );

  auto composite_pass = vuk::make_pass(
    "outline_composite",
    [](
      vuk::CommandBuffer& cmd_list,
      VUK_IA(vuk::eFragmentSampled) original_mask,
      VUK_IA(vuk::eFragmentSampled) dilated_horiz_mask,
      VUK_IA(vuk::eFragmentSampled) depth,
      VUK_IA(vuk::eFragmentSampled) color,
      VUK_IA(vuk::eColorWrite) out_composite
    ) {
      // NOTE: HideBehindWalls is here for future use. Currently we render the silhoutte using main visbuffer which
      // geometry alread passes the depth test.
      // DimBehindWalls will also make the outlines slightly transparent.
      enum OccludeMode {
        HideBehindWalls = 0,
        DimmBehindWalls = 1,
        AlwaysVisible = 2,
      };
      struct PC {
        glm::vec4 outline_color = glm::vec4(1.0f, 0.53f, 0.0f, 1.0f); // Pure Gold
        glm::uvec2 resolution;
        i32 outline_width = 5;
        i32 occluded_mode = OccludeMode::AlwaysVisible;
      } push_block;

      push_block.resolution = glm::uvec2(color->extent.width, color->extent.height);

      cmd_list.bind_graphics_pipeline("highlight_composite")
        .broadcast_color_blend(vuk::BlendPreset::eAlphaBlend)
        .set_dynamic_state(vuk::DynamicStateFlagBits::eScissor | vuk::DynamicStateFlagBits::eViewport)
        .set_rasterization({.cullMode = vuk::CullModeFlagBits::eNone})
        .bind_image(0, 0, original_mask)
        .bind_image(0, 1, dilated_horiz_mask)
        .bind_image(0, 2, depth)
        .bind_image(0, 3, color)
        .bind_sampler(0, 4, vuk::LinearSamplerClamped)
        .push_constants(vuk::ShaderStageFlagBits::eFragment, 0, PushConstants(push_block))
        .draw(3, 1, 0, 0);

      return std::make_tuple(original_mask, dilated_horiz_mask, depth, color, out_composite);
    }
  );

  std::tie(silhouette_mask, temp_mask) = horiz_dilate_pass(std::move(*silhouette_mask), std::move(temp_mask));

  std::tie(*silhouette_mask, temp_mask, depth_attachment, original_result_attachment, outline_composite_output) =
    composite_pass(
      std::move(*silhouette_mask),
      std::move(temp_mask),
      std::move(depth_attachment),
      std::move(original_result_attachment),
      std::move(outline_composite_output)
    );

  ctx.set_image_resource("depth_attachment", std::move(depth_attachment));

  return outline_composite_output;
}

auto ViewportPanel::resolve_pending_pick(this ViewportPanel& self, PendingPick& pick, bool skip_invalid) -> void {
  ZoneScoped;

  if (!pick.pending || !pick.buffer) {
    return;
  }

  // Let the submission clear its full inflight depth before reading the mapping.
  auto& render_context = App::get_rendercontext();
  if (render_context.num_frames - pick.submitted_frame < render_context.num_inflight_frames) {
    return;
  }

  pick.pending = false;

  if (!self.editor_scene) {
    return;
  }

  u32 texel_data = ~0_u32;
  std::memcpy(&texel_data, pick.buffer->mapped_ptr, sizeof(u32));

  // The 3D pass reports a miss as an invalid index, which deselects. The 2D pass shares the frame
  // with it, so a miss there must not clobber what 3D picked.
  if (skip_invalid && texel_data == ~0_u32) {
    return;
  }

  pick_entity(self.editor_scene.get(), texel_data);
}

auto ViewportPanel::mouse_picking_stages(
  this ViewportPanel& self, RendererInstance* renderer_instance, glm::uvec2 picking_texel
) -> void {
  ZoneScoped;

  self.resolve_pending_pick(self.pending_pick_3d, false);
  self.resolve_pending_pick(self.pending_pick_2d, true);

  const auto using_gizmo = ImGuizmo::IsOver() || ImGuizmo::IsUsing();
  const bool should_pick = !using_gizmo && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && self.is_viewport_hovered &&
                           !self.is_ui_capturing_mouse && !self.is_menubar_hovered;

  if (should_pick) {
    renderer_instance
      ->add_stage_after(RenderStage::Forward2D, "mouse_picking_2d", [&self, picking_texel](RenderStageContext& ctx) {
        auto visbuffer_attach = ctx.get_image_resource("visbuffer_attachment_2d");
        auto final_attach = ctx.get_image_resource("final_attachment");

        auto& pick = self.pending_pick_2d;
        if (!pick.buffer) {
          pick.buffer = ctx.render_context.allocate_buffer_super(vuk::MemoryUsage::eGPUtoCPU, sizeof(u32));
        }
        auto readback_buffer = vuk::acquire_buf("pick readback 2d", *pick.buffer, vuk::Access::eNone);

        auto pick_pass = vuk::make_pass(
          "mouse_picking_2d_pass",
          [picking_texel](
            vuk::CommandBuffer& cmd_list,
            VUK_BA(vuk::eComputeWrite) buffer,
            VUK_IA(vuk::eComputeSampled) visbuffer_,
            VUK_IA(vuk::eComputeSampled) final_
          ) {
            cmd_list.bind_compute_pipeline("entity_mouse_picking_2d")
              .bind_image(0, 0, visbuffer_)
              .push_constants(
                vuk::ShaderStageFlagBits::eCompute,
                0,
                PushConstants(picking_texel, buffer->device_address)
              )
              .dispatch(1, 1, 1);

            return std::make_tuple(buffer, visbuffer_, final_);
          }
        );

        std::tie(readback_buffer, visbuffer_attach, final_attach) = pick_pass(
          std::move(readback_buffer),
          std::move(visbuffer_attach),
          std::move(final_attach)
        );

        // The attachments below carry the pass into the frame, so dropping the buffer value here
        // does not cull it. The contents are read once this frame has certainly completed.
        pick.submitted_frame = ctx.render_context.num_frames;
        pick.pending = true;

        ctx.set_image_resource("visbuffer_attachment_2d", std::move(visbuffer_attach))
          .set_image_resource("final_attachment", std::move(final_attach));
      });

    renderer_instance
      ->add_stage_after(RenderStage::VisBufferEncode, "mouse_picking", [&self, picking_texel](RenderStageContext& ctx) {
        auto visbuffer = ctx.get_image_resource("visbuffer_attachment");
        auto meshlet_instances = ctx.get_buffer_resource("meshlet_instances_buffer");
        auto mesh_instances = ctx.get_buffer_resource("mesh_instances_buffer");

        auto& pick = self.pending_pick_3d;
        if (!pick.buffer) {
          pick.buffer = ctx.render_context.allocate_buffer_super(vuk::MemoryUsage::eGPUtoCPU, sizeof(u32));
        }
        auto readback_buffer = vuk::acquire_buf("pick readback", *pick.buffer, vuk::Access::eNone);

        const auto terrain_transform_index = get_terrain_transform_index(
          self.editor_scene ? self.editor_scene->get_scene().get() : nullptr
        );

        auto write_pass = vuk::make_pass(
          "mouse_picking_write_pass",
          [picking_texel, terrain_transform_index](
            vuk::CommandBuffer& cmd_list,
            VUK_BA(vuk::eComputeWrite) buffer,
            VUK_IA(vuk::eComputeSampled) visbuffer_,
            VUK_BA(vuk::eComputeRead) meshlet_instances_,
            VUK_BA(vuk::eComputeRead) mesh_instances_
          ) {
            cmd_list.bind_compute_pipeline("entity_mouse_picking")
              .bind_buffer(0, 0, meshlet_instances_)
              .bind_buffer(0, 1, mesh_instances_)
              .bind_image(0, 2, visbuffer_)
              .push_constants(
                vuk::ShaderStageFlagBits::eCompute,
                0,
                PushConstants(picking_texel, buffer->device_address, terrain_transform_index)
              )
              .dispatch(1, 1, 1);

            return std::make_tuple(buffer, visbuffer_, meshlet_instances_, mesh_instances_);
          }
        );

        std::tie(readback_buffer, visbuffer, meshlet_instances, mesh_instances) = write_pass(
          std::move(readback_buffer),
          std::move(visbuffer),
          std::move(meshlet_instances),
          std::move(mesh_instances)
        );

        pick.submitted_frame = ctx.render_context.num_frames;
        pick.pending = true;

        ctx.set_image_resource("visbuffer_attachment", std::move(visbuffer))
          .set_buffer_resource("meshlet_instances_buffer", std::move(meshlet_instances))
          .set_buffer_resource("mesh_instances_buffer", std::move(mesh_instances));
      });
  }

  if (!self.draw_entity_highlighting) {
    return;
  }

  renderer_instance->add_stage_after(
    RenderStage::VisBufferEncode,
    "entity_highlighting",
    [s = self.editor_scene.get()](RenderStageContext& ctx) {
      auto& editor_context = App::mod<Editor>().get_context();
      std::vector<u32> transform_indices = {};

      if (editor_context.entity.has_value()) {
        if (!editor_context.entity->has<MeshComponent>()) {
          auto traverse_hierarchy = [&](this auto&& f, flecs::entity entity) -> void {
            entity.children([s, &transform_indices, &f](flecs::entity child) {
              if (child.has<MeshComponent>()) {
                auto transform_id = s->get_scene()->get_entity_transform_id(child);
                if (transform_id.has_value()) {
                  auto transform_index = SlotMap_decode_id(*transform_id).index;
                  transform_indices.emplace_back(transform_index);
                }
              }

              f(child);
            });
          };

          // Terrain carries no MeshComponent and no mesh children, so the hierarchy walk finds
          // nothing; its own transform is what the mask has to match against.
          if (editor_context.entity->has<TerrainComponent>()) {
            auto transform_id = s->get_scene()->get_entity_transform_id(*editor_context.entity);
            if (transform_id.has_value()) {
              transform_indices.emplace_back(SlotMap_decode_id(*transform_id).index);
            }
          }

          traverse_hierarchy(*editor_context.entity);
        } else {
          auto transform_id = s->get_scene()->get_entity_transform_id(*editor_context.entity);
          if (transform_id.has_value()) {
            auto transform_index = SlotMap_decode_id(*transform_id).index;
            transform_indices.emplace_back(transform_index);
          }
        }
      }

      if (transform_indices.empty()) {
        return;
      }

      highlight_mask_stage(ctx, transform_indices, get_terrain_transform_index(s->get_scene().get()));
    }
  );

  renderer_instance->add_stage_after(RenderStage::PostProcessing, "entity_highlighting", [](RenderStageContext& ctx) {
    auto result_attachment = ctx.get_image_resource("result_attachment");
    auto composite_result = highlight_composite_stage(ctx, result_attachment);
    ctx.set_image_resource(
      "result_attachment",
      std::move(composite_result.has_value() ? *composite_result : result_attachment)
    );
  });
}

void ViewportPanel::transform_gizmos_button_group(this ViewportPanel& self, ImVec2 start_cursor_pos) {
  const float frame_height = 1.3f * ImGui::GetFrameHeight();
  const ImVec2 frame_padding = ImGui::GetStyle().FramePadding;
  const ImVec2 button_size = {frame_height, frame_height};
  constexpr float button_count = 9.0f;
  const ImVec2 window_pos = ImGui::GetWindowPos();
  const ImVec2 content_min = ImGui::GetWindowContentRegionMin();
  const ImVec2 panel_top_left = {window_pos.x + content_min.x, window_pos.y + content_min.y};

  const auto scaled_gizmo_position = UI::scale(self.gizmo_position);
  const ImVec2 gizmo_pos = {
    panel_top_left.x + scaled_gizmo_position.x,
    panel_top_left.y + scaled_gizmo_position.y,
  };
  const ImRect bb(
    gizmo_pos.x,
    gizmo_pos.y,
    gizmo_pos.x + button_size.x + UI::scale(8.0f),
    gizmo_pos.y + (button_size.y + UI::scale(2.0f)) * (button_count + 0.5f)
  );
  ImVec4 frame_color = ImGui::GetStyleColorVec4(ImGuiCol_Tab);
  frame_color.w = 0.5f;
  ImGui::RenderFrame(bb.Min, bb.Max, ImGui::GetColorU32(frame_color), false, ImGui::GetStyle().FrameRounding);

  ImGui::SetCursorPos({
    start_cursor_pos.x + scaled_gizmo_position.x + frame_padding.x,
    start_cursor_pos.y + scaled_gizmo_position.y,
  });
  ImGui::BeginGroup();
  {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, UI::scale(ImVec2(1.0f, 1.0f)));

    const ImVec2 dragger_cursor_pos = ImGui::GetCursorPos();
    ImGui::SetCursorPosX(dragger_cursor_pos.x + frame_padding.x);
    ImGui::TextUnformatted(ICON_MDI_DOTS_HORIZONTAL);
    ImVec2 dragger_size = ImGui::CalcTextSize(ICON_MDI_DOTS_HORIZONTAL);
    dragger_size.x *= 2.0f;
    ImGui::SetCursorPos(dragger_cursor_pos);
    ImGui::InvisibleButton("GizmoDragger", dragger_size);
    static ImVec2 last_mouse_position = ImGui::GetMousePos();
    const ImVec2 mouse_pos = ImGui::GetMousePos();
    if (ImGui::IsItemActive()) {
      self.gizmo_position.x += (mouse_pos.x - last_mouse_position.x) / App::get_ui_scale();
      self.gizmo_position.y += (mouse_pos.y - last_mouse_position.y) / App::get_ui_scale();
    }
    last_mouse_position = mouse_pos;

    constexpr float alpha = 0.6f;
    if (UI::toggle_button(ICON_MDI_AXIS_ARROW, self.gizmo_type == ImGuizmo::TRANSLATE, button_size, alpha, alpha))
      self.gizmo_type = ImGuizmo::TRANSLATE;
    if (UI::toggle_button(ICON_MDI_ROTATE_3D, self.gizmo_type == ImGuizmo::ROTATE, button_size, alpha, alpha))
      self.gizmo_type = ImGuizmo::ROTATE;
    if (UI::toggle_button(ICON_MDI_ARROW_EXPAND, self.gizmo_type == ImGuizmo::SCALE, button_size, alpha, alpha))
      self.gizmo_type = ImGuizmo::SCALE;
    if (UI::toggle_button(ICON_MDI_VECTOR_SQUARE, self.gizmo_type == ImGuizmo::BOUNDS, button_size, alpha, alpha))
      self.gizmo_type = ImGuizmo::BOUNDS;
    if (UI::toggle_button(ICON_MDI_ARROW_EXPAND_ALL, self.gizmo_type == ImGuizmo::UNIVERSAL, button_size, alpha, alpha))
      self.gizmo_type = ImGuizmo::UNIVERSAL;
    if (
      UI::toggle_button(
        self.gizmo_mode == ImGuizmo::WORLD ? ICON_MDI_EARTH : ICON_MDI_EARTH_OFF,
        self.gizmo_mode == ImGuizmo::WORLD,
        button_size,
        alpha,
        alpha
      )
    )
      self.gizmo_mode = self.gizmo_mode == ImGuizmo::LOCAL ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
    if (
      UI::toggle_button(ICON_MDI_GRID, App::mod<Editor>().editor_cvar.cvar_draw_grid.get(), button_size, alpha, alpha)
    )
      App::mod<Editor>().editor_cvar.cvar_draw_grid.toggle();

    if (UI::toggle_button(ICON_MDI_BRUSH, self.terrain_brush_enabled, button_size, alpha, alpha))
      self.terrain_brush_enabled = !self.terrain_brush_enabled;

    if (self.editor_camera.is_alive() && self.editor_camera.has<CameraComponent>()) {
      auto& cam = self.editor_camera.get_mut<CameraComponent>();
      UI::push_id();
      if (
        UI::toggle_button(
          ICON_MDI_CAMERA,
          cam.projection == CameraComponent::Projection::Orthographic,
          button_size,
          alpha,
          alpha
        )
      )
        cam.projection = cam.projection == CameraComponent::Projection::Orthographic
                           ? CameraComponent::Projection::Perspective
                           : CameraComponent::Projection::Orthographic;
    }
    UI::pop_id();

    ImGui::PopStyleVar(2);
  }
  ImGui::EndGroup();
}

void ViewportPanel::scene_button_group(this ViewportPanel& self, ImVec2 start_cursor_pos) {
  constexpr float button_count = 2.0f;
  const float y_pad = UI::scale(3.0f);
  const ImVec2 button_size = UI::scale(ImVec2(35.0f, 25.0f));
  const ImVec2 group_size = {button_size.x * button_count, button_size.y + y_pad};

  ImGui::SetCursorPos({self.viewport_size.x * 0.5f - (group_size.x * 0.5f), start_cursor_pos.y + y_pad});
  ImGui::BeginGroup();
  {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, UI::scale(ImVec2(1.0f, 1.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, UI::scale(1.0f));

    auto& event_system = App::get_event_system();

    auto is_scene_playing = self.editor_scene->is_playing();

    ImGui::BeginDisabled(is_scene_playing);
    if (ImGui::Button(ICON_MDI_PLAY, button_size)) {
      std::ignore = event_system.emit<Editor::ScenePlayEvent>(Editor::ScenePlayEvent(self.editor_scene->get_id()));
    }
    ImGui::EndDisabled();

    ImGui::SameLine();

    ImGui::BeginDisabled(!is_scene_playing);
    if (ImGui::Button(ICON_MDI_STOP, button_size)) {
      std::ignore = event_system.emit<Editor::SceneStopEvent>(Editor::SceneStopEvent(self.editor_scene->get_id()));
    }
    ImGui::EndDisabled();

    ImGui::PopStyleVar(3);
  }
  ImGui::EndGroup();
}

} // namespace ox
