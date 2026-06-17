#include "ViewportPanel.hpp"

#include <ImGuizmo.h>
#include <glm/gtx/matrix_decompose.hpp>
#include <icons/IconsMaterialDesignIcons.h>
#include <imgui.h>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Core/Input.hpp"
#include "Editor.hpp"
#include "Render/Camera.hpp"
#include "Render/RenderContext.hpp"
#include "Render/RendererConfig.hpp"
#include "Render/Utils/VukCommon.hpp"
#include "Scene/Components.hpp"
#include "UI/ImGuiRenderer.hpp"
#include "UI/PayloadData.hpp"
#include "UI/UI.hpp"
#include "Utils/EditorConfig.hpp"
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
    ImGui::SetCursorPos({screen_pos.x - (gizmo_info.icon_size / 2.f), screen_pos.y - (gizmo_info.icon_size / 2.f)});
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.7f, 0.7f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.1f, 0.1f, 0.1f, 0.1f));

    ImGui::PushFont(nullptr, gizmo_info.icon_size);
    ImGui::PushID(static_cast<i32>(entity.id()));
    const char* icon = icon_select_func(editor_theme.component_icon_map.at(typeid(T).hash_code()), component);
    if (ImGui::Button(icon, {gizmo_info.icon_size, gizmo_info.icon_size})) {
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
  if (!runtime.is_pipeline_available("mouse_picking_pipeline")) {
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
  if (editor_scene && editor_scene->is_playing())
    std::ignore = event_system.emit<Editor::SceneStopEvent>(Editor::SceneStopEvent(editor_scene->get_id()));
}

void ViewportPanel::on_render(this ViewportPanel& self, vuk::ImageAttachment swapchain_attachment) {
  ZoneScoped;

  constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_MenuBar;

  auto& editor = App::mod<Editor>();

  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.1f, 0.1f, 0.1f, 1.f));
  if (self.on_begin(flags)) {
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
    ImVec2 start_cursor_pos = ImGui::GetCursorPos();

    auto& style = ImGui::GetStyle();

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
      ImGui::EndMenuBar();
    }

    self.draw_stats_overlay(self.draw_scene_stats);

    if (viewport_settings_popup)
      ImGui::OpenPopup("viewport_settings");

    ImGui::SetNextWindowSize(ImVec2(345, 0));
    ImGui::SetNextWindowBgAlpha(0.85f);
    if (ImGui::BeginPopup("viewport_settings")) {
      self.draw_settings_panel();
      ImGui::EndPopup();
    }

    if (gizmo_settings_popup)
      ImGui::OpenPopup("gizmo_settings");

    ImGui::SetNextWindowSize(ImVec2(345, 0));
    ImGui::SetNextWindowBgAlpha(0.85f);
    if (ImGui::BeginPopup("gizmo_settings")) {
      self.draw_gizmo_settings_panel();
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
        {swapchain_attachment.extent.width, swapchain_attachment.extent.height},
        self.viewport_position,
        corrected_min_region,
        corrected_max_region,
        mouse_pos
      );
      if (self.mouse_picking_enabled) {
        self.mouse_picking_stages(renderer_instance, picking_texel);
      }

      if (EditorCVar::cvar_draw_grid.as_bool()) {
        self.grid_stage(renderer_instance);
      }

      const Renderer::RenderInfo render_info = {
        .viewport_offset = {self.viewport_position.x, self.viewport_position.y},
      };

      auto viewport_attachment = vuk::declare_ia("viewport", swapchain_attachment);
      viewport_attachment = vuk::clear_image(std::move(viewport_attachment), vuk::Black<f32>);

      auto scene_view_image = renderer_instance->render(std::move(viewport_attachment), render_info);
      self.editor_scene->get_scene()->on_viewport_render(swapchain_attachment.extent, swapchain_attachment.format);

      ImGui::SetCursorPos(
        {ImGui::GetCursorPosX() + self.viewport_offset.x, ImGui::GetCursorPosY() + self.viewport_offset.y}
      );
      UI::image(std::move(scene_view_image), ImVec2{self.render_size.x, self.render_size.y});

      self.drag_drop();
    }

    if (!self.editor_scene->is_playing()) {
      if (self.editor_camera.is_alive() && self.editor_camera.has<CameraComponent>()) {
        self.editor_camera.enable();

        self.draw_gizmos();
      }
      self.transform_gizmos_button_group(start_cursor_pos);
    }

    self.scene_button_group(start_cursor_pos);
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

  const f32 dt = static_cast<f32>(App::get_timestep().get_seconds());

  auto& cam = self.editor_camera.get_mut<CameraComponent>();
  auto& tc = self.editor_camera.get_mut<TransformComponent>();
  const glm::vec3& position = cam.position;
  const glm::vec2 yaw_pitch = glm::vec2(cam.yaw, cam.pitch);
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
    auto& editor_context = App::mod<Editor>().get_context();
    if (editor_context.entity.has_value()) {
      const auto entity_tc = editor_context.entity->get<TransformComponent>();
      auto final_pos = entity_tc.position + cam.forward;
      final_pos += -5.0f * cam.forward * glm::vec3(1.0f);
      cam.position = final_pos;
    }
  }

  const auto actual_sens = EditorCVar::cvar_camera_sens.get() / 10.f;
  const auto smoothed_sens = actual_sens * 100.f;
  const auto camera_sens = EditorCVar::cvar_camera_smooth.get() ? smoothed_sens : actual_sens;

  const auto actual_speed = EditorCVar::cvar_camera_speed.get();
  const auto smoothed_speed = actual_speed * 100.f;
  const auto camera_speed = EditorCVar::cvar_camera_smooth.get() ? smoothed_speed : actual_speed;

  if ((input_sys.get_mouse_held(MouseCode::Middle) || input_sys.get_mouse_held(MouseCode::Right)) && !is_ortho) {
    const glm::vec2 new_mouse_position = input_sys.get_mouse_position_rel();
    window.set_cursor_override(WindowCursor::Crosshair);

    if (input_sys.get_mouse_moved()) {
      const glm::vec2 change = new_mouse_position * camera_sens;
      final_yaw_pitch.x += change.x;
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

  tc.position = EditorCVar::cvar_camera_smooth.as_bool() ? damped_position : final_position;
  const float applied_pitch = EditorCVar::cvar_camera_smooth.as_bool() ? damped_yaw_pitch.y : final_yaw_pitch.y;
  const float applied_yaw = EditorCVar::cvar_camera_smooth.as_bool() ? damped_yaw_pitch.x : final_yaw_pitch.x;
  tc.rotation = glm::quat(glm::vec3(applied_pitch, applied_yaw, 0.0f));
  cam.pitch = applied_pitch;
  cam.yaw = applied_yaw;
  cam.zoom = static_cast<float>(EditorCVar::cvar_camera_zoom.get());
}

void ViewportPanel::set_context(this ViewportPanel& self, const std::shared_ptr<EditorScene>& scene) {
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

void ViewportPanel::drag_drop(this const ViewportPanel& self) {
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* imgui_payload = ImGui::AcceptDragDropPayload(PayloadData::DRAG_DROP_SOURCE)) {
      const auto* payload = PayloadData::from_payload(imgui_payload);
      const auto path = payload->get_path();
      if (path.extension() == ".gltf" || path.extension() == ".glb") {
        if (auto asset = App::mod<AssetManager>().import_asset(path))
          self.editor_scene->get_scene()->create_model_entity(asset);
      }
    }

    ImGui::EndDragDropTarget();
  }
}

