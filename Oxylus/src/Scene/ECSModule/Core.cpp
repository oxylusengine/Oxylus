#include "Scene/ECSModule/Core.hpp"

#include "Core/App.hpp"
#include "Core/UUID.hpp"
#include "Scripting/LuaManager.hpp"

namespace ox {
Core::Core(flecs::world& world) {
  ZoneScoped;

  world
      .component<glm::vec2>("glm::vec2") //
      .member<f32>("x")
      .member<f32>("y");

  world
      .component<glm::ivec2>("glm::ivec2") //
      .member<i32>("x")
      .member<i32>("y");

  world
      .component<glm::vec3>("glm::vec3") //
      .member<f32>("x")
      .member<f32>("y")
      .member<f32>("z");

  world
      .component<glm::vec4>("glm::vec4") //
      .member<f32>("x")
      .member<f32>("y")
      .member<f32>("z")
      .member<f32>("w");

  world
      .component<glm::mat3>("glm::mat3") //
      .member<glm::vec3>("col0")
      .member<glm::vec3>("col1")
      .member<glm::vec3>("col2");

  world
      .component<glm::mat4>("glm::mat4") //
      .member<glm::vec4>("col0")
      .member<glm::vec4>("col1")
      .member<glm::vec4>("col2")
      .member<glm::vec4>("col3");

  world
      .component<glm::quat>("glm::quat") //
      .member<f32>("x")
      .member<f32>("y")
      .member<f32>("z")
      .member<f32>("w");

  world.component<std::string>("std::string")
      .opaque(flecs::String)
      .serialize([](const flecs::serializer* s, const std::string* data) {
        const char* str = data->c_str();
        return s->value(flecs::String, &str);
      })
      .assign_string([](std::string* data, const char* value) { *data = value; });

  world.component<UUID>("ox::UUID")
      .opaque(flecs::String)
      .serialize([](const flecs::serializer* s, const UUID* data) {
        auto str = data->str();
        return s->value(flecs::String, str.c_str());
      })
      .assign_string([](UUID* data, const char* value) { *data = UUID::from_string(std::string_view(value)).value(); });

#ifdef OX_LUA_BINDINGS
  const auto state = App::get_system<LuaManager>(EngineSystems::LuaManager)->get_state();

  auto component_table = state->create_named_table("Core");

  auto flecs_table = state->create_named_table("flecs");

  // --- id ---
  auto id_type = flecs_table.new_usertype<flecs::id>("id");

  // --- world ---
  auto world_type = flecs_table.new_usertype<flecs::world>(
      "world",

      "entity",
      [](flecs::world& w, const std::string& name) -> flecs::entity { return w.entity(name.c_str()); });

  // --- entity ---
  auto entity_type = flecs_table.new_usertype<flecs::entity>(
      "entity",

      "add",
      [](flecs::entity& e, u64 component_id) -> flecs::entity { return e.add(component_id); },

      "has",
      [](flecs::entity& e, u64 component_id) -> bool { return e.has(component_id); },

      "get",
      [](flecs::entity& e, u64 component_id) -> const void* { return e.get(component_id); },

      "set",
      [](flecs::entity& e, sol::table component_data) { return e; });
#endif

  // clang-format off
#undef ECS_EXPORT_TYPES
#define ECS_REFLECT_TYPES
#include "Scene/ECSModule/Reflect.hpp"
#include "Scene/Components.hpp"
#undef ECS_REFLECT_TYPES
  // clang-format on
}
} // namespace ox
