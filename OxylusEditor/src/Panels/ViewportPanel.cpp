#include "ViewportPanel.hpp"

#include <ImGuizmo.h>
#include <icons/IconsMaterialDesignIcons.h>
#include <imgui.h>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Core/Input.hpp"
#include "EditorLayer.hpp"
#include "EditorUI.hpp"
#include "Render/Camera.hpp"
#include "Render/RendererConfig.hpp"
#include "Render/Vulkan/VkContext.hpp"
#include "Scene/ECSModule/Core.hpp"
#include "UI/ImGuiLayer.hpp"
#include "Utils/EditorConfig.hpp"
#include "Utils/OxMath.hpp"
#include "Utils/PayloadData.hpp"

namespace ox {
template <typename T>
void show_component_gizmo(const char* icon,
                          const std::string& name,
                          const float width,
                          const float height,
                          const float xpos,
                          const float ypos,
                          const glm::mat4& view_proj,
                          const Frustum& frustum,
                          Scene* scene) {
  scene->world.query_builder<T>().build().each(
      [view_proj, width, height, xpos, ypos, scene, frustum, icon, name](flecs::entity entity, const T&) {
        const glm::vec3 pos = scene->get_world_transform(entity)[3];

        if (frustum.is_inside(pos) == (uint32_t)Intersection::Outside)
          return;

        const glm::vec2 screen_pos = math::world_to_screen(pos, view_proj, width, height, xpos, ypos);
        ImGui::SetCursorPos({screen_pos.x - ImGui::GetFontSize() * 0.5f, screen_pos.y - ImGui::GetFontSize() * 0.5f});
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.7f, 0.7f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.1f, 0.1f, 0.1f, 0.1f));

        constexpr auto icon_size = 48.f;
        ImGui::PushFont(nullptr, icon_size);
        if (ImGui::Button(icon, {50.f, 50.f})) {
          auto& editor_context = EditorLayer::get()->get_context();
          editor_context.reset();
          editor_context.entity = entity;
          editor_context.type = EditorContext::Type::Entity;
        }
        ImGui::PopFont();

        ImGui::PopStyleColor(2);

        UI::tooltip_hover(name.data());
      });
}

ViewportPanel::ViewportPanel() : EditorPanel("Viewport", ICON_MDI_TERRAIN, true) { ZoneScoped; }

