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

auto LuaSystem::init_script(this LuaSystem& self, const std::string& path, const ox::option<std::string> script)
    -> void {
  ZoneScoped;

  self.file_path = path;
  self.script_ = script;

  const auto state = App::get_system<LuaManager>(EngineSystems::LuaManager)->get_state();

  if (self.environment)
    self.environment.reset();
  self.environment = std::make_unique<sol::environment>(*state, sol::create, state->globals());

  const auto load_file_result = script.has_value()
                                    ? state->script(self.script_.value(), *self.environment, sol::script_pass_on_error)
                                    : state->script_file(self.file_path, *self.environment, sol::script_pass_on_error);

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

  constexpr auto reset_unused = [](std::unique_ptr<sol::protected_function>& func) {
    if (!func->valid()) {
      func.reset();
    }
  };

  self.on_add_func = std::make_unique<sol::protected_function>((*self.environment)["on_add"]);
  reset_unused(self.on_add_func);

  self.on_remove_func = std::make_unique<sol::protected_function>((*self.environment)["on_remove"]);
  reset_unused(self.on_remove_func);

  self.on_scene_start_func = std::make_unique<sol::protected_function>((*self.environment)["on_scene_start"]);
  reset_unused(self.on_scene_start_func);

  self.on_scene_stop_func = std::make_unique<sol::protected_function>((*self.environment)["on_scene_stop"]);
  reset_unused(self.on_scene_stop_func);

  self.on_scene_update_func = std::make_unique<sol::protected_function>((*self.environment)["on_scene_update"]);
  reset_unused(self.on_scene_update_func);

  self.on_scene_fixed_update_func = std::make_unique<sol::protected_function>(
      (*self.environment)["on_scene_fixed_update"]);
  reset_unused(self.on_scene_fixed_update_func);

  self.on_scene_render_func = std::make_unique<sol::protected_function>((*self.environment)["on_scene_render"]);
  reset_unused(self.on_scene_render_func);

  self.on_viewport_render_func = std::make_unique<sol::protected_function>((*self.environment)["on_viewport_render"]);
  reset_unused(self.on_viewport_render_func);

  self.on_contact_added_func = std::make_unique<sol::protected_function>((*self.environment)["on_contact_added"]);
  reset_unused(self.on_contact_added_func);

  self.on_contact_persisted_func = std::make_unique<sol::protected_function>(
      (*self.environment)["on_contact_persisted"]);
  reset_unused(self.on_contact_persisted_func);

  self.on_contact_removed_func = std::make_unique<sol::protected_function>((*self.environment)["on_contact_removed"]);
  reset_unused(self.on_contact_removed_func);

  self.on_body_activated_func = std::make_unique<sol::protected_function>((*self.environment)["on_body_activated"]);
  reset_unused(self.on_body_activated_func);

  self.on_body_deactivated_func = std::make_unique<sol::protected_function>((*self.environment)["on_body_deactivated"]);
  reset_unused(self.on_body_deactivated_func);

  state->collect_gc();
}

auto LuaSystem::load(this LuaSystem& self, const std::string& path, const ox::option<std::string> script) -> void {
  ZoneScoped;

  self.init_script(path, script);
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

  self.on_contact_added_func.reset();
  self.on_contact_persisted_func.reset();
  self.on_contact_removed_func.reset();
  self.on_body_activated_func.reset();
  self.on_body_deactivated_func.reset();
}

auto LuaSystem::on_add(this const LuaSystem& self, Scene* scene) -> void {
  ZoneScoped;

  if (!self.on_add_func)
    return;

  const auto result = self.on_add_func->call(scene);
  check_result(result, "on_add");
}

auto LuaSystem::on_remove(this const LuaSystem& self, Scene* scene) -> void {
  ZoneScoped;

  if (!self.on_remove_func)
    return;

  const auto result = self.on_remove_func->call(scene);
  check_result(result, "on_remove");
}

auto LuaSystem::on_scene_start(this const LuaSystem& self, Scene* scene) -> void {
  ZoneScoped;

  if (!self.on_scene_start_func)
    return;

  const auto result = self.on_scene_start_func->call(scene);
  check_result(result, "on_scene_start");
}

