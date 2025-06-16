#pragma once

namespace sol {
class state;
}

namespace ox::LuaBindings {
void bind_debug_renderer(const std::shared_ptr<sol::state>& state);
}
