#pragma once

namespace sol {
class state;
}

namespace ox::LuaBindings {
void bind_application(const std::shared_ptr<sol::state>& state);

}