void ViewportPanel::on_render(const vuk::Extent3D extent, vuk::Format format) {
  draw_performance_overlay();

  constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_MenuBar;

  if (on_begin(flags)) {
    bool viewport_settings_popup = false;
    ImVec2 start_cursor_pos = ImGui::GetCursorPos();

    auto& style = ImGui::GetStyle();

    auto* editor_layer = EditorLayer::get();

    auto& editor_theme = editor_layer->editor_theme;

    if (ImGui::BeginMenuBar()) {
      if (ImGui::MenuItem(ICON_MDI_COG)) {
        viewport_settings_popup = true;
      }
      auto button_width = ImGui::CalcTextSize(ICON_MDI_ARROW_EXPAND_ALL, nullptr, true);
      ImGui::SetCursorPosX(_viewport_panel_size.x - button_width.x - (style.ItemInnerSpacing.x * 2.f));
      if (ImGui::MenuItem(ICON_MDI_ARROW_EXPAND_ALL)) {
        fullscreen_viewport = !fullscreen_viewport;
      }
      ImGui::EndMenuBar();
    }

    if (viewport_settings_popup)
      ImGui::OpenPopup("ViewportSettings");

    ImGui::SetNextWindowSize(ImVec2(345, 0));
    ImGui::SetNextWindowBgAlpha(0.85f);
    if (ImGui::BeginPopup("ViewportSettings")) {
      draw_settings_panel();
      ImGui::EndPopup();
    }

    const auto viewport_min_region = ImGui::GetWindowContentRegionMin();
    const auto viewport_max_region = ImGui::GetWindowContentRegionMax();
    _viewport_position = glm::vec2(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y);
    _viewport_bounds[0] = {viewport_min_region.x + _viewport_position.x, viewport_min_region.y + _viewport_position.y};
    _viewport_bounds[1] = {viewport_max_region.x + _viewport_position.x, viewport_max_region.y + _viewport_position.y};

    is_viewport_focused = ImGui::IsWindowFocused();
    is_viewport_hovered = ImGui::IsWindowHovered();

    _viewport_panel_size = glm::vec2(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y);
    if ((int)_viewport_size.x != (int)_viewport_panel_size.x || (int)_viewport_size.y != (int)_viewport_panel_size.y) {
      _viewport_size = {_viewport_panel_size.x, _viewport_panel_size.y};
    }

    constexpr auto sixteen_nine_ar = 1.7777777f;
    const auto fixed_width = _viewport_size.y * sixteen_nine_ar;
    ImGui::SetCursorPosX((_viewport_panel_size.x - fixed_width) * 0.5f);

    const auto off = (_viewport_panel_size.x - fixed_width) *
                     0.5f; // add offset since we render image with fixed aspect ratio
    _viewport_offset = {_viewport_bounds[0].x + off * 0.5f, _viewport_bounds[0].y};

    const auto* app = App::get();
    auto renderer_instance = _scene->get_renderer_instance();
    if (renderer_instance != nullptr) {
      const Renderer::RenderInfo render_info = {
          .extent = extent,
          .format = format,
          .picking_texel = {},
      };
      auto scene_view_image = renderer_instance->render(render_info);
      ImGui::Image(app->get_imgui_layer()->add_image(std::move(scene_view_image)),
                   ImVec2{fixed_width, _viewport_panel_size.y});
    } else {
      const auto warning_text = "No scene render output!";
      const auto text_width = ImGui::CalcTextSize(warning_text).x;
      ImGui::SetCursorPosX((_viewport_size.x - text_width) * 0.5f);
      ImGui::SetCursorPosY(_viewport_size.y * 0.5f);
      ImGui::Text(warning_text);
    }

    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* imgui_payload = ImGui::AcceptDragDropPayload(PayloadData::DRAG_DROP_SOURCE)) {
        const auto* payload = PayloadData::from_payload(imgui_payload);
        const auto path = ::fs::path(payload->get_str());
        if (path.extension() == ".oxscene") {
          editor_layer->open_scene(path);
        }
        if (path.extension() == ".gltf" || path.extension() == ".glb") {
          auto* asset_man = App::get_asset_manager();
          if (auto asset = asset_man->import_asset(path.string()))
            _scene->create_mesh_entity(asset);
        }
      }

      ImGui::EndDragDropTarget();
    }

    if (editor_camera.has<CameraComponent>() && !_scene->is_running()) {
      if (editor_layer->scene_state == EditorLayer::SceneState::Edit)
        editor_camera.enable();

      const auto& cam = editor_camera.get<CameraComponent>();
      auto projection = cam.get_projection_matrix();
      projection[1][1] *= -1;
      glm::mat4 view_proj = projection * cam.get_view_matrix();
      const Frustum& frustum = Camera::get_frustum(cam, cam.position);

      show_component_gizmo<LightComponent>(editor_theme.component_icon_map.at(typeid(LightComponent).hash_code()),
                                           "LightComponent",
                                           fixed_width,
                                           _viewport_panel_size.y,
                                           0,
                                           0,
                                           view_proj,
                                           frustum,
                                           _scene.get());
      show_component_gizmo<AudioSourceComponent>(
          editor_theme.component_icon_map.at(typeid(AudioSourceComponent).hash_code()),
          "AudioSourceComponent",
          fixed_width,
          _viewport_panel_size.y,
          0,
          0,
          view_proj,
          frustum,
          _scene.get());
      show_component_gizmo<AudioListenerComponent>(
          editor_theme.component_icon_map.at(typeid(AudioListenerComponent).hash_code()),
          "AudioListenerComponent",
          fixed_width,
          _viewport_panel_size.y,
          0,
          0,
          view_proj,
          frustum,
          _scene.get());
      show_component_gizmo<CameraComponent>(editor_theme.component_icon_map.at(typeid(CameraComponent).hash_code()),
                                            "CameraComponent",
                                            fixed_width,
                                            _viewport_panel_size.y,
                                            0,
                                            0,
                                            view_proj,
                                            frustum,
                                            _scene.get());

      draw_gizmos();
    }
    {
      // Transform Gizmos Button Group
      const float frame_height = 1.3f * ImGui::GetFrameHeight();
      const ImVec2 frame_padding = ImGui::GetStyle().FramePadding;
      const ImVec2 button_size = {frame_height, frame_height};
      constexpr float button_count = 8.0f;
      const ImVec2 gizmo_position = {_viewport_bounds[0].x + _gizmo_position.x,
                                     _viewport_bounds[0].y + _gizmo_position.y};
      const ImRect bb(gizmo_position.x,
                      gizmo_position.y,
                      gizmo_position.x + button_size.x + 8,
                      gizmo_position.y + (button_size.y + 2) * (button_count + 0.5f));
      ImVec4 frame_color = ImGui::GetStyleColorVec4(ImGuiCol_Tab);
      frame_color.w = 0.5f;
      ImGui::RenderFrame(bb.Min, bb.Max, ImGui::GetColorU32(frame_color), false, ImGui::GetStyle().FrameRounding);
      const glm::vec2 temp_gizmo_position = _gizmo_position;

      ImGui::SetCursorPos(
          {start_cursor_pos.x + temp_gizmo_position.x + frame_padding.x, start_cursor_pos.y + temp_gizmo_position.y});
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
          _gizmo_position.x += mouse_pos.x - last_mouse_position.x;
          _gizmo_position.y += mouse_pos.y - last_mouse_position.y;
        }
        last_mouse_position = mouse_pos;

        constexpr float alpha = 0.6f;
        if (UI::toggle_button(ICON_MDI_AXIS_ARROW, _gizmo_type == ImGuizmo::TRANSLATE, button_size, alpha, alpha))
          _gizmo_type = ImGuizmo::TRANSLATE;
        if (UI::toggle_button(ICON_MDI_ROTATE_3D, _gizmo_type == ImGuizmo::ROTATE, button_size, alpha, alpha))
          _gizmo_type = ImGuizmo::ROTATE;
        if (UI::toggle_button(ICON_MDI_ARROW_EXPAND, _gizmo_type == ImGuizmo::SCALE, button_size, alpha, alpha))
          _gizmo_type = ImGuizmo::SCALE;
        if (UI::toggle_button(ICON_MDI_VECTOR_SQUARE, _gizmo_type == ImGuizmo::BOUNDS, button_size, alpha, alpha))
          _gizmo_type = ImGuizmo::BOUNDS;
        if (UI::toggle_button(ICON_MDI_ARROW_EXPAND_ALL, _gizmo_type == ImGuizmo::UNIVERSAL, button_size, alpha, alpha))
          _gizmo_type = ImGuizmo::UNIVERSAL;
        if (UI::toggle_button(_gizmo_mode == ImGuizmo::WORLD ? ICON_MDI_EARTH : ICON_MDI_EARTH_OFF,
                              _gizmo_mode == ImGuizmo::WORLD,
                              button_size,
                              alpha,
                              alpha))
          _gizmo_mode = _gizmo_mode == ImGuizmo::LOCAL ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
        if (UI::toggle_button(ICON_MDI_GRID, RendererCVar::cvar_draw_grid.get(), button_size, alpha, alpha))
          RendererCVar::cvar_draw_grid.toggle();

        if (editor_camera.has<CameraComponent>()) {
          auto& cam = editor_camera.get_mut<CameraComponent>();
          UI::push_id();
          if (UI::toggle_button(ICON_MDI_CAMERA,
                                cam.projection == CameraComponent::Projection::Orthographic,
                                button_size,
                                alpha,
                                alpha))
            cam.projection = cam.projection == CameraComponent::Projection::Orthographic
                                 ? CameraComponent::Projection::Perspective
                                 : CameraComponent::Projection::Orthographic;
        }
        UI::pop_id();

        ImGui::PopStyleVar(2);
      }
      ImGui::EndGroup();
    }
    {
      // Scene Button Group
      constexpr float button_count = 3.0f;
      constexpr float y_pad = 3.0f;
      const ImVec2 button_size = {35.f, 25.f};
      const ImVec2 group_size = {button_size.x * button_count, button_size.y + y_pad};

      ImGui::SetCursorPos({_viewport_size.x * 0.5f - (group_size.x * 0.5f), start_cursor_pos.y + y_pad});
      ImGui::BeginGroup();
      {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {1, 1});
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 1.0f);

        const bool highlight = editor_layer->scene_state == EditorLayer::SceneState::Play;
        const char* icon = editor_layer->scene_state == EditorLayer::SceneState::Edit ? ICON_MDI_PLAY : ICON_MDI_STOP;
        if (UI::toggle_button(icon, highlight, button_size)) {
          if (editor_layer->scene_state == EditorLayer::SceneState::Edit) {
            editor_layer->on_scene_play();
            editor_camera.disable();
          } else if (editor_layer->scene_state == EditorLayer::SceneState::Play) {
            editor_layer->on_scene_stop();
          }
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 0.4f));
        if (ImGui::Button(ICON_MDI_PAUSE, button_size)) {
          if (editor_layer->scene_state == EditorLayer::SceneState::Play)
            editor_layer->on_scene_stop();
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_MDI_STEP_FORWARD, button_size)) {
          editor_layer->on_scene_simulate();
        }
        ImGui::PopStyleColor();

        ImGui::PopStyleVar(3);
      }
      ImGui::EndGroup();
    }
  }
  on_end();
}

