#include "LuaSystem.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <sol/state.hpp>

#include "Core/App.hpp"
#include "LuaManager.hpp"
#include "Scene/Scene.hpp"

namespace ox {
LuaSystem::LuaSystem(std::string path) : file_path(std::move(path)) { init_script(file_path); }

void LuaSystem::check_result(const sol::protected_function_result& result, const char* func_name) {
  if (!result.valid()) {
    const sol::error err = result;
    OX_LOG_ERROR("Error in {0}: {1}", func_name, err.what());
  }
}

void LuaSystem::init_script(const std::string& path) {
  ZoneScoped;
  if (!std::filesystem::exists(path)) {
    OX_LOG_ERROR("Couldn't find the script file! {}", path);
    return;
  }

  const auto state = App::get_system<LuaManager>(EngineSystems::LuaManager)->get_state();
  environment = std::make_unique<sol::environment>(*state, sol::create, state->globals());

  const auto load_file_result = state->script_file(file_path, *environment, sol::script_pass_on_error);

  if (!load_file_result.valid()) {
    const sol::error err = load_file_result;
    OX_LOG_ERROR("Failed to Execute Lua script {0}", file_path);
    OX_LOG_ERROR("Error : {0}", err.what());
    std::string error = std::string(err.what());

    const auto linepos = error.find(".lua:");
    std::string error_line = error.substr(linepos + 5); //+4 .lua: + 1
    const auto linepos_end = error_line.find(':');
    error_line = error_line.substr(0, linepos_end);
    const int line = std::stoi(error_line);
    error = error.substr(linepos + error_line.size() + linepos_end + 4); //+4 .lua:

    errors[line] = error;
  }

  for (auto [l, e] : errors) {
    OX_LOG_ERROR("{} {}", l, e);
  }

  on_init_func = std::make_unique<sol::protected_function>((*environment)["on_init"]);
  if (!on_init_func->valid())
    on_init_func.reset();

  on_update_func = std::make_unique<sol::protected_function>((*environment)["on_update"]);
  if (!on_update_func->valid())
    on_update_func.reset();

  on_fixed_update_func = std::make_unique<sol::protected_function>((*environment)["on_fixed_update"]);
  if (!on_fixed_update_func->valid())
    on_fixed_update_func.reset();

  on_render_func = std::make_unique<sol::protected_function>((*environment)["on_render"]);
  if (!on_render_func->valid())
    on_render_func.reset();

  on_release_func = std::make_unique<sol::protected_function>((*environment)["on_release"]);
  if (!on_release_func->valid())
    on_release_func.reset();

  on_contact_added_func = std::make_unique<sol::protected_function>((*environment)["on_contact_added"]);
  if (!on_contact_added_func->valid())
    on_contact_added_func.reset();

  on_contact_persisted_func = std::make_unique<sol::protected_function>((*environment)["on_contact_persisted"]);
  if (!on_contact_persisted_func->valid())
    on_contact_persisted_func.reset();

  state->collect_gc();
}

void LuaSystem::on_init(Scene* scene, flecs::entity entity) {
  ZoneScoped;
  if (on_init_func) {
    bind_globals(scene, entity, static_cast<f32>(App::get_timestep().get_millis()));
    const auto result = on_init_func->call();
    check_result(result, "on_init");
  }
}

void LuaSystem::on_update(f32 delta_time) {
  ZoneScoped;
  if (on_update_func) {
    const auto result = on_update_func->call(delta_time);
    check_result(result, "on_update");
  }
}

void LuaSystem::on_fixed_update(float delta_time) {
  ZoneScoped;
  if (on_fixed_update_func) {
    const auto result = on_fixed_update_func->call(delta_time);
    check_result(result, "on_fixed_update");
  }
}

void LuaSystem::on_release(Scene* scene, flecs::entity entity) {
  ZoneScoped;
  if (on_release_func) {
    const auto result = on_release_func->call();
    check_result(result, "on_release");
  }

  App::get_system<LuaManager>(EngineSystems::LuaManager)->get_state()->collect_gc();
}

void LuaSystem::on_render(vuk::Extent3D extent, vuk::Format format) {
  ZoneScoped;
  if (on_render_func) {
    const auto result = on_render_func->call(extent, format);
    check_result(result, "on_render");
  }
}

auto LuaSystem::on_contact_added(Scene* scene,
                                 flecs::entity e,
                                 const JPH::Body& body1,
                                 const JPH::Body& body2,
                                 const JPH::ContactManifold& manifold,
                                 const JPH::ContactSettings& settings) -> void {
  ZoneScoped;
  if (on_contact_added_func) {
    const auto result = on_contact_added_func->call(scene, e, std::ref(body1), std::ref(body2), manifold, settings);
    check_result(result, "on_contact_added");
  }
}

auto LuaSystem::on_contact_persisted(Scene* scene,
                                     flecs::entity e,
                                     const JPH::Body& body1,
                                     const JPH::Body& body2,
                                     const JPH::ContactManifold& manifold,
                                     const JPH::ContactSettings& settings) -> void {
  ZoneScoped;
  if (on_contact_persisted_func) {
    const auto result = on_contact_persisted_func->call(scene, e, std::ref(body1), std::ref(body2), manifold, settings);
    check_result(result, "on_contact_persisted");
  }
}

void LuaSystem::load(const std::string& path) {
  ZoneScoped;
  init_script(path);
}

void LuaSystem::reload() {
  ZoneScoped;
  if (environment) {
    const sol::protected_function releaseFunc = (*environment)["on_release"];
    if (releaseFunc.valid()) {
      const auto result = releaseFunc.call();
      check_result(result, "on_release");
    }
  }

  init_script(file_path);
}

void LuaSystem::bind_globals(Scene* scene, flecs::entity entity, f32 delta_time) const {
  (*environment)["scene"] = scene;
  (*environment)["world"] = std::ref(scene->world);
  (*environment)["this"] = entity;
  (*environment)["delta_time"] = delta_time;
}
} // namespace ox
