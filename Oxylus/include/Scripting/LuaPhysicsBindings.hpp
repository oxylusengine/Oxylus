#pragma once

namespace sol {
class state;
}

namespace ox::LuaBindings {
void bind_physics(sol::state* state);
}