void ViewportPanel::set_context(const std::shared_ptr<Scene>& scene, SceneHierarchyPanel& scene_hierarchy_panel) {
  _scene_hierarchy_panel = &scene_hierarchy_panel;

  if (!scene)
    return;

  this->_scene = scene;

  editor_camera = _scene->create_entity("editor_camera");
  editor_camera.add<CameraComponent>().add<Hidden>();
}

void ViewportPanel::on_update() {
  if (!is_viewport_hovered || _scene->is_running() || !editor_camera.has<CameraComponent>()) {
    return;
  }

  auto& cam = editor_camera.get_mut<CameraComponent>();
  auto& tc = editor_camera.get_mut<TransformComponent>();
  const glm::vec3& position = cam.position;
  const glm::vec2 yaw_pitch = glm::vec2(cam.yaw, cam.pitch);
  glm::vec3 final_position = position;
  glm::vec2 final_yaw_pitch = yaw_pitch;

  const auto is_ortho = cam.projection == CameraComponent::Projection::Orthographic;
  if (is_ortho) {
    final_position = {0.0f, 0.0f, 0.0f};
    final_yaw_pitch = {glm::radians(-90.f), 0.f};
  }

  const auto& window = App::get()->get_window();

  if (Input::get_key_pressed(KeyCode::F)) {
    auto& editor_context = EditorLayer::get()->get_context();
    editor_context.entity.and_then([&cam](flecs::entity& e) {
      const auto entity_tc = e.get<TransformComponent>();
      auto final_pos = entity_tc.position + cam.forward;
      final_pos += -5.0f * cam.forward * glm::vec3(1.0f);
      cam.position = final_pos;
      return std::optional<std::monostate>{};
    });
  }

  if (Input::get_mouse_held(MouseCode::ButtonRight) && !is_ortho) {
    const glm::vec2 new_mouse_position = Input::get_mouse_position_rel();
    window.set_cursor(WindowCursor::Crosshair);

    if (Input::get_mouse_moved()) {
      const glm::vec2 change = new_mouse_position * EditorCVar::cvar_camera_sens.get();
      final_yaw_pitch.x += change.x;
      final_yaw_pitch.y = glm::clamp(final_yaw_pitch.y - change.y, glm::radians(-89.9f), glm::radians(89.9f));
    }

    const float max_move_speed = EditorCVar::cvar_camera_speed.get() *
                                 (ImGui::IsKeyDown(ImGuiKey_LeftShift) ? 3.0f : 1.0f);
    if (Input::get_key_held(KeyCode::W))
      final_position += cam.forward * max_move_speed;
    else if (Input::get_key_held(KeyCode::S))
      final_position -= cam.forward * max_move_speed;
    if (Input::get_key_held(KeyCode::D))
      final_position += cam.right * max_move_speed;
    else if (Input::get_key_held(KeyCode::A))
      final_position -= cam.right * max_move_speed;

    if (Input::get_key_held(KeyCode::Q)) {
      final_position.y -= max_move_speed;
    } else if (Input::get_key_held(KeyCode::E)) {
      final_position.y += max_move_speed;
    }
  }
  // Panning
  else if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
    const glm::vec2 new_mouse_position = Input::get_mouse_position_rel();
    window.set_cursor(WindowCursor::ResizeAll);

    const glm::vec2 change = (new_mouse_position - _locked_mouse_position) * EditorCVar::cvar_camera_sens.get();

    if (Input::get_mouse_moved()) {
      const float max_move_speed = EditorCVar::cvar_camera_speed.get() *
                                   (ImGui::IsKeyDown(ImGuiKey_LeftShift) ? 3.0f : 1.0f);
      final_position += cam.forward * change.y * max_move_speed;
      final_position += cam.right * change.x * max_move_speed;
    }
  } else {
    window.set_cursor(WindowCursor::Arrow);
  }

  const glm::vec3 damped_position = math::smooth_damp(position,
                                                      final_position,
                                                      _translation_velocity,
                                                      _translation_dampening,
                                                      10000.0f,
                                                      static_cast<float>(App::get_timestep().get_seconds()));
  const glm::vec2 damped_yaw_pitch = math::smooth_damp(yaw_pitch,
                                                       final_yaw_pitch,
                                                       _rotation_velocity,
                                                       _rotation_dampening,
                                                       1000.0f,
                                                       static_cast<float>(App::get_timestep().get_seconds()));

  tc.position = EditorCVar::cvar_camera_smooth.get() ? damped_position : final_position;
  tc.rotation.x = EditorCVar::cvar_camera_smooth.get() ? damped_yaw_pitch.y : final_yaw_pitch.y;
  tc.rotation.y = EditorCVar::cvar_camera_smooth.get() ? damped_yaw_pitch.x : final_yaw_pitch.x;

  cam.zoom = static_cast<float>(EditorCVar::cvar_camera_zoom.get());
}

