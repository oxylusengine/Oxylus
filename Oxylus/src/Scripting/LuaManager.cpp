#include "Scripting/LuaManager.hpp"

#include <filesystem>
#include <format>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <sol/sol.hpp>
#include <stdexcept>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Core/Types.hpp"
#include "Core/VFS.hpp"
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
// only their addresses matter, as lightuserdata keys and markers; not const so the linker can't fold them together
static u8 module_cache_key = 0;
static u8 module_loading_marker = 0;

// A sibling that was imported loads from its pack, which is all a shipped game has. `script_path` is virtual for a
// cooked caller and physical otherwise, `find_asset` takes either.
static auto load_cooked_module(sol::state_view state, const std::filesystem::path& script_path)
  -> option<sol::load_result> {
  ZoneScoped;

  if (!App::get() || !App::has_mod<AssetManager>()) {
    return nullopt;
  }

  auto& asset_man = App::mod<AssetManager>();
  const auto uuid = asset_man.find_asset(script_path);
  if (!uuid || !asset_man.load_asset(uuid)) {
    return nullopt;
  }

  // the chunk keeps nothing of the payload once it is loaded, so the reference goes straight back
  OX_DEFER(&) { asset_man.unload_asset(uuid); };

  auto script = asset_man.get_script(uuid);
  if (!script || script->bytecode.empty()) {
    return nullopt;
  }

  const auto* bytecode = reinterpret_cast<const c8*>(script->bytecode.data());
  const auto chunk_name = "@" + script_path.generic_string();
  return state.load(std::string_view(bytecode, script->bytecode.size()), chunk_name, sol::load_mode::binary);
}

// resolves `path` against the calling script's own file, so scripts load their siblings the same way whether the
// asset root is the editor's project dir or the shipped app dir
static auto require_script(sol::this_state lua, sol::this_environment this_env, std::string_view path) -> sol::object {
  ZoneScoped;

  // level 1 is the calling lua function, file-loaded chunks carry an '@path' source
  lua_Debug caller = {};
  if (!lua_getstack(lua, 1, &caller) || !lua_getinfo(lua, "Sl", &caller) || caller.source[0] != '@') {
    throw std::runtime_error(std::format("require_script('{}'): caller was not loaded from a file", path));
  }
  if (!this_env) {
    throw std::runtime_error(std::format("require_script('{}'): caller has no environment", path));
  }

  const auto script_path = (std::filesystem::path(caller.source + 1).parent_path() / path).lexically_normal();
  const auto key = script_path.generic_string();

  // cached per environment rather than in package.loaded, so each LuaSystem (and every reload or play session)
  // re-reads its modules and never shares module state with another scene
  sol::state_view state(lua);
  sol::environment& env = this_env;
  auto modules = env.raw_get<sol::optional<sol::table>>(sol::lightuserdata_value(&module_cache_key));
  if (!modules) {
    modules = state.create_table();
    env.raw_set(sol::lightuserdata_value(&module_cache_key), *modules);
  }

  if (sol::object cached = (*modules)[key]; cached.valid()) {
    if (cached.is<void*>() && cached.as<void*>() == &module_loading_marker) {
      throw std::runtime_error(
        std::format(
          "{}:{}: require_script('{}'): circular require of '{}'",
          caller.short_src,
          caller.currentline,
          path,
          key
        )
      );
    }
    return cached;
  }

  auto chunk = load_cooked_module(state, script_path);
  if (!chunk.has_value()) {
    // anything else, a script outside the assets or one that didn't compile, is read off disk
    const auto physical_path = App::get() ? App::get_vfs().to_physical(script_path) : script_path;

    // a missing file would otherwise surface much later as a nil module at the use site
    if (physical_path.empty() || !std::filesystem::exists(physical_path)) {
      throw std::runtime_error(
        std::format("{}:{}: require_script('{}'): '{}' does not exist", caller.short_src, caller.currentline, path, key)
      );
    }

    chunk.emplace(state.load_file(physical_path.string()));
  }

  if (!chunk->valid()) {
    const sol::error err = chunk.value();
    throw std::runtime_error(err.what());
  }

  // no error handler: the outermost call already appends a traceback, nested ones would stack a copy per level
  auto chunk_func = sol::protected_function(chunk->get<sol::function>(), sol::reference(sol::lua_nil));
  env.set_on(chunk_func);

  (*modules)[key] = sol::lightuserdata_value(&module_loading_marker);
  sol::protected_function_result result = chunk_func();
  if (!result.valid()) {
    (*modules)[key] = sol::lua_nil;
    const sol::error err = result;
    throw std::runtime_error(err.what());
  }

  // same contract as lua's require: a module that returns nothing is cached as true
  sol::object module = result.get<sol::object>();
  if (!module.valid()) {
    module = sol::make_object(state, true);
  }
  (*modules)[key] = module;

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
