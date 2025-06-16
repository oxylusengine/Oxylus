#pragma once

namespace sol {
class state;
}

namespace ox::LuaBindings {
void bind_input(const std::shared_ptr<sol::state>& state);
}