void ViewportPanel::draw_stats_overlay(this const ViewportPanel& self, bool draw) {
  if (!self.performance_overlay_visible || !self.editor_scene)
    return;
  auto work_pos = ImVec2(self.viewport_position.x, self.viewport_position.y);
  auto work_size = ImVec2(self.viewport_size.x, self.viewport_size.y);
  auto padding = glm::vec2{15, 55};

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
  ImGui::SetNextWindowSize(draw ? ImVec2({220.f, 0.f}) : ImVec2(120.f, 5.f), ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 2.0f);
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
      const auto particle_entities_count = self.editor_scene->get_scene()->world.count<ParticleSystemComponent>();
      ImGui::Text("Entities with particle: %d", particle_entities_count);
      const auto rigidbody_entities_count = self.editor_scene->get_scene()->world.count<RigidBodyComponent>();
      ImGui::Text("Entities with rigidbody: %d", rigidbody_entities_count);
      const auto audio_entities_count = self.editor_scene->get_scene()->world.count<AudioSourceComponent>();
      ImGui::Text("Entities with audio: %d", audio_entities_count);
    }
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
}

void ViewportPanel::draw_settings_panel(this ViewportPanel& self) {
  ZoneScoped;

  i32 open_action = -1;

  if (UI::button("Expand All")) {
    open_action = 1;
  }
  ImGui::SameLine();
  if (UI::button("Collapse All")) {
    open_action = 0;
  }
  ImGui::SameLine();
  if (UI::button("Reset to defaults")) {
    RendererCVar::cvar_enable_debug_renderer.set_default();
    RendererCVar::cvar_enable_physics_debug_renderer.set_default();
    RendererCVar::cvar_draw_bounding_boxes.set_default();
    RendererCVar::cvar_draw_camera_frustum.get_default();
    RendererCVar::cvar_bloom_enable.set_default();
    RendererCVar::cvar_bloom_threshold.set_default();
    RendererCVar::cvar_bloom_clamp.set_default();
    RendererCVar::cvar_bloom_quality_level.set_default();
    RendererCVar::cvar_fxaa_enable.set_default();
    RendererCVar::cvar_vbgtao_quality_level.set_default();
    RendererCVar::cvar_vbgtao_radius.set_default();
    RendererCVar::cvar_vbgtao_thickness.set_default();
    RendererCVar::cvar_vbgtao_final_power.set_default();
    RendererCVar::cvar_contact_shadows.set_default();
    RendererCVar::cvar_contact_shadows_steps.set_default();
    RendererCVar::cvar_contact_shadows_thickness.set_default();
    RendererCVar::cvar_contact_shadows_length.set_default();
    EditorCVar::cvar_camera_sens.set_default();
    EditorCVar::cvar_camera_speed.set_default();
    EditorCVar::cvar_camera_smooth.set_default();
    EditorCVar::cvar_camera_zoom.set_default();
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
      window.get_window_content_scale()
    );
    if (UI::icon_button(ICON_MDI_RELOAD, "Reload renderer"))
      RendererCVar::cvar_reload_renderer.toggle();
    if (UI::begin_properties(UI::default_properties_flags, true, 0.3f)) {
      UI::property("VSync", (bool*)RendererCVar::cvar_vsync.get_ptr());
      UI::end_properties();
    }

    if (open_action != -1)
      ImGui::SetNextItemOpen(open_action != 0);
    if (ImGui::TreeNodeEx("Debug", TREE_FLAGS, "%s", "Debug")) {
      if (UI::begin_properties(UI::default_properties_flags, true, 0.3f)) {
        UI::property("Enable debug renderer", (bool*)RendererCVar::cvar_enable_debug_renderer.get_ptr());
        UI::property(
          "Enable physics debug renderer",
          (bool*)RendererCVar::cvar_enable_physics_debug_renderer.get_ptr()
        );
        UI::property("Draw bounding boxes", (bool*)RendererCVar::cvar_draw_bounding_boxes.get_ptr());
        UI::property("Freeze culling frustum", (bool*)RendererCVar::cvar_freeze_culling_frustum.get_ptr());
        UI::property("Draw camera frustum", (bool*)RendererCVar::cvar_draw_camera_frustum.get_ptr());
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
          "GTAO"
        };
        UI::property(
          "Debug View",
          RendererCVar::cvar_debug_view.get_ptr(),
          debug_views,
          static_cast<i32>(ox::count_of(debug_views))
        );
        UI::property("Enable frustum culling", (bool*)RendererCVar::cvar_culling_frustum.get_ptr());
        UI::property("Enable occlusion culling", (bool*)RendererCVar::cvar_culling_frustum.get_ptr());
        UI::property("Enable triangle culling", (bool*)RendererCVar::cvar_culling_triangle.get_ptr());
        UI::end_properties();
      }
      ImGui::TreePop();
    }

    if (open_action != -1)
      ImGui::SetNextItemOpen(open_action != 0);
    if (ImGui::TreeNodeEx("Bloom", TREE_FLAGS, "%s", "Bloom")) {
      if (UI::begin_properties(UI::default_properties_flags, true, 0.3f)) {
        UI::property("Enabled", (bool*)RendererCVar::cvar_bloom_enable.get_ptr());
        UI::property<float>("Threshold", RendererCVar::cvar_bloom_threshold.get_ptr(), 0, 5);
        UI::property<float>("Clamp", RendererCVar::cvar_bloom_clamp.get_ptr(), 0, 5);
        const char* quality_levels[4] = {"Low", "Medium", "High", "Ultra"};
        UI::property("Quality Level", RendererCVar::cvar_bloom_quality_level.get_ptr(), quality_levels, 4);
        UI::end_properties();
      }
      ImGui::TreePop();
    }

    if (open_action != -1)
      ImGui::SetNextItemOpen(open_action != 0);
    if (ImGui::TreeNodeEx("FXAA", TREE_FLAGS, "%s", "FXAA")) {
      if (UI::begin_properties(UI::default_properties_flags, true, 0.3f)) {
        UI::property("Enabled", (bool*)RendererCVar::cvar_fxaa_enable.get_ptr());
        UI::end_properties();
      }
      ImGui::TreePop();
    }

    if (open_action != -1)
      ImGui::SetNextItemOpen(open_action != 0);
    if (ImGui::TreeNodeEx("GTAO", TREE_FLAGS, "%s", "GTAO")) {
      if (UI::begin_properties(UI::default_properties_flags, true, 0.3f)) {
        UI::property("Enabled", (bool*)RendererCVar::cvar_vbgtao_enable.get_ptr());
        const char* quality_levels[4] = {"Low", "Medium", "High", "Ultra"};
        UI::property("Quality Level", RendererCVar::cvar_vbgtao_quality_level.get_ptr(), quality_levels, 4);
        UI::property<float>("Radius", RendererCVar::cvar_vbgtao_radius.get_ptr(), 0.1f, 5.f);
        UI::property<float>("Thickness", RendererCVar::cvar_vbgtao_thickness.get_ptr(), 0.0f, 5.f);
        UI::property<float>("Final Power", RendererCVar::cvar_vbgtao_final_power.get_ptr(), 0.f, 10.f);
        UI::end_properties();
      }
      ImGui::TreePop();
    }

    if (open_action != -1)
      ImGui::SetNextItemOpen(open_action != 0);
    if (ImGui::TreeNodeEx("Contact Shadows", TREE_FLAGS, "%s", "Contact Shadows")) {
      if (UI::begin_properties(UI::default_properties_flags, true, 0.3f)) {
        UI::property("Enabled", (bool*)RendererCVar::cvar_contact_shadows.get_ptr());
        UI::property("Steps", RendererCVar::cvar_contact_shadows_steps.get_ptr(), 1, 64);
        UI::property<float>("Thickness", RendererCVar::cvar_contact_shadows_thickness.get_ptr(), 0.0, 5);
        UI::property<float>("Length", RendererCVar::cvar_contact_shadows_length.get_ptr(), 0.0, 5);
        UI::end_properties();
      }
      ImGui::TreePop();
    }

    ImGui::TreePop();
  }

  if (open_action != -1)
    ImGui::SetNextItemOpen(open_action != 0);
  if (ImGui::TreeNodeEx("Viewport", TREE_FLAGS, "%s", "Viewport")) {
    if (UI::begin_properties(UI::default_properties_flags, true, 0.3f)) {
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
        UI::property<float>("Camera sensitivity", EditorCVar::cvar_camera_sens.get_ptr(), 0.01f, 20.0f);
        UI::property<float>("Movement speed", EditorCVar::cvar_camera_speed.get_ptr(), 0.1f, 100.0f);
        UI::property("Smooth camera", (bool*)EditorCVar::cvar_camera_smooth.get_ptr());
        UI::property("Camera zoom", EditorCVar::cvar_camera_zoom.get_ptr(), 1, 100);
        UI::end_properties();
      }

      ImGui::TreePop();
    }

    ImGui::TreePop();
  }
}