auto LuaSystem::on_scene_update(this const LuaSystem& self, Scene* scene, f32 delta_time) -> void {
  ZoneScoped;

  if (!self.on_scene_update_func)
    return;

  const auto result = self.on_scene_update_func->call(scene, delta_time);
  check_result(result, "on_scene_update");
}

auto LuaSystem::on_scene_fixed_update(this const LuaSystem& self, Scene* scene, const f32 delta_time) -> void {
  ZoneScoped;

  if (!self.on_scene_fixed_update_func)
    return;

  const auto result = self.on_scene_fixed_update_func->call(scene, delta_time);
  check_result(result, "on_scene_fixed_update");
}

auto LuaSystem::on_scene_stop(this const LuaSystem& self, Scene* scene) -> void {
  ZoneScoped;

  if (!self.on_scene_stop_func)
    return;

  const auto result = self.on_scene_stop_func->call(scene);
  check_result(result, "on_scene_stop");
}

auto LuaSystem::on_scene_render(this const LuaSystem& self, Scene* scene, vuk::Extent3D extent, vuk::Format format)
    -> void {
  ZoneScoped;

  if (!self.on_scene_render_func)
    return;

  const auto result = self.on_scene_render_func->call(
      scene, glm::vec3(extent.width, extent.height, extent.depth), format);
  check_result(result, "on_scene_render");
}

auto LuaSystem::on_viewport_render(this const LuaSystem& self, Scene* scene, vuk::Extent3D extent, vuk::Format format)
    -> void {
  ZoneScoped;

  if (!self.on_viewport_render_func)
    return;

  const auto result = self.on_viewport_render_func->call(
      scene, glm::vec3(extent.width, extent.height, extent.depth), format);
  check_result(result, "on_viewport_render");
}

auto LuaSystem::on_contact_added(this const LuaSystem& self,
                                 Scene* scene,
                                 const JPH::Body& body1,
                                 const JPH::Body& body2,
                                 const JPH::ContactManifold& manifold,
                                 const JPH::ContactSettings& settings) -> void {
  ZoneScoped;

  if (!self.on_contact_added_func)
    return;

  const auto result = self.on_contact_added_func->call(scene, &body1, &body2, manifold, settings);
  check_result(result, "on_contact_added");
}

auto LuaSystem::on_contact_persisted(this const LuaSystem& self,
                                     Scene* scene,
                                     const JPH::Body& body1,
                                     const JPH::Body& body2,
                                     const JPH::ContactManifold& manifold,
                                     const JPH::ContactSettings& settings) -> void {
  ZoneScoped;

  if (!self.on_contact_persisted_func)
    return;

  const auto result = self.on_contact_persisted_func->call(scene, &body1, &body2, manifold, settings);
  check_result(result, "on_contact_persisted");
}

auto LuaSystem::on_contact_removed(this const LuaSystem& self, Scene* scene, const JPH::SubShapeIDPair& sub_shape_pair)
    -> void {
  ZoneScoped;

  if (!self.on_contact_removed_func)
    return;

  const auto result = self.on_contact_removed_func->call(scene, sub_shape_pair);
  check_result(result, "on_contact_removed");
}

auto
LuaSystem::on_body_activated(this const LuaSystem& self, Scene* scene, const JPH::BodyID& body_id, u64 body_user_data)
    -> void {
  ZoneScoped;

  if (!self.on_body_activated_func)
    return;

  const auto result = self.on_body_activated_func->call(scene, body_id, body_user_data);
  check_result(result, "on_body_activated");
}

auto
LuaSystem::on_body_deactivated(this const LuaSystem& self, Scene* scene, const JPH::BodyID& body_id, u64 body_user_data)
    -> void {
  ZoneScoped;

  if (!self.on_body_deactivated_func)
    return;

  const auto result = self.on_body_deactivated_func->call(scene, body_id, body_user_data);
  check_result(result, "on_body_deactivated");
}
} // namespace ox
