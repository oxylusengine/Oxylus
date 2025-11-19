#include "ViewportPanel.hpp"

#include <ImGuizmo.h>
#include <icons/IconsMaterialDesignIcons.h>
#include <imgui.h>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Core/Input.hpp"
#include "Editor.hpp"
#include "Render/Camera.hpp"
#include "Render/RendererConfig.hpp"
#include "Render/Slang/Slang.hpp"
#include "Render/Utils/VukCommon.hpp"
#include "Render/Vulkan/VkContext.hpp"
#include "Scene/ECSModule/Core.hpp"
#include "UI/ImGuiRenderer.hpp"
#include "UI/PayloadData.hpp"
#include "UI/UI.hpp"
#include "Utils/EditorConfig.hpp"
#include "Utils/OxMath.hpp"

namespace ox {
template <typename T>
void show_component_gizmo(
  f32 icon_size,
  const std::string& name,
  const float width,
  const float height,
  const float xpos,
  const float ypos,
  const glm::mat4& view_proj,
  const Frustum& frustum,
  Scene* scene
) {
  auto& editor = App::mod<Editor>();
  auto& editor_theme = editor.editor_theme;

  const char* icon = editor_theme.component_icon_map.at(typeid(T).hash_code());
  scene->world.query_builder<T>().build().each([&](flecs::entity entity, const T&) {
    const glm::vec3 pos = Scene::get_world_transform(entity)[3];

    if (entity.has<Hidden>())
      return;

    if (frustum.is_inside(pos) == (uint32_t)Intersection::Outside)
      return;

    const glm::vec2 screen_pos = math::world_to_screen(pos, view_proj, width, height, xpos, ypos);
    ImGui::SetCursorPos({screen_pos.x - (icon_size / 2.f), screen_pos.y - (icon_size / 2.f)});
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.7f, 0.7f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.1f, 0.1f, 0.1f, 0.1f));

    ImGui::PushFont(nullptr, icon_size);
    ImGui::PushID(entity.id());
    if (ImGui::Button(icon, {icon_size, icon_size})) {
      auto& editor_context = editor.get_context();
      editor_context.reset(EditorContext::Type::Entity, nullopt, entity);
    }
    ImGui::PopID();
    ImGui::PopFont();

    ImGui::PopStyleColor(2);

    UI::tooltip_hover(name.data());
  });
}

ViewportPanel::ViewportPanel() : EditorPanel("Viewport", ICON_MDI_TERRAIN, true) {
  ZoneScoped;

  auto& vk_context = App::get_vkcontext();
  auto& runtime = *vk_context.runtime;
  if (!runtime.is_pipeline_available("mouse_picking_pipeline")) {
    auto& vfs = App::get_vfs();
    auto shaders_dir = vfs.resolve_physical_dir(VFS::APP_DIR, "Shaders");
    Slang slang = {};
    slang.create_session({.root_directory = shaders_dir, .definitions = {}});

    slang.create_pipeline(
      runtime,
      "mouse_picking_pipeline",
      {.path = shaders_dir / "editor/mouse_picking.slang", .entry_points = {"cs_main"}}
    );

    slang.create_pipeline(
      runtime,
      "highlighting_pipeline",
      {.path = shaders_dir / "editor/highlighting.slang", .entry_points = {"cs_main"}}
    );

    slang.create_pipeline(
      runtime,
      "apply_highlighting_pipeline",
      {.path = shaders_dir / "editor/apply_highlighting.slang", .entry_points = {"vs_main", "fs_main"}}
    );

    slang.create_pipeline(
      runtime,
      "grid_pipeline",
      {.path = shaders_dir / "editor/grid.slang", .entry_points = {"vs_main", "fs_main"}}
    );

    slang.create_pipeline(
      runtime,
      "apply_grid_pipeline",
      {.path = shaders_dir / "editor/apply_grid.slang", .entry_points = {"vs_main", "fs_main"}}
    );
  }
}

