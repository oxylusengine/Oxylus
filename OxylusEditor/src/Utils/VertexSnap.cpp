#include "VertexSnap.hpp"

#include <array>
#include <imgui.h>
#include <limits>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Core/Input.hpp"
#include "Editor.hpp"
#include "Render/BoundingVolume.hpp"
#include "Scene/Components.hpp"
#include "Scene/Scene.hpp"
#include "UI/UI.hpp"
#include "Utils/Command.hpp"

namespace ox {
struct ViewportProjection {
  glm::mat4 view_proj = {};
  glm::mat4 inverse_view_proj = {};
  glm::vec2 rect_min = {};
  glm::vec2 rect_size = {};
  glm::vec3 camera_position = {};
  glm::vec3 camera_forward = {};
};

// xy in screen pixels, z is the ndc depth
static auto project_to_viewport(const ViewportProjection& projection, const glm::vec3& world_position)
  -> option<glm::vec3> {
  const auto clip = projection.view_proj * glm::vec4(world_position, 1.0f);
  if (clip.w <= 0.0f) {
    return nullopt;
  }

  const auto ndc = glm::vec3(clip) / clip.w;
  return glm::vec3(
    projection.rect_min.x + (ndc.x * 0.5f + 0.5f) * projection.rect_size.x,
    projection.rect_min.y + (0.5f - ndc.y * 0.5f) * projection.rect_size.y,
    ndc.z
  );
}

static auto unproject_from_viewport(
  const ViewportProjection& projection, const glm::vec2 screen_position, f32 ndc_depth
) -> glm::vec3 {
  const auto ndc = glm::vec2(
    (screen_position.x - projection.rect_min.x) / projection.rect_size.x * 2.0f - 1.0f,
    1.0f - (screen_position.y - projection.rect_min.y) / projection.rect_size.y * 2.0f
  );
  const auto world_position = projection.inverse_view_proj * glm::vec4(ndc, ndc_depth, 1.0f);
  return glm::vec3(world_position) / world_position.w;
}

// keeps vertex walks to meshes whose projected box lands near the cursor
static auto is_box_near_cursor(
  const ViewportProjection& projection, const AABB& world_box, const glm::vec2 cursor, f32 radius
) -> bool {
  auto rect_min = glm::vec2(std::numeric_limits<f32>::max());
  auto rect_max = glm::vec2(std::numeric_limits<f32>::lowest());
  for (auto corner_index = 0_u32; corner_index < 8; corner_index++) {
    const auto corner = glm::vec3(
      (corner_index & 1) ? world_box.max.x : world_box.min.x,
      (corner_index & 2) ? world_box.max.y : world_box.min.y,
      (corner_index & 4) ? world_box.max.z : world_box.min.z
    );
    const auto screen = project_to_viewport(projection, corner);
    // a box crossing the camera plane has no trustworthy screen rect
    if (!screen) {
      return true;
    }

    rect_min = glm::min(rect_min, glm::vec2(*screen));
    rect_max = glm::max(rect_max, glm::vec2(*screen));
  }

  return cursor.x >= rect_min.x - radius && cursor.x <= rect_max.x + radius && cursor.y >= rect_min.y - radius &&
         cursor.y <= rect_max.y + radius;
}

// collision positions are the only CPU-side copy of a mesh's vertices, skinned meshes give their bind pose
template <typename Func>
static auto for_each_mesh_vertex(const MeshComponent& mesh, const glm::mat4& world, Func&& func) -> void {
  auto model = App::mod<AssetManager>().get_model(mesh.model_uuid);
  if (!model || mesh.mesh_index >= model->collision_meshes.size() || !model->is_mesh_ready(mesh.mesh_index)) {
    return;
  }

  for (const auto& position : model->collision_meshes[mesh.mesh_index].positions) {
    func(glm::vec3(world * glm::vec4(position, 1.0f)));
  }
}

template <typename Func>
static auto for_each_sprite_vertex(const glm::mat4& world, Func&& func) -> void {
  constexpr auto points = std::array{
    glm::vec3(-0.5f, -0.5f, 0.0f),
    glm::vec3(0.5f, -0.5f, 0.0f),
    glm::vec3(0.5f, 0.5f, 0.0f),
    glm::vec3(-0.5f, 0.5f, 0.0f),
    glm::vec3(0.0f, 0.0f, 0.0f),
  };
  for (const auto& point : points) {
    func(glm::vec3(world * glm::vec4(point, 1.0f)));
  }
}

template <typename Func>
static auto for_each_in_subtree(const flecs::entity root, Func& func) -> void {
  func(root);
  root.children([&func](const flecs::entity child) { for_each_in_subtree(child, func); });
}

static auto is_in_subtree(const flecs::entity entity, const flecs::entity root) -> bool {
  for (auto ancestor = entity; ancestor != flecs::entity::null(); ancestor = ancestor.parent()) {
    if (ancestor == root) {
      return true;
    }
  }

  return false;
}

struct VertexSnapHit {
  glm::vec3 world_position = {};
  f32 distance_sq = std::numeric_limits<f32>::max();
  f32 depth = std::numeric_limits<f32>::max();
};

static auto consider_snap_vertex(
  const ViewportProjection& projection, const glm::vec2 cursor, const glm::vec3& world_position, VertexSnapHit& hit
) -> void {
  const auto screen = project_to_viewport(projection, world_position);
  if (!screen) {
    return;
  }

  const auto offset = glm::vec2(*screen) - cursor;
  const auto distance_sq = glm::dot(offset, offset);
  const auto depth = glm::dot(world_position - projection.camera_position, projection.camera_forward);
  // vertices stacked under the same pixel resolve to the one nearest the camera
  const auto is_tied = glm::abs(distance_sq - hit.distance_sq) < 1.0f;
  if (is_tied ? depth < hit.depth : distance_sq < hit.distance_sq) {
    hit = {.world_position = world_position, .distance_sq = distance_sq, .depth = depth};
  }
}

static auto draw_vertex_marker(const ViewportProjection& projection, const glm::vec3& world_position, ImU32 color)
  -> void {
  const auto screen = project_to_viewport(projection, world_position);
  if (!screen) {
    return;
  }

  const auto half_size = UI::scale(5.0f);
  auto* draw_list = ImGui::GetWindowDrawList();
  draw_list->AddRectFilled(
    {screen->x - half_size, screen->y - half_size},
    {screen->x + half_size, screen->y + half_size},
    color
  );
  draw_list->AddRect(
    {screen->x - half_size, screen->y - half_size},
    {screen->x + half_size, screen->y + half_size},
    IM_COL32(0, 0, 0, 255)
  );
}

auto VertexSnap::update(this VertexSnap& self, const Context& context) -> bool {
  ZoneScoped;

  auto* tc = context.selected_entity != flecs::entity::null()
               ? context.selected_entity.try_get_mut<TransformComponent>()
               : nullptr;
  const auto is_key_held = App::mod<Input>().get_key_held(ScanCode::V) && !ImGui::GetIO().WantTextInput;
  self.is_active = tc && context.is_translating && is_key_held && !context.is_gizmo_dragging &&
                   (self.is_dragging || context.is_hovered);
  if (!self.is_active) {
    self.is_dragging = false;
    return false;
  }

  const auto& camera = *context.camera;
  auto camera_projection = camera.get_projection_matrix();
  camera_projection[1][1] *= -1;
  const auto view_proj = camera_projection * camera.get_view_matrix();
  const auto inverse_view = glm::inverse(camera.get_view_matrix());

  const auto projection = ViewportProjection{
    .view_proj = view_proj,
    .inverse_view_proj = glm::inverse(view_proj),
    .rect_min = context.rect_min,
    .rect_size = context.rect_max - context.rect_min,
    .camera_position = glm::vec3(inverse_view[3]),
    .camera_forward = -glm::vec3(inverse_view[2]),
  };
  if (projection.rect_size.x <= 0.0f || projection.rect_size.y <= 0.0f) {
    return true;
  }

  const auto mouse_position = ImGui::GetMousePos();
  const auto cursor = glm::vec2(mouse_position.x, mouse_position.y);
  const auto selected_entity = context.selected_entity;
  const auto selected_world = Scene::get_world_transform(selected_entity);

  if (!self.is_dragging) {
    auto source = VertexSnapHit{};
    auto visit_source = [&](const flecs::entity entity) {
      if (entity.has<Hidden>() || !entity.has<TransformComponent>()) {
        return;
      }

      const auto* mesh = entity.try_get<MeshComponent>();
      const auto has_sprite = entity.has<SpriteComponent>();
      if (!mesh && !has_sprite) {
        return;
      }

      const auto world = Scene::get_world_transform(entity);
      const auto consider = [&](const glm::vec3& world_position) {
        consider_snap_vertex(projection, cursor, world_position, source);
      };
      if (mesh) {
        for_each_mesh_vertex(*mesh, world, consider);
      }

      if (has_sprite) {
        for_each_sprite_vertex(world, consider);
      }
    };
    for_each_in_subtree(selected_entity, visit_source);

    const auto is_source_found = source.distance_sq < std::numeric_limits<f32>::max();
    const auto pivot_world = is_source_found ? source.world_position : glm::vec3(selected_world[3]);
    self.pivot_local = glm::vec3(glm::inverse(selected_world) * glm::vec4(pivot_world, 1.0f));
    draw_vertex_marker(projection, pivot_world, IM_COL32(255, 200, 40, 255));

    if (context.can_begin_drag && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      self.is_dragging = true;
    }

    return true;
  }

  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    self.is_dragging = false;
    return true;
  }