void ViewportPanel::draw_gizmo_settings_panel(this ViewportPanel& self) {
  if (UI::begin_properties(UI::default_properties_flags, true, 0.3f)) {
    UI::property("Draw grid", (bool*)EditorCVar::cvar_draw_grid.get_ptr());
    UI::property<float>("Grid distance", EditorCVar::cvar_draw_grid_distance.get_ptr(), 10.f, 10000.0f);

    UI::property("Draw Component Gizmos", &self.draw_component_gizmos);
    UI::property("Component Gizmos Size", &self.gizmo_icon_size);
    UI::property("Entity Highlighting", &self.draw_entity_highlighting);
    UI::end_properties();
  }
}

void ViewportPanel::draw_gizmos(this ViewportPanel& self) {
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
      [](const char* icon, const LightComponent& c) {
        switch (c.type) {
          case LightComponent::Directional: return ICON_MDI_WEATHER_SUNNY;
          case LightComponent::Spot       : return ICON_MDI_SPOTLIGHT;
          case LightComponent::Point      : return icon;
        }
      }
    );
    show_component_gizmo<AudioSourceComponent>(
      gizmo_info,
      "AudioSourceComponent",
      self.editor_scene->get_scene().get(),
      [](const char* icon, const AudioSourceComponent& c) { return icon; }
    );
    show_component_gizmo<AudioListenerComponent>(
      gizmo_info,
      "AudioListenerComponent",
      self.editor_scene->get_scene().get(),
      [](const char* icon, const AudioListenerComponent& c) { return icon; }
    );
    show_component_gizmo<CameraComponent>(
      gizmo_info,
      "CameraComponent",
      self.editor_scene->get_scene().get(),
      [](const char* icon, const CameraComponent& c) { return icon; }
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

  if (selected_entity == flecs::entity::null() || !self.editor_camera.has<CameraComponent>() || self.gizmo_type == -1)
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
    const bool snap = input_sys.get_key_held(ScanCode::LeftControl);
    float snap_value = 0.5f; // Snap to 0.5m for translation/scale
    // Snap to 45 degrees for rotation
    if (self.gizmo_type == ImGuizmo::OPERATION::ROTATE)
      snap_value = 45.0f;

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

auto ViewportPanel::mouse_picking_stages(
  this ViewportPanel& self, RendererInstance* renderer_instance, glm::uvec2 picking_texel
) -> void {
  auto using_gizmo = ImGuizmo::IsOver();
  if (!(!using_gizmo && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && self.is_viewport_hovered)) {
    return;
  }

  renderer_instance->add_stage_after(
    RenderStage::Forward2D,
    "mouse_picking_2d",
    [s = self.editor_scene, picking_texel](RenderStageContext& ctx) {
      auto visbuffer_attach = ctx.get_image_resource("visbuffer_attachment_2d");
      auto final_attach = ctx.get_image_resource("final_attachment");

      auto readback_buffer = ctx.render_context.alloc_transient_buffer(vuk::MemoryUsage::eGPUtoCPU, sizeof(u32));

      auto pick_pass = vuk::make_pass(
        "mouse_picking_2d_pass",
        [picking_texel](
          vuk::CommandBuffer& cmd_list,
          VUK_BA(vuk::eComputeWrite) buffer,
          VUK_IA(vuk::eComputeSampled) visbuffer_,
          VUK_IA(vuk::eComputeSampled) final_
        ) {
          cmd_list
            .bind_compute_pipeline("mouse_picking_pipeline_2d") //
            .bind_image(0, 0, visbuffer_)
            .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(picking_texel, buffer->device_address))
            .dispatch(1, 1, 1);

          return std::make_tuple(buffer, visbuffer_, final_);
        }
      );

      std::tie(readback_buffer, visbuffer_attach, final_attach) = pick_pass(
        std::move(readback_buffer),
        std::move(visbuffer_attach),
        std::move(final_attach)
      );

      auto temp_compiler = vuk::Compiler{};
      readback_buffer.wait(*ctx.render_context.superframe_allocator, temp_compiler);

      u32 texel_data = ~0_u32;
      std::memcpy(&texel_data, readback_buffer->mapped_ptr, sizeof(u32));
      pick_entity(s.get(), texel_data);

      ctx.set_image_resource("visbuffer_attachment_2d", std::move(visbuffer_attach))
        .set_image_resource("final_attachment", std::move(final_attach));
    }
  );

  renderer_instance->add_stage_after(
    RenderStage::VisBufferEncode,
    "mouse_picking",
    [picking_texel, s = self.editor_scene](RenderStageContext& ctx) {
      auto depth_attachment = ctx.get_image_resource("depth_attachment");
      auto visbuffer = ctx.get_image_resource("visbuffer_attachment");
      auto meshlet_instances = ctx.get_buffer_resource("meshlet_instances_buffer");
      auto mesh_instances = ctx.get_buffer_resource("mesh_instances_buffer");

      auto readback_buffer = ctx.render_context.alloc_transient_buffer(vuk::MemoryUsage::eGPUtoCPU, sizeof(u32));

      auto write_pass = vuk::make_pass(
        "mouse_picking_write_pass",
        [picking_texel](
          vuk::CommandBuffer& cmd_list,
          VUK_BA(vuk::eComputeWrite) buffer,
          VUK_IA(vuk::eComputeSampled) visbuffer_,
          VUK_BA(vuk::eComputeRead) meshlet_instances_,
          VUK_BA(vuk::eComputeRead) mesh_instances_
        ) {
          cmd_list.bind_compute_pipeline("mouse_picking_pipeline")
            .bind_buffer(0, 0, meshlet_instances_)
            .bind_buffer(0, 1, mesh_instances_)
            .bind_image(0, 2, visbuffer_)
            .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(picking_texel, buffer->device_address))
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

      auto temp_compiler = vuk::Compiler{};
      readback_buffer.wait(*ctx.render_context.superframe_allocator, temp_compiler);

      u32 texel_data = ~0_u32;
      std::memcpy(&texel_data, readback_buffer->mapped_ptr, sizeof(u32));
      pick_entity(s.get(), texel_data);

      auto highlight_attachment = vuk::declare_ia(
        "highlight",
        {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage,
         .format = vuk::Format::eR32Sfloat,
         .sample_count = vuk::Samples::e1}
      );
      highlight_attachment.same_shape_as(visbuffer);
      highlight_attachment = vuk::clear_image(std::move(highlight_attachment), vuk::Black<f32>);

      auto highlight_pass = vuk::make_pass(
        "highlighting_pass",
        [s](
          vuk::CommandBuffer& cmd_list,
          VUK_IA(vuk::eComputeRW) result,
          VUK_BA(vuk::eHostRead) entity_buffer,
          VUK_IA(vuk::eComputeSampled) visbuffer_,
          VUK_IA(vuk::eComputeSampled) depth,
          VUK_BA(vuk::eComputeRead) meshlet_instances_,
          VUK_BA(vuk::eComputeRead) mesh_instances_
        ) {
          auto& editor_context = App::mod<Editor>().get_context();

          std::vector<u32> transform_indices = {};

          if (editor_context.entity.has_value()) {
            // if selected entity is not a mesh check if it has mesh childs
            if (!editor_context.entity->has<MeshComponent>()) {
              editor_context.entity->children([s, &transform_indices](flecs::entity e) {
                if (e.has<MeshComponent>()) {
                  auto transform_id = s->get_scene()->get_entity_transform_id(e);
                  if (transform_id.has_value()) {
                    auto transform_index = SlotMap_decode_id(*transform_id).index;
                    transform_indices.emplace_back(transform_index);
                  }
                }
              });
            } else {
              auto transform_id = s->get_scene()->get_entity_transform_id(*editor_context.entity);
              if (transform_id.has_value()) {
                auto transform_index = SlotMap_decode_id(*transform_id).index;
                transform_indices.emplace_back(transform_index);
              }
            }

            if (!transform_indices.empty()) {
              auto* buffer = cmd_list._scratch_buffer(0, 5, transform_indices.size() * sizeof(u32));
              std::memcpy(buffer, transform_indices.data(), transform_indices.size() * sizeof(u32));
              cmd_list.bind_compute_pipeline("highlighting_pipeline")
                .bind_buffer(0, 0, meshlet_instances_)
                .bind_buffer(0, 1, mesh_instances_)
                .bind_image(0, 2, visbuffer_)
                .bind_image(0, 3, depth)
                .bind_image(0, 4, result)
                .bind_sampler(0, 6, vuk::NearestSamplerClamped)
                .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants((u32)transform_indices.size()))
                .dispatch_invocations_per_pixel(visbuffer_);
            }
          }

          return std::make_tuple(result, entity_buffer, visbuffer_, depth, meshlet_instances_, mesh_instances_);
        }
      );

      std::tie(highlight_attachment, readback_buffer, visbuffer, depth_attachment, meshlet_instances, mesh_instances) =
        highlight_pass(
          std::move(highlight_attachment),
          std::move(readback_buffer),
          std::move(visbuffer),
          std::move(depth_attachment),
          std::move(meshlet_instances),
          std::move(mesh_instances)
        );

      ctx.set_shared_image_resource("highlight_attachment", std::move(highlight_attachment))
        .set_image_resource("depth_attachment", std::move(depth_attachment))
        .set_image_resource("visbuffer_attachment", std::move(visbuffer))
        .set_buffer_resource("meshlet_instances_buffer", std::move(meshlet_instances))
        .set_buffer_resource("mesh_instances_buffer", std::move(mesh_instances));
    }
  );

  if (!self.draw_entity_highlighting) {
    return;
  }

  renderer_instance->add_stage_after(RenderStage::PostProcessing, "entity_highlighting", [](RenderStageContext& ctx) {
    auto result_attachment = ctx.get_image_resource("result_attachment");
    auto highlight_attachment = ctx.get_shared_image_resource("highlight_attachment");

    if (!highlight_attachment.has_value()) {
      ctx.set_image_resource("result_attachment", std::move(result_attachment));
      return;
    }

    auto highlight_applied_attachment = vuk::declare_ia(
      "highlight_applied_attachment",
      {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
       .sample_count = vuk::Samples::e1}
    );
    highlight_applied_attachment.same_shape_as(result_attachment);
    highlight_applied_attachment.same_format_as(result_attachment);
    highlight_applied_attachment = vuk::clear_image(std::move(highlight_applied_attachment), vuk::Black<f32>);

    auto highlight_pass = vuk::make_pass(
      "apply_highlighting_pass",
      [](
        vuk::CommandBuffer& cmd_list,
        VUK_IA(vuk::eColorRW) result,
        VUK_IA(vuk::eFragmentSampled) source,
        VUK_IA(vuk::eFragmentSampled) highlight
      ) {
        cmd_list.bind_graphics_pipeline("apply_highlighting_pipeline")
          .set_rasterization({})
          .broadcast_color_blend({})
          .set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
          .set_viewport(0, vuk::Rect2D::framebuffer())
          .set_scissor(0, vuk::Rect2D::framebuffer())
          .bind_image(0, 0, highlight)
          .bind_image(0, 1, source)
          .bind_sampler(0, 2, vuk::LinearSamplerClamped)
          .draw(3, 1, 0, 0);

        return std::make_tuple(result, source, highlight);
      }
    );

    std::tie(highlight_applied_attachment, result_attachment, *highlight_attachment) = highlight_pass(
      highlight_applied_attachment,
      result_attachment,
      *highlight_attachment
    );

    // change result_attachment to highlight applied attachment
    ctx.set_shared_image_resource("highlight_attachment", std::move(*highlight_attachment))
      .set_image_resource("result_attachment", std::move(highlight_applied_attachment));
  });
}

