#pragma once

namespace sol {
class state;
}

namespace ox::LuaBindings {
void bind_audio(sol::state* state);
}
