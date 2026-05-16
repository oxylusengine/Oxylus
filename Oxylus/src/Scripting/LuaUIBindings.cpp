#include "Scripting/LuaUIBindings.hpp"

#include <sol/state.hpp>

#include "Scripting/LuaImGuiBindings.hpp"
#include "UI/UI.hpp"

namespace ox {
auto UIBinding::bind(sol::state* state) -> void {
  LuaImGuiBindings::init(state);

  auto ui = state->create_table("UI");
  ui.set_function("center_next_window", &UI::center_next_window);
}
} // namespace ox
