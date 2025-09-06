#include "SceneHierarchyPanel.hpp"

#include <glm/trigonometric.hpp>
#include <icons/IconsMaterialDesignIcons.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include "EditorLayer.hpp"
#include "Render/DebugRenderer.hpp"

namespace ox {
SceneHierarchyPanel::SceneHierarchyPanel() : EditorPanel("Scene Hierarchy", ICON_MDI_VIEW_LIST, true) {
  viewer.add_icon = ICON_MDI_PLUS;
  viewer.search_icon = ICON_MDI_MAGNIFY;
  viewer.entity_icon = ICON_MDI_CUBE_OUTLINE;
  viewer.visibility_icon_on = ICON_MDI_EYE_OUTLINE;
  viewer.visibility_icon_off = ICON_MDI_EYE_OFF_OUTLINE;
  viewer.script_icon = ICON_MDI_SCRIPT;

  viewer.asset_manager_viewer.filter_icon = ICON_MDI_FILTER;
  viewer.asset_manager_viewer.search_icon = ICON_MDI_MAGNIFY;

  viewer.on_selected_entity_callback([](flecs::entity e) {
    auto& context = EditorLayer::get()->get_context();

    context.reset();
    context.type = EditorContext::Type::Entity;
    context.entity = e;
  });

  viewer.on_selected_entity_reset_callback([]() {
    auto& context = EditorLayer::get()->get_context();
    context.reset();
  });
}

auto SceneHierarchyPanel::on_update() -> void {
  auto* editor_layer = EditorLayer::get();
  auto& editor_context = editor_layer->get_context();
  auto& undo_redo_system = editor_layer->undo_redo_system;

  if (editor_context.type == EditorContext::Type::Entity) {
    if (editor_context.entity.has_value())
      viewer.selected_entity_.set(editor_context.entity.value());
  }

  if (viewer.selected_entity_.get() != flecs::entity::null()) {
    if (auto* cam = viewer.selected_entity_.get().try_get<CameraComponent>()) {
      const auto proj = cam->get_projection_matrix() * cam->get_view_matrix();
      DebugRenderer::draw_frustum(proj, glm::vec4(0, 1, 0, 1), cam->near_clip, cam->far_clip);
    }
    if (auto* light = viewer.selected_entity_.get().try_get<LightComponent>()) {
      const glm::vec3 world_pos = Scene::get_world_position(viewer.selected_entity_.get());
      if (light->type == LightComponent::Point) {
        DebugRenderer::draw_sphere(light->radius, world_pos, glm::vec4(0, 1.f, 0.f, 1.f));
      } else if (light->type == LightComponent::Spot) {

      }
    }

    if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) && ImGui::IsKeyPressed(ImGuiKey_D)) {
      auto clone_entity = [](flecs::entity entity) -> flecs::entity {
        std::string clone_name = entity.name().c_str();
        while (entity.world().lookup(clone_name.data())) {
          clone_name = fmt::format("{}_clone", clone_name);
        }
        auto cloned_entity = entity.clone(true);
        return cloned_entity.set_name(clone_name.data());
      };

      viewer.selected_entity_.set(clone_entity(viewer.selected_entity_.get()));
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) &&
        (viewer.table_hovered_ || editor_layer->viewport_panels[0]->is_viewport_hovered)) {
      viewer.deleted_entity_ = viewer.selected_entity_.get();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F2)) {
      viewer.renaming_entity_ = viewer.selected_entity_.get();
    }
  }

  if (viewer.selected_script_ && ImGui::IsKeyPressed(ImGuiKey_Delete) &&
      (viewer.table_hovered_ || viewer.table_hovered_scripts)) {
    viewer.get_scene()->remove_lua_system(*viewer.selected_script_);
  }

  if (viewer.deleted_entity_) {
    auto command_id = fmt::format("delete entity {}", viewer.deleted_entity_.name().c_str());
    undo_redo_system->execute_command<EntityDeleteCommand>(viewer.get_scene(), viewer.deleted_entity_, "", command_id);
    viewer.selected_entity_.reset();
  }
}

auto SceneHierarchyPanel::on_render(vuk::Extent3D extent, vuk::Format format) -> void {
  ZoneScoped;

  viewer.render(_id.c_str(), &visible);
}
} // namespace ox
