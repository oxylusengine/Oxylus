#pragma once

namespace sol {
class state;
}

namespace ox::LuaBindings {
void bind_math(sol::state* state);
}
