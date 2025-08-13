#pragma once

namespace sol {
class state;
}

namespace ox::LuaBindings {
void bind_ui(sol::state* state);
}