  const auto pivot_world = glm::vec3(selected_world * glm::vec4(self.pivot_local, 1.0f));
  const auto pivot_screen = project_to_viewport(projection, pivot_world);
  if (!pivot_screen) {
    return true;
  }

  const auto radius = UI::scale(self.radius);
  auto target = VertexSnapHit{};
  const auto consider_target = [&](const glm::vec3& world_position) {
    consider_snap_vertex(projection, cursor, world_position, target);
  };

  context.scene->world.query_builder<const MeshComponent>().build().each(
    [&](const flecs::entity entity, const MeshComponent& mesh) {
      if (entity.has<Hidden>() || !entity.has<TransformComponent>() || is_in_subtree(entity, selected_entity)) {
        return;
      }

      const auto world = Scene::get_world_transform(entity);
      const auto has_bounds = mesh.baked_aabb.min != mesh.baked_aabb.max;
      if (has_bounds && !is_box_near_cursor(projection, mesh.baked_aabb.get_transformed(world), cursor, radius)) {
        return;
      }

      for_each_mesh_vertex(mesh, world, consider_target);
    }
  );
  context.scene->world.query_builder<const SpriteComponent>().build().each(
    [&](const flecs::entity entity, const SpriteComponent&) {
      if (entity.has<Hidden>() || !entity.has<TransformComponent>() || is_in_subtree(entity, selected_entity)) {
        return;
      }

      for_each_sprite_vertex(Scene::get_world_transform(entity), consider_target);
    }
  );

  // with nothing in reach the vertex follows the cursor across the plane it sits on, facing the camera
  const auto is_snapped = target.distance_sq <= radius * radius;
  const auto destination = is_snapped ? target.world_position
                                      : unproject_from_viewport(projection, cursor, pivot_screen->z);

  const auto delta_world = destination - pivot_world;
  if (glm::dot(delta_world, delta_world) > 0.0f) {
    const auto parent = selected_entity.parent();
    const auto parent_world = parent != flecs::entity::null() ? Scene::get_world_transform(parent) : glm::mat4(1.0f);

    auto moved_tc = *tc;
    moved_tc.position += glm::vec3(glm::inverse(parent_world) * glm::vec4(delta_world, 0.0f));
    App::mod<Editor>().undo_redo_system->execute_command<ComponentChangeCommand<TransformComponent>>(
      selected_entity,
      tc,
      *tc,
      moved_tc,
      "vertex snap"
    );
  }

  draw_vertex_marker(projection, destination, is_snapped ? IM_COL32(80, 230, 110, 255) : IM_COL32(255, 200, 40, 255));

  return true;
}
} // namespace ox
