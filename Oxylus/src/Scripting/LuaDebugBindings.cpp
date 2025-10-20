#include "Scripting/LuaDebugBindings.hpp"

#include <sol/state.hpp>

#include "Physics/RayCast.hpp"
#include "Render/DebugRenderer.hpp"
#include "Scripting/LuaHelpers.hpp"

namespace ox {
auto DebugBinding::bind(sol::state* state) -> void {
  auto debug_table = state->new_usertype<DebugRenderer>("DebugRenderer");
  debug_table.set_function("draw_point", [](DebugRenderer& dr, const glm::vec3& point, glm::vec3 color) -> void {
    dr.draw_point(point, 1.0f, glm::vec4(color, 1.0f));
  });
  debug_table.set_function(
    "draw_line",
    [](DebugRenderer& dr, const glm::vec3& start, const glm::vec3& end, const glm::vec3& color = glm::vec3(1)) -> void {
      dr.draw_line(start, end, 1.0f, glm::vec4(color, 1.0f));
    }
  );
  debug_table.set_function(
    "draw_ray",
    [](DebugRenderer& dr, const RayCast& ray, const glm::vec3& color = glm::vec3(1)) -> void {
      dr.draw_line(ray.get_origin(), ray.get_direction(), 1.0f, glm::vec4(color, 1.0f));
    }
  );
  debug_table.set_function(
    "draw_aabb",
    [](DebugRenderer& dr, const AABB& aabb, const glm::vec3& color, const bool depth_tested) -> void {
      dr.draw_aabb(aabb, glm::vec4(color, 1.0f), false, 1.0f, depth_tested);
    }
  );
}
} // namespace ox
