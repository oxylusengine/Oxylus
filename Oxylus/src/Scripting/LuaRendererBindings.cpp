#include "Scripting/LuaRendererBindings.hpp"

#include <sol/state.hpp>

#include "Scripting/LuaHelpers.hpp"
#include "Render/Window.hpp"

namespace ox::LuaBindings {
void bind_renderer(sol::state* state) {
  auto window_table = state->create_table("Window");
  SET_TYPE_FUNCTION(window_table, Window, get_width);
  SET_TYPE_FUNCTION(window_table, Window, get_height);
}
} // namespace ox::LuaBindings