ViewportPanel::~ViewportPanel() {
  auto& event_system = App::get_event_system();
  if (editor_scene_ && editor_scene_->is_playing())
    event_system.emit<Editor::SceneStopEvent>(Editor::SceneStopEvent(editor_scene_->get_id()));
}

void ViewportPanel::on_render(vuk::ImageAttachment swapchain_attachment) {
  constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_MenuBar;

  auto& editor = App::mod<Editor>();

  if (on_begin(flags)) {
    if (ImGui::BeginPopupContextItem("viewport context")) {
      if (ImGui::MenuItem("Unload Scene")) {
        editor_scene_ = nullptr;
        set_name("Viewport");
        editor.reset_current_docking_layout();
      }
      ImGui::EndPopup();
    }

    bool viewport_settings_popup = false;
    bool gizmo_settings_popup = false;
    ImVec2 start_cursor_pos = ImGui::GetCursorPos();

    auto& style = ImGui::GetStyle();

    if (ImGui::BeginMenuBar()) {
      if (ImGui::MenuItem(ICON_MDI_COG)) {
        viewport_settings_popup = true;
      }
      if (ImGui::MenuItem(ICON_MDI_INFORMATION, nullptr, draw_scene_stats)) {
        draw_scene_stats = !draw_scene_stats;
      }
      if (ImGui::MenuItem(ICON_MDI_SPHERE, nullptr, gizmo_settings_popup)) {
        gizmo_settings_popup = true;
      }
      auto button_width = ImGui::CalcTextSize(ICON_MDI_ARROW_EXPAND_ALL, nullptr, true);
      ImGui::SetCursorPosX(viewport_panel_size_.x - button_width.x - (style.ItemInnerSpacing.x * 2.f));
      if (ImGui::MenuItem(ICON_MDI_ARROW_EXPAND_ALL)) {
        fullscreen_viewport = !fullscreen_viewport;
      }
      ImGui::EndMenuBar();
    }

    draw_stats_overlay(draw_scene_stats);

    if (viewport_settings_popup)
      ImGui::OpenPopup("viewport_settings");

    ImGui::SetNextWindowSize(ImVec2(345, 0));
    ImGui::SetNextWindowBgAlpha(0.85f);
    if (ImGui::BeginPopup("viewport_settings")) {
      draw_settings_panel();
      ImGui::EndPopup();
    }

    if (gizmo_settings_popup)
      ImGui::OpenPopup("gizmo_settings");

    ImGui::SetNextWindowSize(ImVec2(345, 0));
    ImGui::SetNextWindowBgAlpha(0.85f);
    if (ImGui::BeginPopup("gizmo_settings")) {
      draw_gizmo_settings_panel();
      ImGui::EndPopup();
    }

    const ImVec2 viewport_min_region = ImGui::GetWindowContentRegionMin();
    const ImVec2 viewport_max_region = ImGui::GetWindowContentRegionMax();
    viewport_position_ = glm::vec2(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y);
    viewport_bounds_[0] = {viewport_min_region.x + viewport_position_.x, viewport_min_region.y + viewport_position_.y};
    viewport_bounds_[1] = {viewport_max_region.x + viewport_position_.x, viewport_max_region.y + viewport_position_.y};

    is_viewport_focused = ImGui::IsWindowFocused();
    is_viewport_hovered = ImGui::IsWindowHovered();

    viewport_panel_size_ = glm::vec2(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y);
    viewport_size_ = {viewport_panel_size_.x, viewport_panel_size_.y};

    constexpr auto sixteen_nine_ar = 1.7777777f;
    const auto fixed_width = viewport_size_.y * sixteen_nine_ar;
    ImGui::SetCursorPosX((viewport_panel_size_.x - fixed_width) * 0.5f);

    const auto off = (viewport_panel_size_.x - fixed_width) *
                     0.5f; // add offset since we render image with fixed aspect ratio
    viewport_offset_ = {viewport_bounds_[0].x + off * 0.5f, viewport_bounds_[0].y};

    if (!editor_scene_) {
      const auto warning_text = "No scene!";
      const auto text_width = ImGui::CalcTextSize(warning_text).x;
      ImGui::SetCursorPosX((viewport_size_.x - text_width) * 0.5f);
      ImGui::SetCursorPosY(viewport_size_.y * 0.5f);
      ImGui::Text(warning_text);

      on_end();

      return;
    }

    auto renderer_instance = editor_scene_->get_scene()->get_renderer_instance();
    if (!renderer_instance) {
      const auto warning_text = "No scene render output!";
      const auto text_width = ImGui::CalcTextSize(warning_text).x;
      ImGui::SetCursorPosX((viewport_size_.x - text_width) * 0.5f);
      ImGui::SetCursorPosY(viewport_size_.y * 0.5f);
      ImGui::Text(warning_text);
    } else {
      constexpr auto get_mouse_texel_coords =
        [](glm::uvec2 render_size, glm::vec2 window_pos, ImVec2 content_min, ImVec2 content_max, ImVec2 mouse_pos)
        -> glm::uvec2 {
        ImVec2 rendered_min = {window_pos.x + content_min.x, window_pos.y + content_min.y};
        ImVec2 rendered_max = {window_pos.x + content_max.x, window_pos.y + content_max.y};
        ImVec2 rendered_size = {rendered_max.x - rendered_min.x, rendered_max.y - rendered_min.y};

        if (mouse_pos.x < rendered_min.x || mouse_pos.x > rendered_max.x || mouse_pos.y < rendered_min.y ||
            mouse_pos.y > rendered_max.y) {
          return glm::uvec2(~0_u32);
        }

        glm::vec2 mouse_rel = {mouse_pos.x - rendered_min.x, mouse_pos.y - rendered_min.y};

        return glm::uvec2{
          static_cast<u32>((mouse_rel.x / rendered_size.x) * render_size.x),
          static_cast<u32>((mouse_rel.y / rendered_size.y) * render_size.y)
        };
      };

      auto mouse_pos = ImGui::GetMousePos();
      glm::uvec2 picking_texel = get_mouse_texel_coords(
        {swapchain_attachment.extent.width, swapchain_attachment.extent.height},
        viewport_position_,
        viewport_min_region,
        viewport_max_region,
        mouse_pos
      );

      if (mouse_picking_enabled_) {
        mouse_picking_stages(renderer_instance, picking_texel);
      }

      if (EditorCVar::cvar_draw_grid.as_bool()) {
        grid_stage(renderer_instance);
      }

      const Renderer::RenderInfo render_info = {
        .viewport_offset = {viewport_position_.x, viewport_position_.y},
      };

      auto viewport_attachment = vuk::declare_ia("viewport", swapchain_attachment);
      viewport_attachment = vuk::clear_image(std::move(viewport_attachment), vuk::Black<f32>);

      auto scene_view_image = renderer_instance->render(std::move(viewport_attachment), render_info);
      editor_scene_->get_scene()->on_viewport_render(swapchain_attachment.extent, swapchain_attachment.format);
      UI::image(std::move(scene_view_image), ImVec2{fixed_width, viewport_panel_size_.y});
    }

    if (!editor_scene_->is_playing()) {
      if (editor_camera.has<CameraComponent>()) {
        editor_camera.enable();

        draw_gizmos();
      }
      transform_gizmos_button_group(start_cursor_pos);
    }

    scene_button_group(start_cursor_pos);
  }

  on_end();
}

