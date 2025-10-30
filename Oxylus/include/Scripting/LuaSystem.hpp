#pragma once

#include <ankerl/unordered_dense.h>
#include <filesystem>
#include <flecs.h>
#include <sol/environment.hpp>
#include <vuk/Types.hpp>

#include "Core/Option.hpp"
#include "Core/Types.hpp"

namespace JPH {
class ContactSettings;
class ContactManifold;
class Body;
class BodyID;
class SubShapeIDPair;
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
  auto load(this LuaSystem& self, const std::filesystem::path& path, const ox::option<std::string> script = nullopt)
    -> void;
  auto reload(this LuaSystem& self) -> void;

  auto reset_functions(this LuaSystem& self) -> void;

  auto on_add(this const LuaSystem& self, Scene* scene) -> void;
  auto on_remove(this const LuaSystem& self, Scene* scene) -> void;

  auto on_scene_start(this const LuaSystem& self, Scene* scene) -> void;
  auto on_scene_stop(this const LuaSystem& self, Scene* scene) -> void;
  auto on_scene_update(this const LuaSystem& self, Scene* scene, f32 delta_time) -> void;
  auto on_scene_fixed_update(this const LuaSystem& self, Scene* scene, f32 delta_time) -> void;
  auto on_scene_render(this const LuaSystem& self, Scene* scene, vuk::Extent3D extent, vuk::Format format) -> void;
  auto on_viewport_render(this const LuaSystem& self, Scene* scene, vuk::Extent3D extent, vuk::Format format) -> void;

  auto on_contact_added(
    this const LuaSystem& self,
    Scene* scene,
    const JPH::Body& body1,
    const JPH::Body& body2,
    const JPH::ContactManifold& manifold,
    const JPH::ContactSettings& settings
  ) -> void;
  auto on_contact_persisted(
    this const LuaSystem& self,
    Scene* scene,
    const JPH::Body& body1,
    const JPH::Body& body2,
    const JPH::ContactManifold& manifold,
    const JPH::ContactSettings& settings
  ) -> void;
  auto on_contact_removed(this const LuaSystem& self, Scene* scene, const JPH::SubShapeIDPair& sub_shape_pair) -> void;
  auto on_body_activated(this const LuaSystem& self, Scene* scene, const JPH::BodyID& body_id, u64 body_user_data)
    -> void;
  auto on_body_deactivated(this const LuaSystem& self, Scene* scene, const JPH::BodyID& body_id, u64 body_user_data)
    -> void;

  auto get_path() const -> const std::filesystem::path& { return file_path; }

private:
  std::filesystem::path file_path = {};
  ox::option<std::string> script_ = {};
  ankerl::unordered_dense::map<int, std::string> errors = {};

  std::unique_ptr<sol::environment> environment = nullptr;

  std::unique_ptr<sol::protected_function> on_add_func = nullptr;
  std::unique_ptr<sol::protected_function> on_remove_func = nullptr;

  std::unique_ptr<sol::protected_function> on_scene_start_func = nullptr;
  std::unique_ptr<sol::protected_function> on_scene_stop_func = nullptr;
  std::unique_ptr<sol::protected_function> on_scene_update_func = nullptr;
  std::unique_ptr<sol::protected_function> on_scene_fixed_update_func = nullptr;
  std::unique_ptr<sol::protected_function> on_scene_render_func = nullptr;
  std::unique_ptr<sol::protected_function> on_viewport_render_func = nullptr;

  std::unique_ptr<sol::protected_function> on_contact_added_func = nullptr;
  std::unique_ptr<sol::protected_function> on_contact_persisted_func = nullptr;
  std::unique_ptr<sol::protected_function> on_contact_removed_func = nullptr;
  std::unique_ptr<sol::protected_function> on_body_activated_func = nullptr;
  std::unique_ptr<sol::protected_function> on_body_deactivated_func = nullptr;

  void init_script(
    this LuaSystem& self, const std::filesystem::path& path, const ox::option<std::string> script = nullopt
  );
  static void check_result(const sol::protected_function_result& result, const char* func_name);
};
} // namespace ox
