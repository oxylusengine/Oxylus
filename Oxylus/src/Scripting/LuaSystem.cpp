#include "Scripting/LuaSystem.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <sol/state.hpp>

#include "Core/App.hpp"
#include "Scene/Scene.hpp"
#include "Scripting/LuaManager.hpp"

namespace ox {
LuaSystem::LuaSystem(std::string path) : file_path(std::move(path)) {
  ZoneScoped;

  init_script(file_path);
}

auto LuaSystem::check_result(const sol::protected_function_result& result, const char* func_name) -> void {
  ZoneScoped;

  if (!result.valid()) {
    const sol::error err = result;
    OX_LOG_ERROR("Error in {0}: {1}", func_name, err.what());
  }
}

auto LuaSystem::init_script(this LuaSystem& self, const std::string& path) -> void {
  ZoneScoped;

  self.file_path = path;

  if (!std::filesystem::exists(path)) {
    OX_LOG_ERROR("Couldn't find the script file! {}", self.file_path);
    return;
  }

  const auto state = App::get_system<LuaManager>(EngineSystems::LuaManager)->get_state();
  self.environment = std::make_unique<sol::environment>(*state, sol::create, state->globals());

  const auto load_file_result = state->script_file(self.file_path, *self.environment, sol::script_pass_on_error);

  if (!load_file_result.valid()) {
    const sol::error err = load_file_result;
    OX_LOG_ERROR("Failed to Execute Lua script {0}", self.file_path);
    OX_LOG_ERROR("Error : {0}", err.what());
    std::string error = std::string(err.what());

    const auto linepos = error.find(".lua:");
    std::string error_line = error.substr(linepos + 5); //+4 .lua: + 1
    const auto linepos_end = error_line.find(':');
    error_line = error_line.substr(0, linepos_end);
    const int line = std::stoi(error_line);
    error = error.substr(linepos + error_line.size() + linepos_end + 4); //+4 .lua:

    self.errors[line] = error;
  }

  for (auto [l, e] : self.errors) {
    OX_LOG_ERROR("{} {}", l, e);
  }

  self.on_add_func = std::make_unique<sol::protected_function>((*self.environment)["on_add"]);
  if (!self.on_add_func->valid())
    self.on_add_func.reset();

  self.on_remove_func = std::make_unique<sol::protected_function>((*self.environment)["on_remove"]);
  if (!self.on_remove_func->valid())
    self.on_remove_func.reset();

  self.on_scene_start_func = std::make_unique<sol::protected_function>((*self.environment)["on_scene_start"]);
  if (!self.on_scene_start_func->valid())
    self.on_scene_start_func.reset();

  self.on_scene_stop_func = std::make_unique<sol::protected_function>((*self.environment)["on_scene_stop"]);
  if (!self.on_scene_stop_func->valid())
    self.on_scene_stop_func.reset();

  self.on_scene_update_func = std::make_unique<sol::protected_function>((*self.environment)["on_scene_update"]);
  if (!self.on_scene_update_func->valid())
    self.on_scene_update_func.reset();

  self.on_scene_fixed_update_func = std::make_unique<sol::protected_function>((*self.environment)["on_scene_fixed_update"]);
  if (!self.on_scene_fixed_update_func->valid())
    self.on_scene_fixed_update_func.reset();

  self.on_scene_render_func = std::make_unique<sol::protected_function>((*self.environment)["on_scene_render"]);
  if (!self.on_scene_render_func->valid())
    self.on_scene_render_func.reset();

  state->collect_gc();
}

auto LuaSystem::load(this LuaSystem& self, const std::string& path) -> void {
  ZoneScoped;

  self.init_script(path);
}

auto LuaSystem::reload(this LuaSystem& self) -> void {
  ZoneScoped;

  self.reset_functions();

  self.init_script(self.file_path);
}

auto LuaSystem::reset_functions(this LuaSystem& self) -> void {
  ZoneScoped;

  self.on_add_func.reset();
  self.on_remove_func.reset();

  self.on_scene_start_func.reset();
  self.on_scene_stop_func.reset();
  self.on_scene_update_func.reset();
  self.on_scene_fixed_update_func.reset();
  self.on_scene_render_func.reset();
}

auto LuaSystem::bind_globals(this const LuaSystem& self, Scene* scene, flecs::entity entity, f32 delta_time) -> void {
  (*self.environment)["scene"] = scene;
  (*self.environment)["world"] = std::ref(scene->world);
  (*self.environment)["entity"] = entity;
  (*self.environment)["delta_time"] = delta_time;
}

auto LuaSystem::on_add(this const LuaSystem& self, Scene* scene, flecs::entity entity) -> void {
  ZoneScoped;

  if (!self.on_add_func)
    return;

  self.bind_globals(scene, entity, 0);
  const auto result = self.on_add_func->call();
  check_result(result, "on_add");
}

auto LuaSystem::on_remove(this const LuaSystem& self, Scene* scene, flecs::entity entity) -> void {
  ZoneScoped;

  if (!self.on_remove_func)
    return;

  self.bind_globals(scene, entity, 0);
  const auto result = self.on_remove_func->call();
  check_result(result, "on_remove");
}

auto LuaSystem::on_scene_start(this const LuaSystem& self, Scene* scene, flecs::entity entity) -> void {
  ZoneScoped;

  if (!self.on_scene_start_func)
    return;

  self.bind_globals(scene, entity, 0);
  const auto result = self.on_scene_start_func->call();
  check_result(result, "on_scene_start");
}

auto LuaSystem::on_scene_update(this const LuaSystem& self, f32 delta_time) -> void {
  ZoneScoped;

  if (!self.on_scene_update_func)
    return;

  const auto result = self.on_scene_update_func->call(delta_time);
  check_result(result, "on_scene_update");
}

auto LuaSystem::on_scene_fixed_update(this const LuaSystem& self, const float delta_time) -> void {
  ZoneScoped;

  if (!self.on_scene_fixed_update_func)
    return;

  const auto result = self.on_scene_fixed_update_func->call(delta_time);
  check_result(result, "on_scene_fixed_update");
}

auto LuaSystem::on_scene_stop(this const LuaSystem& self, Scene* scene, flecs::entity entity) -> void {
  ZoneScoped;

  if (!self.on_scene_stop_func)
    return;

  const auto result = self.on_scene_stop_func->call();
  check_result(result, "on_scene_stop");
}

auto LuaSystem::on_scene_render(this const LuaSystem& self, vuk::Extent3D extent, vuk::Format format) -> void {
  ZoneScoped;

  if (!self.on_scene_render_func)
    return;

  const auto result = self.on_scene_render_func->call(extent, format);
  check_result(result, "on_scene_render");
}
} // namespace ox
