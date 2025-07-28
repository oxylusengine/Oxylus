#include "Scripting/LuaManager.hpp"

#include <sol/sol.hpp>

#ifdef OX_LUA_BINDINGS
  #include "Scripting/LuaApplicationBindings.hpp"
  #include "Scripting/LuaAssetManagerBindings.hpp"
  #include "Scripting/LuaAudioBindings.hpp"
  #include "Scripting/LuaDebugBindings.hpp"
  #include "Scripting/LuaFlecsBindings.hpp"
  #include "Scripting/LuaInputBindings.hpp"
  #include "Scripting/LuaMathBindings.hpp"
  #include "Scripting/LuaPhysicsBindings.hpp"
  #include "Scripting/LuaRendererBindings.hpp"
  #include "Scripting/LuaSceneBindings.hpp"
  #include "Scripting/LuaUIBindings.hpp"
#endif

namespace ox {
auto LuaManager::init() -> std::expected<void, std::string> {
  ZoneScoped;
  _state = std::make_unique<sol::state>();
  _state->open_libraries(
      sol::lib::base, sol::lib::package, sol::lib::math, sol::lib::table, sol::lib::os, sol::lib::string);

#define BIND(type) bind<type>(#type, _state.get());

#ifdef OX_LUA_BINDINGS
  bind_log();
  BIND(AppBinding);
  BIND(AssetManagerBinding);
  BIND(AudioBinding);
  BIND(DebugBinding);
  BIND(FlecsBinding);
  BIND(MathBinding);
  BIND(PhysicsBinding);
  BIND(RendererBinding);
  BIND(SceneBinding);
  BIND(UIBinding);
#endif

  return {};
}

auto LuaManager::deinit() -> std::expected<void, std::string> {
  _state->collect_gc();
  _state.reset();

  return {};
}

#define SET_LOG_FUNCTIONS(table, name, log_func)                                                              \
  table.set_function(                                                                                         \
      name,                                                                                                   \
      sol::overload(                                                                                          \
          [](const std::string_view message) { log_func("{}", message); },                                    \
          [](const glm::vec4& vec4) { log_func("x: {} y: {} z: {} w: {}", vec4.x, vec4.y, vec4.z, vec4.w); }, \
          [](const glm::vec3& vec3) { log_func("x: {} y: {} z: {}", vec3.x, vec3.y, vec3.z); },               \
          [](const glm::vec2& vec2) { log_func("x: {} y: {}", vec2.x, vec2.y); },                             \
          [](const glm::uvec2& vec2) { log_func("x: {} y: {}", vec2.x, vec2.y); }));

void LuaManager::bind_log() const {
  ZoneScoped;
  auto log = _state->create_table("Log");

  SET_LOG_FUNCTIONS(log, "info", OX_LOG_INFO)
  SET_LOG_FUNCTIONS(log, "warn", OX_LOG_WARN)
  SET_LOG_FUNCTIONS(log, "error", OX_LOG_ERROR)
}
} // namespace ox
