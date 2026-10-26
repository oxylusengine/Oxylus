#include "Scene/Components.hpp"

#include "Scene/ComponentRegistry.hpp"

#ifdef OX_LUA_BINDINGS
  #include "Core/App.hpp"
  #include "Scripting/LuaManager.hpp"
#endif

#include "Components.gen.inl"

namespace ox {
CoreComponentsModule::CoreComponentsModule(flecs::world& world) {
  ZoneScoped;

  // import already scoped us to Core, and every child of Core is offered as a gameplay component
  const auto module_scope = world.set_scope(0);
  world.component<AssetFields>("AssetFields");
  world.set_scope(module_scope);

  world.module<CoreComponentsModule>("Core");

#ifdef OX_LUA_BINDINGS
  auto* state = App::mod<LuaManager>().get_state();
  auto registry = ComponentRegistry{world, state, state->create_named_table("Core")};
#else
  auto registry = ComponentRegistry{world};
#endif

  // Math types
  registry.bind_value<&glm::vec2::x, &glm::vec2::y>("glm::vec2");
  registry.bind_value<&glm::ivec2::x, &glm::ivec2::y>("glm::ivec2");
  registry.bind_value<&glm::vec3::x, &glm::vec3::y, &glm::vec3::z>("glm::vec3");
  registry.bind_value<&glm::uvec3::x, &glm::uvec3::y, &glm::uvec3::z>("glm::uvec3");
  registry.bind_value<&glm::vec4::x, &glm::vec4::y, &glm::vec4::z, &glm::vec4::w>("glm::vec4");
  registry.bind_matrix<glm::mat3, glm::vec3, 3>("glm::mat3");
  registry.bind_matrix<glm::mat4, glm::vec4, 4>("glm::mat4");
  registry.bind_value<&glm::quat::x, &glm::quat::y, &glm::quat::z, &glm::quat::w>("glm::quat");

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
      auto* cstr = str.c_str();
      return s->value(flecs::String, &cstr);
    })
    .assign_string([](UUID* data, const char* value) { *data = UUID::from_string(std::string_view(value)).value(); });

  // the one enum not nested in a component, ecsgen can't see it
  registry.bind_enum<GPU::TonemapType>("TonemapType");

  bind_core_components(registry);
}
} // namespace ox
