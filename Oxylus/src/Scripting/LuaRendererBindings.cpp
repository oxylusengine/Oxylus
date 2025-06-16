#include "LuaRendererBindings.hpp"

#include <sol/state.hpp>

#include "LuaHelpers.hpp"
#include "Render/Window.hpp"

namespace ox::LuaBindings {
void bind_renderer(const std::shared_ptr<sol::state>& state) {
  auto window_table = state->create_table("Window");
  SET_TYPE_FUNCTION(window_table, Window, get_width);
  SET_TYPE_FUNCTION(window_table, Window, get_height);
}
} // namespace ox::LuaBindings
