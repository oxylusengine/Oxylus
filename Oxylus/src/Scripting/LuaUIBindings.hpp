#pragma once

namespace sol {
class state;
}

namespace ox::LuaBindings {
void bind_ui(const std::shared_ptr<sol::state>& state);
}
