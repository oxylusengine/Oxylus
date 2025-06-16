#include "LuaManager.hpp"

#include <sol/sol.hpp>

#ifdef OX_LUA_BINDINGS
  #include "LuaApplicationBindings.hpp"
  #include "LuaAssetManagerBindings.hpp"
  #include "LuaAudioBindings.hpp"
  #include "LuaComponentBindings.hpp"
  #include "LuaDebugBindings.hpp"
  #include "LuaInputBindings.hpp"
  #include "LuaMathBindings.hpp"
  #include "LuaPhysicsBindings.hpp"
  #include "LuaRendererBindings.hpp"
  #include "LuaSceneBindings.hpp"
  #include "LuaUIBindings.hpp"
#endif

namespace ox {

auto LuaManager::init() -> std::expected<void, std::string> {
  ZoneScoped;
  m_state = std::make_shared<sol::state>();
  m_state->open_libraries(
      sol::lib::base, sol::lib::package, sol::lib::math, sol::lib::table, sol::lib::os, sol::lib::string);

#ifdef OX_LUA_BINDINGS
  bind_log();
  LuaBindings::bind_application(m_state);
  LuaBindings::bind_math(m_state);
  LuaBindings::bind_scene(m_state);
  LuaBindings::bind_asset_manager(m_state);
  LuaBindings::bind_debug_renderer(m_state);
  LuaBindings::bind_renderer(m_state);
  LuaBindings::bind_components(m_state);
  LuaBindings::bind_input(m_state);
  LuaBindings::bind_audio(m_state);
  LuaBindings::bind_physics(m_state);
  LuaBindings::bind_ui(m_state);
#endif

  return {};
}

auto LuaManager::deinit() -> std::expected<void, std::string> {
  m_state->collect_gc();
  m_state.reset();

  return {};
}

#define SET_LOG_FUNCTIONS(table, name, log_func)                                                                       \
  table.set_function(                                                                                                  \
      name, sol::overload([](const std::string_view message) { log_func("{}", message); }, [](const glm::vec4& vec4) { \
    log_func("x: {} y: {} z: {} w: {}", vec4.x, vec4.y, vec4.z, vec4.w);                                               \
  }, [](const glm::vec3& vec3) { log_func("x: {} y: {} z: {}", vec3.x, vec3.y, vec3.z); }, [](const glm::vec2& vec2) { \
    log_func("x: {} y: {}", vec2.x, vec2.y);                                                                           \
  }, [](const glm::uvec2& vec2) { log_func("x: {} y: {}", vec2.x, vec2.y); }));

void LuaManager::bind_log() const {
  ZoneScoped;
  auto log = m_state->create_table("Log");

  SET_LOG_FUNCTIONS(log, "info", OX_LOG_INFO)
  SET_LOG_FUNCTIONS(log, "warn", OX_LOG_WARN)
  SET_LOG_FUNCTIONS(log, "error", OX_LOG_ERROR)
}
} // namespace ox