auto ViewportPanel::grid_stage(this ViewportPanel& self, RendererInstance* renderer_instance) -> void {
  ZoneScoped;

  renderer_instance->add_stage_after(RenderStage::PostProcessing, "grid_stage", [](RenderStageContext& ctx) {
    auto result_attachment = ctx.get_image_resource("result_attachment");
    auto depth_attachment = ctx.get_image_resource("depth_attachment");
    auto camera_buffer = ctx.get_buffer_resource("camera_buffer");

    auto grid_pass = vuk::make_pass(
      "grid_pass",
      [](
        vuk::CommandBuffer& cmd_list,
        VUK_IA(vuk::eColorWrite) out,
        VUK_IA(vuk::eDepthStencilRead) depth,
        VUK_BA(vuk::eAttributeRead) vertex_buffer,
        VUK_BA(vuk::eIndexRead) index_buffer,
        VUK_BA(vuk::eVertexRead) camera
      ) {
        const auto vertex_pack = vuk::Packed{
          vuk::Format::eR32G32B32Sfloat,
          vuk::Format::eR32G32Sfloat,
        };

        const auto grid_distance = EditorCVar::cvar_draw_grid_distance.get();
        const auto position = glm::vec3(0.f, 0.f, 0.f);
        const auto rotation = glm::vec3(glm::radians(90.f), 0.f, 0.f);
        const auto scale = glm::floor(glm::vec3(grid_distance / 2.0f)) * 2.0f;
        auto grid_transform = glm::translate(glm::mat4(1.0f), position) * glm::toMat4(glm::quat(rotation)) *
                              glm::scale(glm::mat4(1.0f), scale);

        cmd_list.bind_graphics_pipeline("grid_pipeline")
          .set_dynamic_state(vuk::DynamicStateFlagBits::eScissor | vuk::DynamicStateFlagBits::eViewport)
          .set_viewport(0, vuk::Rect2D::framebuffer())
          .set_scissor(0, vuk::Rect2D::framebuffer())
          .set_depth_stencil(
            {.depthTestEnable = true, .depthWriteEnable = false, .depthCompareOp = vuk::CompareOp::eGreaterOrEqual}
          )
          .broadcast_color_blend(vuk::BlendPreset::eAlphaBlend)
          .set_rasterization({.cullMode = vuk::CullModeFlagBits::eNone})
          .bind_buffer(0, 0, camera)
          .bind_vertex_buffer(0, vertex_buffer, 0, vertex_pack)
          .bind_index_buffer(index_buffer, vuk::IndexType::eUint32)
          .push_constants(
            vuk::ShaderStageFlagBits::eVertex | vuk::ShaderStageFlagBits::eFragment,
            0,
            PushConstants(grid_transform, scale.x)
          )
          .draw_indexed(6, 1, 0, 0, 0);

        return std::make_tuple(out, depth, camera);
      }
    );

    auto grid_attachment = vuk::declare_ia(
      "grid_attachment",
      {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
       .sample_count = vuk::Samples::e1}
    );
    grid_attachment.same_format_as(result_attachment);
    grid_attachment.same_shape_as(result_attachment);
    grid_attachment = vuk::clear_image(grid_attachment, vuk::Black<f32>);

    auto& renderer = App::mod<Renderer>();
    auto grid_vertex_buffer = vuk::acquire_buf("grid vertex buffer", *renderer.quad_vertex_buffer, vuk::eMemoryRead);
    auto grid_index_buffer = vuk::acquire_buf("grid index buffer", *renderer.quad_index_buffer, vuk::eMemoryRead);

    std::tie(
      grid_attachment,
      depth_attachment,
      camera_buffer
    ) = grid_pass(grid_attachment, depth_attachment, grid_vertex_buffer, grid_index_buffer, camera_buffer);

    auto apply_grid_pass = vuk::make_pass(
      "apply_grid_pass",
      [](
        vuk::CommandBuffer& cmd_list,
        VUK_IA(vuk::eColorWrite) out,
        VUK_IA(vuk::eFragmentSampled) source,
        VUK_IA(vuk::eFragmentSampled) grid
      ) {
        cmd_list.bind_graphics_pipeline("apply_grid_pipeline")
          .set_rasterization({})
          .broadcast_color_blend({})
          .set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
          .set_viewport(0, vuk::Rect2D::framebuffer())
          .set_scissor(0, vuk::Rect2D::framebuffer())
          .bind_image(0, 0, grid)
          .bind_image(0, 1, source)
          .bind_sampler(0, 2, vuk::LinearSamplerClamped)
          .draw(3, 1, 0, 0);

        return std::make_tuple(out, source, grid);
      }
    );

    auto grid_applied_attachment = vuk::declare_ia(
      "grid_applied_attachment",
      {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
       .sample_count = vuk::Samples::e1}
    );
    grid_applied_attachment.same_format_as(result_attachment);
    grid_applied_attachment.same_shape_as(result_attachment);
    grid_applied_attachment = vuk::clear_image(grid_applied_attachment, vuk::Black<f32>);

    std::tie(grid_applied_attachment, result_attachment, grid_attachment) = apply_grid_pass(
      grid_applied_attachment,
      result_attachment,
      grid_attachment
    );

    ctx.set_image_resource("result_attachment", std::move(grid_applied_attachment))
      .set_image_resource("depth_attachment", std::move(depth_attachment))
      .set_buffer_resource("camera_buffer", std::move(camera_buffer));
  });
}

