#pragma once

namespace sol {
class state;
}

namespace ox::LuaBindings {
void bind_asset_manager(const std::shared_ptr<sol::state>& state);
}