void ViewportPanel::set_context(const std::shared_ptr<EditorScene>& scene) {
  OX_CHECK_NULL(scene);

  last_save_scene_path = {};

  this->editor_scene_ = scene;

  set_name(fmt::format("Viewport:{}", scene->get_scene()->scene_name));

  if (!scene->is_playing()) {
    editor_camera = editor_scene_->get_scene()->create_entity("editor_camera", false);
    editor_camera.add<CameraComponent>().add<Hidden>();
  }

  auto& event_system = App::get_event_system();
  event_system.emit<Editor::ViewportSceneLoadEvent>(Editor::ViewportSceneLoadEvent{});
}

void ViewportPanel::on_update() {
  if (!editor_scene_ || !is_viewport_hovered || editor_scene_->get_scene()->is_running() ||
      !editor_camera.has<CameraComponent>()) {
    return;
  }

  const float dt = static_cast<float>(App::get_timestep().get_seconds());

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

  const auto& window = App::get_window();

  auto& input_sys = App::mod<Input>();
  if (input_sys.get_key_pressed(KeyCode::F)) {
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

  if (input_sys.get_mouse_held(MouseCode::ButtonRight) && !is_ortho) {
    const glm::vec2 new_mouse_position = input_sys.get_mouse_position_rel();
    window.set_cursor_override(WindowCursor::Crosshair);

    if (input_sys.get_mouse_moved()) {
      const glm::vec2 change = new_mouse_position * camera_sens;
      final_yaw_pitch.x += change.x;
      final_yaw_pitch.y = glm::clamp(final_yaw_pitch.y - change.y, glm::radians(-89.9f), glm::radians(89.9f));
    }

    const float max_move_speed = camera_speed * (ImGui::IsKeyDown(ImGuiKey_LeftShift) ? 3.0f : 1.0f) * dt;

    if (input_sys.get_key_held(KeyCode::W))
      final_position += cam.forward * max_move_speed;
    else if (input_sys.get_key_held(KeyCode::S))
      final_position -= cam.forward * max_move_speed;
    if (input_sys.get_key_held(KeyCode::D))
      final_position += cam.right * max_move_speed;
    else if (input_sys.get_key_held(KeyCode::A))
      final_position -= cam.right * max_move_speed;

    if (input_sys.get_key_held(KeyCode::Q)) {
      final_position.y -= max_move_speed;
    } else if (input_sys.get_key_held(KeyCode::E)) {
      final_position.y += max_move_speed;
    }
  }
  // Panning
  else if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
    const glm::vec2 new_mouse_position = input_sys.get_mouse_position_rel();
    window.set_cursor_override(WindowCursor::ResizeAll);

    const glm::vec2 change = (new_mouse_position - _locked_mouse_position) * 1.f;

    if (input_sys.get_mouse_moved()) {
      const float max_move_speed = camera_speed * (ImGui::IsKeyDown(ImGuiKey_LeftShift) ? 3.0f : 1.0f) * dt;
      final_position += cam.forward * change.y * max_move_speed;
      final_position += cam.right * change.x * max_move_speed;
    }
  }

  const glm::vec3 damped_position =
    math::smooth_damp(position, final_position, _translation_velocity, _translation_dampening, 1000.0f, dt);
  const glm::vec2 damped_yaw_pitch =
    math::smooth_damp(yaw_pitch, final_yaw_pitch, _rotation_velocity, _rotation_dampening, 1000.0f, dt);

  tc.position = EditorCVar::cvar_camera_smooth.get() ? damped_position : final_position;
  tc.rotation.x = EditorCVar::cvar_camera_smooth.get() ? damped_yaw_pitch.y : final_yaw_pitch.y;
  tc.rotation.y = EditorCVar::cvar_camera_smooth.get() ? damped_yaw_pitch.x : final_yaw_pitch.x;

  cam.zoom = static_cast<float>(EditorCVar::cvar_camera_zoom.get());
}

