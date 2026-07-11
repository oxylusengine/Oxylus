#include "Scripting/LuaUIBindings.hpp"

#include <sol/state.hpp>

#include "Scripting/LuaImGuiBindings.hpp"
#include "UI/NetStatsViewer.hpp"
#include "UI/UI.hpp"

namespace ox {
auto UIBinding::bind(sol::state* state) -> void {
  LuaImGuiBindings::init(state);

  auto ui = state->create_named_table("OxUI");
  ui.set_function("center_next_window", &UI::center_next_window);

  ui.set_function("draw_network_stats", &NetStatsViewer::draw_network_stats);
}
} // namespace ox
