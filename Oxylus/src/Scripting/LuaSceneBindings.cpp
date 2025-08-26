#include "Scripting/LuaSceneBindings.hpp"

#include <sol/state.hpp>
#include <sol/variadic_args.hpp>

#include "Scene/Scene.hpp"
#include "Scripting/LuaHelpers.hpp"

namespace ox {
auto SceneBinding::bind(sol::state* state) -> void {
  ZoneScoped;
  sol::usertype<Scene> scene_type = state->new_usertype<Scene>("Scene");

  scene_type.set_function("world", [](Scene* scene) -> flecs::world* { return &scene->world; });

  SET_TYPE_FUNCTION(scene_type, Scene, runtime_start);
  SET_TYPE_FUNCTION(scene_type, Scene, runtime_stop);
  SET_TYPE_FUNCTION(scene_type, Scene, runtime_update);
  SET_TYPE_FUNCTION(scene_type, Scene, create_entity);
  SET_TYPE_FUNCTION(scene_type, Scene, create_mesh_entity);
  SET_TYPE_FUNCTION(scene_type, Scene, save_to_file);
  SET_TYPE_FUNCTION(scene_type, Scene, load_from_file);
  SET_TYPE_FUNCTION(scene_type, Scene, safe_entity_name);
  SET_TYPE_FUNCTION(scene_type, Scene, physics_init);
  SET_TYPE_FUNCTION(scene_type, Scene, physics_deinit);
  SET_TYPE_FUNCTION(scene_type, Scene, get_world_transform);
  SET_TYPE_FUNCTION(scene_type, Scene, get_local_transform);

  scene_type.set_function("get_world_position",
                          [](Scene* scene, flecs::entity e) -> glm::vec3 { return scene->get_world_transform(e)[3]; });
  scene_type.set_function("get_local_position",
                          [](Scene* scene, flecs::entity e) -> glm::vec3 { return scene->get_local_transform(e)[3]; });

  scene_type.set_function(
      "defer", [](Scene* scene, sol::function func) { scene->defer_function([func](Scene* s) { func(s); }); });
}
} // namespace ox