void ViewportPanel::draw_performance_overlay() {
  if (!performance_overlay_visible)
    return;
  UI::draw_framerate_overlay(ImVec2(_viewport_position.x, _viewport_position.y),
                             ImVec2(_viewport_panel_size.x, _viewport_panel_size.y),
                             {15, 55},
                             &performance_overlay_visible);
}

void ViewportPanel::draw_settings_panel() {
  ZoneScoped;

  i32 open_action = -1;

  if (UI::button("Expand All"))
    open_action = 1;
  ImGui::SameLine();
  if (UI::button("Collapse All"))
    open_action = 0;

  constexpr ImGuiTreeNodeFlags TREE_FLAGS = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowItemOverlap |
                                            ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_FramePadding;
  if (open_action != -1)
    ImGui::SetNextItemOpen(open_action != 0);
  if (ImGui::TreeNodeEx("Renderer", TREE_FLAGS, "%s", "Renderer")) {
    ImGui::Text("GPU: %s", App::get_vkcontext().device_name.c_str());
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
        UI::property("Enable physics debug renderer",
                     (bool*)RendererCVar::cvar_enable_physics_debug_renderer.get_ptr());
        UI::property("Draw bounding boxes", (bool*)RendererCVar::cvar_draw_bounding_boxes.get_ptr());
        UI::property("Freeze culling frustum", (bool*)RendererCVar::cvar_freeze_culling_frustum.get_ptr());
        UI::property("Draw camera frustum", (bool*)RendererCVar::cvar_draw_camera_frustum.get_ptr());
        const char* debug_views[11] = {"None",
                                       "Triangles",
                                       "Meshlets",
                                       "Overdraw",
                                       "Albdeo",
                                       "Normal",
                                       "Emissive",
                                       "Metallic",
                                       "Roughness",
                                       "Occlusion",
                                       "HiZ"};
        UI::property("Debug View", RendererCVar::cvar_debug_view.get_ptr(), debug_views, 11);
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
        UI::property("Enabled", (bool*)RendererCVar::cvar_gtao_enable.get_ptr());
        UI::property<int>("Denoise Passes", RendererCVar::cvar_gtao_denoise_passes.get_ptr(), 1, 5);
        UI::property<float>("Radius", RendererCVar::cvar_gtao_radius.get_ptr(), 0, 1);
        UI::property<float>("Falloff Range", RendererCVar::cvar_gtao_falloff_range.get_ptr(), 0, 1);
        UI::property<float>(
            "Sample Distribution Power", RendererCVar::cvar_gtao_sample_distribution_power.get_ptr(), 0, 5);
        UI::property<float>(
            "Thin Occluder Compensation", RendererCVar::cvar_gtao_thin_occluder_compensation.get_ptr(), 0, 5);
        UI::property<float>("Final Value Power", RendererCVar::cvar_gtao_final_value_power.get_ptr(), 0, 5);
        UI::property<float>(
            "Depth Mip Sampling Offset", RendererCVar::cvar_gtao_depth_mip_sampling_offset.get_ptr(), 0, 5);
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
      UI::property("Draw grid", (bool*)RendererCVar::cvar_draw_grid.get_ptr());
      UI::property<float>("Grid distance", RendererCVar::cvar_draw_grid_distance.get_ptr(), 10.f, 100.0f);
      UI::end_properties();
    }

    if (open_action != -1)
      ImGui::SetNextItemOpen(open_action != 0);
    if (ImGui::TreeNodeEx("Camera", TREE_FLAGS, "%s", "Camera")) {
      if (UI::begin_properties(UI::default_properties_flags, true, 0.3f)) {
        UI::property<float>("Camera sensitivity", EditorCVar::cvar_camera_sens.get_ptr(), 0.1f, 20.0f);
        UI::property<float>("Movement speed", EditorCVar::cvar_camera_speed.get_ptr(), 5, 100.0f);
        UI::property("Smooth camera", (bool*)EditorCVar::cvar_camera_smooth.get_ptr());
        UI::property("Camera zoom", EditorCVar::cvar_camera_zoom.get_ptr(), 1, 100);
        UI::end_properties();
      }

      ImGui::TreePop();
    }

    ImGui::TreePop();
  }
}

