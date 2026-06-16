#include "Scripting/LuaRMLBindings.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Lua.h>
#include <sol/state.hpp>

namespace ox {
auto RMLBinding::bind(sol::state* state) -> void {
  ZoneScoped;

  Rml::Lua::Initialise(state->lua_state());

  sol::table rml_extensions = state->create_named_table("rmlui_ext");
  rml_extensions.set_function("ClearStyleCache", []() { Rml::Factory::ClearStyleSheetCache(); });
  rml_extensions.set_function("ClearTemplateCache", []() { Rml::Factory::ClearTemplateCache(); });
}
} // namespace ox
