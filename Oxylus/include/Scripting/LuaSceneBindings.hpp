#pragma once

namespace sol {
class state;
}

namespace ox::LuaBindings {
void bind_scene(sol::state* state);
}
