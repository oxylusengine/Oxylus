#include "Scripting/LuaRendererBindings.hpp"

#include <sol/state.hpp>

#include "Render/RendererInstance.hpp"
#include "Render/Window.hpp"
#include "Scripting/LuaHelpers.hpp"

namespace ox {
auto RendererBinding::bind(sol::state* state) -> void {
  auto window_table = state->create_table("Window");
  SET_TYPE_FUNCTION(window_table, Window, get_size_in_pixels);
  SET_TYPE_FUNCTION(window_table, Window, get_logical_width);
  SET_TYPE_FUNCTION(window_table, Window, get_logical_height);

  auto renderer_instance = state->new_usertype<RendererInstance>(
      "RendererInstance", "get_viewport_offset", &RendererInstance::get_viewport_offset);
}
} // namespace ox
