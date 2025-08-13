#pragma once

namespace sol {
class state;
}

namespace ox::LuaBindings {
void bind_debug_renderer(sol::state* state);
}
