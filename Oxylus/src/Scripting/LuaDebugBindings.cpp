#include "Scripting/LuaDebugBindings.hpp"

#include <sol/state.hpp>

#include "Scripting/LuaHelpers.hpp"
#include "Physics/RayCast.hpp"
#include "Render/DebugRenderer.hpp"

namespace ox::LuaBindings {
void bind_debug_renderer(sol::state* state) {
  auto debug_table = state->create_table("Debug");
  debug_table.set_function("draw_point", [](const glm::vec3& point, glm::vec3 color) -> void {
    DebugRenderer::draw_point(point, 1.0f, glm::vec4(color, 1.0f));
  });
  debug_table.set_function(
      "draw_line", [](const glm::vec3& start, const glm::vec3& end, const glm::vec3& color = glm::vec3(1)) -> void {
    DebugRenderer::draw_line(start, end, 1.0f, glm::vec4(color, 1.0f));
  });
  debug_table.set_function("draw_ray", [](const RayCast& ray, const glm::vec3& color = glm::vec3(1)) -> void {
    DebugRenderer::draw_line(ray.get_origin(), ray.get_direction(), 1.0f, glm::vec4(color, 1.0f));
  });
  debug_table.set_function("draw_aabb", [](const AABB& aabb, const glm::vec3& color, const bool depth_tested) -> void {
    DebugRenderer::draw_aabb(aabb, glm::vec4(color, 1.0f), false, 1.0f, depth_tested);
  });
}
} // namespace ox::LuaBindings