void ViewportPanel::draw_stats_overlay(bool draw) const {
  if (!performance_overlay_visible || !editor_scene_)
    return;
  auto work_pos = ImVec2(viewport_position_.x, viewport_position_.y);
  auto work_size = ImVec2(viewport_panel_size_.x, viewport_panel_size_.y);
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
  ImGui::SetNextWindowSize(draw ? ImVec2({220.f, 0.f}) : ImVec2(0.f, 0.f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 2.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  auto overlay_id = fmt::format("{}_overlay", get_id());
  if (ImGui::Begin(overlay_id.c_str(), nullptr, window_flags)) {
    ImGui::Text("%.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
    if (draw) {
      ImGui::Text("Scripts in scene: %zu", editor_scene_->get_scene()->get_lua_systems().size());
      const auto transform_entities_count = editor_scene_->get_scene()->world.count<TransformComponent>();
      ImGui::Text("Entities with transforms: %d", transform_entities_count);
      const auto mesh_entities_count = editor_scene_->get_scene()->world.count<MeshComponent>();
      ImGui::Text("Entities with mesh: %d", mesh_entities_count);
      const auto light_entities_count = editor_scene_->get_scene()->world.count<LightComponent>();
      ImGui::Text("Entities with light: %d", light_entities_count);
      const auto sprite_entities_count = editor_scene_->get_scene()->world.count<SpriteComponent>();
      ImGui::Text("Entities with sprite: %d", sprite_entities_count);
      const auto particle_entities_count = editor_scene_->get_scene()->world.count<ParticleSystemComponent>();
      ImGui::Text("Entities with particle: %d", particle_entities_count);
      const auto rigidbody_entities_count = editor_scene_->get_scene()->world.count<RigidBodyComponent>();
      ImGui::Text("Entities with rigidbody: %d", rigidbody_entities_count);
      const auto audio_entities_count = editor_scene_->get_scene()->world.count<AudioSourceComponent>();
      ImGui::Text("Entities with audio: %d", audio_entities_count);
    }
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
}

void ViewportPanel::draw_settings_panel() {
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
    auto& vk_context = App::get_vkcontext();
    ImGui::Text("GPU: %s", vk_context.device_name.c_str());
    ImGui::Text(
      "Swapchain: %dx%d",
      static_cast<u32>(vk_context.swapchain_extent.x),
      static_cast<u32>(vk_context.swapchain_extent.y)
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
        UI::property("Debug View", RendererCVar::cvar_debug_view.get_ptr(), debug_views, ox::count_of(debug_views));
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
        UI::property<float>("Radius", RendererCVar::cvar_vbgtao_radius.get_ptr(), 0.1, 5);
        UI::property<float>("Thickness", RendererCVar::cvar_vbgtao_thickness.get_ptr(), 0.0f, 5.f);
        UI::property<float>("Final Power", RendererCVar::cvar_vbgtao_final_power.get_ptr(), 0, 10);
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

void ViewportPanel::draw_gizmo_settings_panel() {
  if (UI::begin_properties(UI::default_properties_flags, true, 0.3f)) {
    UI::property("Draw grid", (bool*)EditorCVar::cvar_draw_grid.get_ptr());
    UI::property<float>("Grid distance", EditorCVar::cvar_draw_grid_distance.get_ptr(), 10.f, 10000.0f);

    UI::property("Draw Component Gizmos", &draw_component_gizmos_);
    UI::property("Component Gizmos Size", &gizmo_icon_size_);
    UI::property("Entity Highlighting", &draw_entity_highlighting_);
    UI::end_properties();
  }
}

void ViewportPanel::draw_gizmos() {
  auto& editor = App::mod<Editor>();
  auto& editor_context = editor.get_context();
  auto& undo_redo_system = editor.undo_redo_system;

  const auto& cam = editor_camera.get<CameraComponent>();
  auto projection = cam.get_projection_matrix();
  projection[1][1] *= -1;
  glm::mat4 view_proj = projection * cam.get_view_matrix();
  const Frustum& frustum = Camera::get_frustum(cam, cam.position);

  if (draw_component_gizmos_) {
    show_component_gizmo<LightComponent>(
      gizmo_icon_size_,
      "LightComponent",
      viewport_panel_size_.x,
      viewport_panel_size_.y,
      0,
      0,
      view_proj,
      frustum,
      editor_scene_->get_scene().get()
    );
    show_component_gizmo<AudioSourceComponent>(
      gizmo_icon_size_,
      "AudioSourceComponent",
      viewport_panel_size_.x,
      viewport_panel_size_.y,
      0,
      0,
      view_proj,
      frustum,
      editor_scene_->get_scene().get()
    );
    show_component_gizmo<AudioListenerComponent>(
      gizmo_icon_size_,
      "AudioListenerComponent",
      viewport_panel_size_.x,
      viewport_panel_size_.y,
      0,
      0,
      view_proj,
      frustum,
      editor_scene_->get_scene().get()
    );
    show_component_gizmo<CameraComponent>(
      gizmo_icon_size_,
      "CameraComponent",
      viewport_panel_size_.x,
      viewport_panel_size_.y,
      0,
      0,
      view_proj,
      frustum,
      editor_scene_->get_scene().get()
    );
  }

  const flecs::entity selected_entity = editor_context.entity.value_or(flecs::entity::null());

  ImGuizmo::BeginFrame();

  auto& input_sys = App::mod<Input>();
  if (input_sys.get_key_held(KeyCode::LeftControl)) {
    if (input_sys.get_key_pressed(KeyCode::Q)) {
      if (!ImGuizmo::IsUsing())
        gizmo_type_ = -1;
    }
    if (input_sys.get_key_pressed(KeyCode::W)) {
      if (!ImGuizmo::IsUsing())
        gizmo_type_ = ImGuizmo::OPERATION::TRANSLATE;
    }
    if (input_sys.get_key_pressed(KeyCode::E)) {
      if (!ImGuizmo::IsUsing())
        gizmo_type_ = ImGuizmo::OPERATION::ROTATE;
    }
    if (input_sys.get_key_pressed(KeyCode::R)) {
      if (!ImGuizmo::IsUsing())
        gizmo_type_ = ImGuizmo::OPERATION::SCALE;
    }
  }

  if (selected_entity == flecs::entity::null() || !editor_camera.has<CameraComponent>() || gizmo_type_ == -1)
    return;

  if (auto* tc = selected_entity.try_get_mut<TransformComponent>()) {
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(
      viewport_bounds_[0].x,
      viewport_bounds_[0].y,
      viewport_bounds_[1].x - viewport_bounds_[0].x,
      viewport_bounds_[1].y - viewport_bounds_[0].y
    );

    auto camera_projection = cam.get_projection_matrix();
    camera_projection[1][1] *= -1;

    const glm::mat4& camera_view = cam.get_view_matrix();

    glm::mat4 transform = Scene::get_world_transform(selected_entity);

    // Snapping
    const bool snap = input_sys.get_key_held(KeyCode::LeftControl);
    float snap_value = 0.5f; // Snap to 0.5m for translation/scale
    // Snap to 45 degrees for rotation
    if (gizmo_type_ == ImGuizmo::OPERATION::ROTATE)
      snap_value = 45.0f;

    const float snap_values[3] = {snap_value, snap_value, snap_value};

    const auto is_ortho = cam.projection == CameraComponent::Projection::Orthographic;
    ImGuizmo::SetOrthographic(is_ortho);
    if (gizmo_mode_ == ImGuizmo::OPERATION::TRANSLATE && is_ortho)
      gizmo_mode_ = ImGuizmo::OPERATION::TRANSLATE_X | ImGuizmo::OPERATION::TRANSLATE_Y;

    ImGuizmo::Manipulate(
      value_ptr(camera_view),
      value_ptr(camera_projection),
      static_cast<ImGuizmo::OPERATION>(gizmo_type_),
      static_cast<ImGuizmo::MODE>(gizmo_mode_),
      value_ptr(transform),
      nullptr,
      snap ? snap_values : nullptr
    );

    if (ImGuizmo::IsUsing()) {
      const flecs::entity parent = selected_entity.parent();
      const glm::mat4& parent_world_transform = parent != flecs::entity::null() ? Scene::get_world_transform(parent)
                                                                                : glm::mat4(1.0f);
      glm::vec3 translation, rotation, scale;
      if (math::decompose_transform(inverse(parent_world_transform) * transform, translation, rotation, scale)) {
        tc->position = translation;
        const glm::vec3 delta_rotation = rotation - tc->rotation;
        tc->rotation += delta_rotation;
        tc->scale = scale;

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

auto ViewportPanel::mouse_picking_stages(RendererInstance* renderer_instance, glm::uvec2 picking_texel) -> void {
  renderer_instance->add_stage_after(
    RenderStage::VisBufferEncode,
    "mouse_picking",
    [picking_texel, viewport_hovered = is_viewport_hovered, using_gizmo = ImGuizmo::IsOver(), s = editor_scene_](
      RenderStageContext& ctx
    ) {
      auto depth_attachment = ctx.get_image_resource("depth_attachment");
      auto visbuffer = ctx.get_image_resource("visbuffer_attachment");
      auto meshlet_instances = ctx.get_buffer_resource("meshlet_instances_buffer");
      auto mesh_instances = ctx.get_buffer_resource("mesh_instances_buffer");

      auto readback_buffer = ctx.vk_context.alloc_transient_buffer(vuk::MemoryUsage::eGPUtoCPU, sizeof(u32));

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

      auto read_pass = vuk::make_pass(
        "mouse_picking_read_pass",
        [s, viewport_hovered, using_gizmo](
          vuk::CommandBuffer& cmd_list,
          VUK_BA(vuk::eHostRead) buffer,
          VUK_IA(vuk::eComputeSampled) visbuffer_
        ) {
          u32 transform_index = *reinterpret_cast<u32*>(buffer.ptr->mapped_ptr);

          auto& editor = App::mod<Editor>();

          if (!using_gizmo && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && viewport_hovered) {
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

          return std::make_tuple(buffer, visbuffer_);
        }
      );

      std::tie(readback_buffer, visbuffer) = read_pass(std::move(readback_buffer), std::move(visbuffer));

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

  if (!draw_entity_highlighting_) {
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

auto ViewportPanel::grid_stage(RendererInstance* renderer_instance) -> void {
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

void ViewportPanel::transform_gizmos_button_group(ImVec2 start_cursor_pos) {
  const float frame_height = 1.3f * ImGui::GetFrameHeight();
  const ImVec2 frame_padding = ImGui::GetStyle().FramePadding;
  const ImVec2 button_size = {frame_height, frame_height};
  constexpr float button_count = 8.0f;
  const ImVec2 gizmo_position = {viewport_bounds_[0].x + gizmo_position_.x, viewport_bounds_[0].y + gizmo_position_.y};
  const ImRect bb(
    gizmo_position.x,
    gizmo_position.y,
    gizmo_position.x + button_size.x + 8,
    gizmo_position.y + (button_size.y + 2) * (button_count + 0.5f)
  );
  ImVec4 frame_color = ImGui::GetStyleColorVec4(ImGuiCol_Tab);
  frame_color.w = 0.5f;
  ImGui::RenderFrame(bb.Min, bb.Max, ImGui::GetColorU32(frame_color), false, ImGui::GetStyle().FrameRounding);
  const glm::vec2 temp_gizmo_position = gizmo_position_;

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
      gizmo_position_.x += mouse_pos.x - last_mouse_position.x;
      gizmo_position_.y += mouse_pos.y - last_mouse_position.y;
    }
    last_mouse_position = mouse_pos;

    constexpr float alpha = 0.6f;
    if (UI::toggle_button(ICON_MDI_AXIS_ARROW, gizmo_type_ == ImGuizmo::TRANSLATE, button_size, alpha, alpha))
      gizmo_type_ = ImGuizmo::TRANSLATE;
    if (UI::toggle_button(ICON_MDI_ROTATE_3D, gizmo_type_ == ImGuizmo::ROTATE, button_size, alpha, alpha))
      gizmo_type_ = ImGuizmo::ROTATE;
    if (UI::toggle_button(ICON_MDI_ARROW_EXPAND, gizmo_type_ == ImGuizmo::SCALE, button_size, alpha, alpha))
      gizmo_type_ = ImGuizmo::SCALE;
    if (UI::toggle_button(ICON_MDI_VECTOR_SQUARE, gizmo_type_ == ImGuizmo::BOUNDS, button_size, alpha, alpha))
      gizmo_type_ = ImGuizmo::BOUNDS;
    if (UI::toggle_button(ICON_MDI_ARROW_EXPAND_ALL, gizmo_type_ == ImGuizmo::UNIVERSAL, button_size, alpha, alpha))
      gizmo_type_ = ImGuizmo::UNIVERSAL;
    if (UI::toggle_button(
          gizmo_mode_ == ImGuizmo::WORLD ? ICON_MDI_EARTH : ICON_MDI_EARTH_OFF,
          gizmo_mode_ == ImGuizmo::WORLD,
          button_size,
          alpha,
          alpha
        ))
      gizmo_mode_ = gizmo_mode_ == ImGuizmo::LOCAL ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
    if (UI::toggle_button(ICON_MDI_GRID, EditorCVar::cvar_draw_grid.get(), button_size, alpha, alpha))
      EditorCVar::cvar_draw_grid.toggle();

    if (editor_camera.has<CameraComponent>()) {
      auto& cam = editor_camera.get_mut<CameraComponent>();
      UI::push_id();
      if (UI::toggle_button(
            ICON_MDI_CAMERA,
            cam.projection == CameraComponent::Projection::Orthographic,
            button_size,
            alpha,
            alpha
          ))
        cam.projection = cam.projection == CameraComponent::Projection::Orthographic
                           ? CameraComponent::Projection::Perspective
                           : CameraComponent::Projection::Orthographic;
    }
    UI::pop_id();

    ImGui::PopStyleVar(2);
  }
  ImGui::EndGroup();
}

void ViewportPanel::scene_button_group(ImVec2 start_cursor_pos) {
  constexpr float button_count = 2.0f;
  constexpr float y_pad = 3.0f;
  const ImVec2 button_size = {35.f, 25.f};
  const ImVec2 group_size = {button_size.x * button_count, button_size.y + y_pad};

  ImGui::SetCursorPos({viewport_size_.x * 0.5f - (group_size.x * 0.5f), start_cursor_pos.y + y_pad});
  ImGui::BeginGroup();
  {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {1, 1});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 1.0f);

    auto& event_system = App::get_event_system();

    auto is_scene_playing = editor_scene_->is_playing();

    ImGui::BeginDisabled(is_scene_playing);
    if (ImGui::Button(ICON_MDI_PLAY, button_size)) {
      event_system.emit<Editor::ScenePlayEvent>(Editor::ScenePlayEvent(editor_scene_->get_id()));
    }
    ImGui::EndDisabled();

    ImGui::SameLine();

    ImGui::BeginDisabled(!is_scene_playing);
    if (ImGui::Button(ICON_MDI_STOP, button_size)) {
      event_system.emit<Editor::SceneStopEvent>(Editor::SceneStopEvent(editor_scene_->get_id()));
    }
    ImGui::EndDisabled();

    ImGui::PopStyleVar(3);
  }
  ImGui::EndGroup();
}

} // namespace ox
