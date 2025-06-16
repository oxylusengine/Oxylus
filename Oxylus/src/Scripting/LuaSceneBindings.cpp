#include "LuaSceneBindings.hpp"

#include <sol/state.hpp>
#include <sol/variadic_args.hpp>

#include "LuaHelpers.hpp"
#include "Scene/Scene.hpp"

namespace ox::LuaBindings {

void bind_scene(const std::shared_ptr<sol::state>& state) {
  ZoneScoped;
  sol::usertype<Scene> scene_type = state->new_usertype<Scene>("Scene");
  scene_type.set_function("create_entity",
                          [](const Scene& self, const std::string& name = "") { return self.create_entity(name); });
}
} // namespace ox::LuaBindings