void ViewportPanel::draw_gizmos() {
  auto* editor_layer = EditorLayer::get();
  auto& editor_context = editor_layer->get_context();
  auto& undo_redo_system = editor_layer->undo_redo_system;

  const flecs::entity selected_entity = editor_context.entity.value_or(flecs::entity::null());

  if (Input::get_key_held(KeyCode::LeftControl)) {
    if (Input::get_key_pressed(KeyCode::Q)) {
      if (!ImGuizmo::IsUsing())
        _gizmo_type = -1;
    }
    if (Input::get_key_pressed(KeyCode::W)) {
      if (!ImGuizmo::IsUsing())
        _gizmo_type = ImGuizmo::OPERATION::TRANSLATE;
    }
    if (Input::get_key_pressed(KeyCode::E)) {
      if (!ImGuizmo::IsUsing())
        _gizmo_type = ImGuizmo::OPERATION::ROTATE;
    }
    if (Input::get_key_pressed(KeyCode::R)) {
      if (!ImGuizmo::IsUsing())
        _gizmo_type = ImGuizmo::OPERATION::SCALE;
    }
  }

  if (selected_entity == flecs::entity::null() || !editor_camera.has<CameraComponent>() || _gizmo_type == -1)
    return;

  if (auto* tc = selected_entity.try_get_mut<TransformComponent>()) {
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(_viewport_bounds[0].x,
                      _viewport_bounds[0].y,
                      _viewport_bounds[1].x - _viewport_bounds[0].x,
                      _viewport_bounds[1].y - _viewport_bounds[0].y);

    const auto& cam = editor_camera.get<CameraComponent>();

    auto camera_projection = cam.get_projection_matrix();
    camera_projection[1][1] *= -1;

    const glm::mat4& camera_view = cam.get_view_matrix();

    glm::mat4 transform = _scene->get_world_transform(selected_entity);

    // Snapping
    const bool snap = Input::get_key_held(KeyCode::LeftControl);
    float snap_value = 0.5f; // Snap to 0.5m for translation/scale
    // Snap to 45 degrees for rotation
    if (_gizmo_type == ImGuizmo::OPERATION::ROTATE)
      snap_value = 45.0f;

    const float snap_values[3] = {snap_value, snap_value, snap_value};

    const auto is_ortho = cam.projection == CameraComponent::Projection::Orthographic;
    ImGuizmo::SetOrthographic(is_ortho);
    if (_gizmo_mode == ImGuizmo::OPERATION::TRANSLATE && is_ortho)
      _gizmo_mode = ImGuizmo::OPERATION::TRANSLATE_X | ImGuizmo::OPERATION::TRANSLATE_Y;

    ImGuizmo::Manipulate(value_ptr(camera_view),
                         value_ptr(camera_projection),
                         static_cast<ImGuizmo::OPERATION>(_gizmo_type),
                         static_cast<ImGuizmo::MODE>(_gizmo_mode),
                         value_ptr(transform),
                         nullptr,
                         snap ? snap_values : nullptr);

    if (ImGuizmo::IsUsing()) {
      const flecs::entity parent = selected_entity.parent();
      const glm::mat4& parent_world_transform = parent != flecs::entity::null() ? _scene->get_world_transform(parent)
                                                                                : glm::mat4(1.0f);
      glm::vec3 translation, rotation, scale;
      if (math::decompose_transform(inverse(parent_world_transform) * transform, translation, rotation, scale)) {
        tc->position = translation;
        const glm::vec3 delta_rotation = rotation - tc->rotation;
        tc->rotation += delta_rotation;
        tc->scale = scale;

        auto old_tc = *tc;
        undo_redo_system->execute_command<PropertyChangeCommand<TransformComponent>>(
            tc, old_tc, *tc, "gizmo transform");

        selected_entity.modified<TransformComponent>();
      }
    }
  }
}
} // namespace ox
