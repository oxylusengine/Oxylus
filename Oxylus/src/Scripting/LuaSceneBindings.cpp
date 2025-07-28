#include "Scripting/LuaSceneBindings.hpp"

#include <sol/state.hpp>
#include <sol/variadic_args.hpp>

#include "Scene/Scene.hpp"
#include "Scripting/LuaHelpers.hpp"

namespace ox {
auto SceneBinding::bind(sol::state* state) -> void {
  ZoneScoped;
  sol::usertype<Scene> scene_type = state->new_usertype<Scene>("Scene");

  SET_TYPE_FUNCTION(scene_type, Scene, runtime_start);
  SET_TYPE_FUNCTION(scene_type, Scene, runtime_stop);
  SET_TYPE_FUNCTION(scene_type, Scene, runtime_update);
  SET_TYPE_FUNCTION(scene_type, Scene, create_entity);
  SET_TYPE_FUNCTION(scene_type, Scene, create_mesh_entity);
  SET_TYPE_FUNCTION(scene_type, Scene, save_to_file);
  SET_TYPE_FUNCTION(scene_type, Scene, load_from_file);
}
} // namespace ox
