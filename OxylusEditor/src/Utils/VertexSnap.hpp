#pragma once

#include <flecs.h>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "Core/Types.hpp"

namespace ox {
class Scene;
struct CameraComponent;

// hold V with the translate gizmo to drag the selection by its vertex nearest the cursor onto the nearest
// mesh or sprite vertex
struct VertexSnap {
  struct Context {
    Scene* scene = nullptr;
    const CameraComponent* camera = nullptr;
    flecs::entity selected_entity = {};
    glm::vec2 rect_min = {};
    glm::vec2 rect_max = {};
    bool is_translating = false;
    bool is_gizmo_dragging = false;
    bool is_hovered = false;
    bool can_begin_drag = false;
  };

  f32 radius = 24.0f;
  // read by picking a frame later, so the click that starts a vertex drag doesn't change the selection
  bool is_active = false;
  bool is_dragging = false;
  // grabbed vertex in the selected entity's local frame, so it rides along while the entity moves
  glm::vec3 pivot_local = {};

  // true while it owns the selection and the transform gizmo should stay hidden
  auto update(this VertexSnap& self, const Context& context) -> bool;
};
} // namespace ox
