#pragma once

namespace sol {
class state;
}

namespace ox::LuaBindings {
void bind_application(sol::state* state);
}
