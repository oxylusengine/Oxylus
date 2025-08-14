﻿#pragma once

#include <flecs.h>
#include <sol/environment.hpp>
#include <vuk/Types.hpp>

#include "Oxylus.hpp"

namespace JPH {
class ContactSettings;
class ContactManifold;
class Body;
} // namespace JPH

namespace ox {
class Scene;

enum class ScriptID : u64 { Invalid = std::numeric_limits<u64>::max() };
class LuaSystem {
public:
  LuaSystem() = default;
  explicit LuaSystem(std::string path);
  ~LuaSystem() = default;

  // Either use a path to load it from a lua file or pass in the lua
  auto load(this LuaSystem& self, const std::string& path, const ox::option<std::string> script = nullopt) -> void;
  auto reload(this LuaSystem& self) -> void;

  auto reset_functions(this LuaSystem& self) -> void;

  auto on_add(this const LuaSystem& self, Scene* scene, flecs::entity entity) -> void;
  auto on_remove(this const LuaSystem& self, Scene* scene, flecs::entity entity) -> void;

  auto on_scene_start(this const LuaSystem& self, Scene* scene, flecs::entity entity) -> void;
  auto on_scene_stop(this const LuaSystem& self, Scene* scene, flecs::entity entity) -> void;
  auto on_scene_update(this const LuaSystem& self, Scene* scene, flecs::entity entity, f32 delta_time) -> void;
  auto on_scene_fixed_update(this const LuaSystem& self, Scene* scene, flecs::entity entity, f32 delta_time) -> void;
  auto on_scene_render(this const LuaSystem& self,
                       Scene* scene,
                       flecs::entity entity,
                       const f32 delta_time,
                       vuk::Extent3D extent,
                       vuk::Format format) -> void;

  auto get_path() const -> const std::string& { return file_path; }

private:
  std::string file_path = {};
  ox::option<std::string> script = {};
  ankerl::unordered_dense::map<int, std::string> errors = {};

  std::unique_ptr<sol::environment> environment = nullptr;

  std::unique_ptr<sol::protected_function> on_add_func = nullptr;
  std::unique_ptr<sol::protected_function> on_remove_func = nullptr;

  std::unique_ptr<sol::protected_function> on_scene_start_func = nullptr;
  std::unique_ptr<sol::protected_function> on_scene_stop_func = nullptr;
  std::unique_ptr<sol::protected_function> on_scene_update_func = nullptr;
  std::unique_ptr<sol::protected_function> on_scene_fixed_update_func = nullptr;
  std::unique_ptr<sol::protected_function> on_scene_render_func = nullptr;

  void init_script(this LuaSystem& self, const std::string& path, const ox::option<std::string> script = nullopt);
  static void check_result(const sol::protected_function_result& result, const char* func_name);
};
} // namespace ox
