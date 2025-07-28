#include "Scripting/LuaUIBindings.hpp"

#include <sol/state.hpp>

#include "Scripting/LuaImGuiBindings.hpp"

namespace ox {
auto UIBinding::bind(sol::state* state) -> void {
  LuaImGuiBindings::init(state);

  // TODO: the rest of the api
}
} // namespace ox
