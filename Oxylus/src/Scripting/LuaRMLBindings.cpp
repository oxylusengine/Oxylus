#include "Scripting/LuaRMLBindings.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Lua.h>
#include <sol/state.hpp>

namespace ox {
auto RMLBinding::bind(sol::state* state) -> void {
  ZoneScoped;

  Rml::Lua::Initialise(state->lua_state());
}
} // namespace ox
