#include "Scripting/LuaManager.hpp"

#include <filesystem>
#include <format>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <sol/sol.hpp>
#include <stdexcept>

#include "Core/Types.hpp"
#include "Scripting/LuaNetworkBindings.hpp"
#include "Utils/Log.hpp"

#ifdef OX_LUA_BINDINGS
  #include "Scripting/LuaApplicationBindings.hpp"  // IWYU pragma: export
  #include "Scripting/LuaAssetManagerBindings.hpp" // IWYU pragma: export
  #include "Scripting/LuaAudioBindings.hpp"        // IWYU pragma: export
  #include "Scripting/LuaDebugBindings.hpp"        // IWYU pragma: export
  #include "Scripting/LuaFlecsBindings.hpp"        // IWYU pragma: export
  #include "Scripting/LuaInputBindings.hpp"        // IWYU pragma: export
  #include "Scripting/LuaMathBindings.hpp"         // IWYU pragma: export
  #include "Scripting/LuaPhysicsBindings.hpp"      // IWYU pragma: export
  #include "Scripting/LuaRMLBindings.hpp"          // IWYU pragma: export
  #include "Scripting/LuaRendererBindings.hpp"     // IWYU pragma: export
  #include "Scripting/LuaSceneBindings.hpp"        // IWYU pragma: export
  #include "Scripting/LuaUIBindings.hpp"           // IWYU pragma: export
  #include "Scripting/LuaVFSBindings.hpp"          // IWYU pragma: export
#endif

namespace ox {
// resolves `path` against the calling script's own file, so scripts load their siblings the same way whether the
// asset root is the editor's project dir or the shipped app dir
static auto require_script(sol::this_state lua, std::string_view path) -> sol::object {
  ZoneScoped;

  // level 1 is the calling lua function, file-loaded chunks carry an '@path' source
  lua_Debug caller = {};
  if (!lua_getstack(lua, 1, &caller) || !lua_getinfo(lua, "Sl", &caller) || caller.source[0] != '@') {
    throw std::runtime_error(std::format("require_script('{}'): caller was not loaded from a file", path));
  }

  const auto script_path = (std::filesystem::path(caller.source + 1).parent_path() / path).lexically_normal();
  const auto key = script_path.generic_string();

  sol::state_view state(lua);
  sol::table loaded = state["package"]["loaded"];
  if (sol::object cached = loaded[key]; cached.valid()) {
    return cached;
  }

  // a missing file would otherwise surface much later as a nil module at the use site
  if (!std::filesystem::exists(script_path)) {
    throw std::runtime_error(
      std::format("{}:{}: require_script('{}'): '{}' does not exist", caller.short_src, caller.currentline, path, key)
    );
  }

  sol::load_result chunk = state.load_file(script_path.string());
  if (!chunk.valid()) {
    const sol::error err = chunk;
    throw std::runtime_error(err.what());
  }

  sol::protected_function_result result = chunk.get<sol::protected_function>()();
  if (!result.valid()) {
    const sol::error err = result;
    throw std::runtime_error(err.what());
  }

  // same contract as lua's require: a module that returns nothing is cached as true
  sol::object module = result.get<sol::object>();
  if (!module.valid()) {
    module = sol::make_object(state, true);
  }
  loaded[key] = module;

  return module;
}

auto LuaManager::init(this LuaManager& self) -> std::expected<void, std::string> {
  ZoneScoped;
  self.state = std::make_unique<sol::state>();
  self.state->open_libraries(
    sol::lib::base,
    sol::lib::package,
    sol::lib::math,
    sol::lib::table,
    sol::lib::os,
    sol::lib::string
  );

  self.state->set_function("require_script", &require_script);

#define BIND(type) self.bind<type>(#type, self.state.get())

#ifdef OX_LUA_BINDINGS
  self.bind_log();
  self.bind_vector();
  BIND(AppBinding);
  BIND(AssetManagerBinding);
  BIND(AudioBinding);
  BIND(DebugBinding);
  BIND(FlecsBinding);
  BIND(InputBinding);
  BIND(MathBinding);
  BIND(PhysicsBinding);
  BIND(RendererBinding);
  BIND(SceneBinding);
  BIND(UIBinding);
  BIND(VFSBinding);
  BIND(RMLBinding);
  BIND(NetworkBinding);
#endif

  return {};
}

auto LuaManager::deinit(this LuaManager& self) -> std::expected<void, std::string> {
  self.state->collect_gc();
  self.state.reset();

  return {};
}

#define SET_LOG_FUNCTIONS(table, name, log_func)                                                                       \
  table.set_function(                                                                                                  \
    name,                                                                                                              \
    sol::overload(                                                                                                     \
      [](const std::string_view message) { log_func("{}", message); },                                                 \
      [](const glm::vec4& vec4) { log_func("x: {} y: {} z: {} w: {}", vec4.x, vec4.y, vec4.z, vec4.w); },              \
      [](const glm::vec3& vec3) { log_func("x: {} y: {} z: {}", vec3.x, vec3.y, vec3.z); },                            \
      [](const glm::vec2& vec2) { log_func("x: {} y: {}", vec2.x, vec2.y); },                                          \
      [](const glm::uvec2& vec2) { log_func("x: {} y: {}", vec2.x, vec2.y); }                                          \
    )                                                                                                                  \
  );

auto LuaManager::bind_log(this const LuaManager& self) -> void {
  ZoneScoped;
  sol::table log = self.state->create_named_table("Oxlog");

  SET_LOG_FUNCTIONS(log, "info", OX_LOG_INFO)
  SET_LOG_FUNCTIONS(log, "warn", OX_LOG_WARN)
  SET_LOG_FUNCTIONS(log, "error", OX_LOG_ERROR)
}

auto LuaManager::bind_vector(this const LuaManager& self) -> void {
  ZoneScoped;

  self.state->set_function("new_number_vector", []() { return std::vector<f64>{}; });
  self.state->set_function("new_string_vector", []() { return std::vector<std::string>{}; });
}
} // namespace ox
