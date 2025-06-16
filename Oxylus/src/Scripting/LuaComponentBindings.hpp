#pragma once

namespace sol {
class state;
}

namespace ox::LuaBindings {
void bind_components(const std::shared_ptr<sol::state>& state);

void bind_light_component(const std::shared_ptr<sol::state>& state);
void bind_mesh_component(const std::shared_ptr<sol::state>& state);
void bind_camera_component(const std::shared_ptr<sol::state>& state);
} // namespace ox::LuaBindings
