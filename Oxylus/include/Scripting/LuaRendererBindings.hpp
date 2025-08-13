#pragma once

namespace sol {
class state;
}

namespace ox::LuaBindings {
void bind_renderer(sol::state* state);
}