void ViewportPanel::transform_gizmos_button_group(this ViewportPanel& self, ImVec2 start_cursor_pos) {
  const float frame_height = 1.3f * ImGui::GetFrameHeight();
  const ImVec2 frame_padding = ImGui::GetStyle().FramePadding;
  const ImVec2 button_size = {frame_height, frame_height};
  constexpr float button_count = 8.0f;
  const ImVec2 window_pos = ImGui::GetWindowPos();
  const ImVec2 content_min = ImGui::GetWindowContentRegionMin();
  const ImVec2 panel_top_left = {window_pos.x + content_min.x, window_pos.y + content_min.y};

  const ImVec2 gizmo_pos = {panel_top_left.x + self.gizmo_position.x, panel_top_left.y + self.gizmo_position.y};
  const ImRect bb(
    gizmo_pos.x,
    gizmo_pos.y,
    gizmo_pos.x + button_size.x + 8,
    gizmo_pos.y + (button_size.y + 2) * (button_count + 0.5f)
  );
  ImVec4 frame_color = ImGui::GetStyleColorVec4(ImGuiCol_Tab);
  frame_color.w = 0.5f;
  ImGui::RenderFrame(bb.Min, bb.Max, ImGui::GetColorU32(frame_color), false, ImGui::GetStyle().FrameRounding);

  const auto temp_gizmo_position = self.gizmo_position;
  ImGui::SetCursorPos(
    {start_cursor_pos.x + temp_gizmo_position.x + frame_padding.x, start_cursor_pos.y + temp_gizmo_position.y}
  );
  ImGui::BeginGroup();
  {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {1, 1});

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
      self.gizmo_position.x += mouse_pos.x - last_mouse_position.x;
      self.gizmo_position.y += mouse_pos.y - last_mouse_position.y;
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
    if (UI::toggle_button(ICON_MDI_GRID, EditorCVar::cvar_draw_grid.get(), button_size, alpha, alpha))
      EditorCVar::cvar_draw_grid.toggle();

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
  constexpr float y_pad = 3.0f;
  const ImVec2 button_size = {35.f, 25.f};
  const ImVec2 group_size = {button_size.x * button_count, button_size.y + y_pad};

  ImGui::SetCursorPos({self.viewport_size.x * 0.5f - (group_size.x * 0.5f), start_cursor_pos.y + y_pad});
  ImGui::BeginGroup();
  {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {1, 1});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 1.0f);

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
