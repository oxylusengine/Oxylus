#include "Scripting/LuaSceneBindings.hpp"

#include <RmlUi/Core/Context.h>
#include <sol/state.hpp>
#include <sol/variadic_args.hpp>

#include "Scene/Scene.hpp"
#include "Scripting/LuaHelpers.hpp"

struct ecs_world_t {};

namespace ox {
auto SceneBinding::bind(sol::state* state) -> void {
  ZoneScoped;
  sol::usertype<Scene> scene_type = state->new_usertype<Scene>("Scene");

  scene_type.set_function("world", [](Scene* scene) -> ecs_world_t* { return scene->world.world_; });

  scene_type["input_focused"] = &Scene::input_focused;

  SET_TYPE_FUNCTION(scene_type, Scene, runtime_start);
  SET_TYPE_FUNCTION(scene_type, Scene, runtime_stop);
  SET_TYPE_FUNCTION(scene_type, Scene, runtime_update);
  scene_type.set_function(
    "create_entity",
    [](Scene* scene, sol::optional<std::string> name, sol::optional<bool> safe_naming) {
      return scene->create_entity(name.has_value() ? *name : "", safe_naming.has_value() ? *safe_naming : false);
    }
  );
  SET_TYPE_FUNCTION(scene_type, Scene, create_model_entity);
  SET_TYPE_FUNCTION(scene_type, Scene, create_particle_system_entity);
  SET_TYPE_FUNCTION(scene_type, Scene, save_to_file);
  SET_TYPE_FUNCTION(scene_type, Scene, load_from_file);
  SET_TYPE_FUNCTION(scene_type, Scene, safe_entity_name);
  SET_TYPE_FUNCTION(scene_type, Scene, physics_init);
  SET_TYPE_FUNCTION(scene_type, Scene, physics_deinit);
  SET_TYPE_FUNCTION(scene_type, Scene, get_world_transform);
  SET_TYPE_FUNCTION(scene_type, Scene, get_local_transform);
  SET_TYPE_FUNCTION(scene_type, Scene, get_renderer_instance);
  SET_TYPE_FUNCTION(scene_type, Scene, is_running);
  SET_TYPE_FUNCTION(scene_type, Scene, get_rml_context);
  SET_TYPE_FUNCTION(scene_type, Scene, get_rml_context_name);
  SET_TYPE_FUNCTION(scene_type, Scene, set_rml_dpi_ratio);
  SET_TYPE_FUNCTION(scene_type, Scene, clear_rml_dpi_ratio_override);
  SET_TYPE_FUNCTION(scene_type, Scene, get_uuid);

  scene_type.set_function("get_world_position", [](Scene* scene, flecs::entity e) -> glm::vec3 {
    return scene->get_world_transform(e)[3];
  });
  scene_type.set_function("get_local_position", [](Scene* scene, flecs::entity e) -> glm::vec3 {
    return scene->get_local_transform(e)[3];
  });

  SET_TYPE_FUNCTION(scene_type, Scene, play_particles);
  SET_TYPE_FUNCTION(scene_type, Scene, stop_particles);
  SET_TYPE_FUNCTION(scene_type, Scene, restart_particles);
  SET_TYPE_FUNCTION(scene_type, Scene, is_particles_playing);
  SET_TYPE_FUNCTION(scene_type, Scene, emit_particle_burst);

  // one Lua name for both overloads: a number picks the slot, a string resolves against the asset
  scene_type.set_function(
    "set_particle_parameter",
    [](Scene* scene, flecs::entity e, sol::object key, const glm::vec4& value) -> bool {
      if (key.is<u32>()) {
        scene->set_particle_parameter(e, key.as<u32>(), value);
        return true;
      }

      if (key.is<std::string>()) {
        return scene->set_particle_parameter(e, key.as<std::string>(), value);
      }

      return false;
    }
  );

  scene_type.set_function("defer", [](Scene* scene, sol::function func) {
    scene->defer_function([func](Scene* s) {
      ZoneScopedN("scene::defer lua function");
      func(s);
    });
  });
}
} // namespace ox
