#pragma once

namespace sol {
class state;
}

namespace ox::LuaBindings {
void bind_input(sol::state* state);
}
