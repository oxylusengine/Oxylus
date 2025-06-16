#pragma once

namespace sol {
class state;
}

namespace ox::LuaBindings {
void bind_math(const std::shared_ptr<sol::state>& state);
}
