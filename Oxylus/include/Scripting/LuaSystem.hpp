#pragma once

#include <filesystem>
#include <flecs.h>
#include <sol/environment.hpp>
#include <vuk/Types.hpp>

#include "Core/Option.hpp"
#include "Core/Types.hpp"
#include "Scripting/LuaScript.hpp"
#include "Scripting/SceneScript.hpp"

namespace ox {
// One scene's live instance of a LuaScript: its own environment and its own resolved callbacks. Never share one
// between scenes, that is what makes two scenes fight over the script's state.
class LuaSystem final : public SceneScript {
public:
  LuaSystem() = default;
  explicit LuaSystem(const LuaScript& script);
  ~LuaSystem() override = default;

  // Either use a path to load it from a lua file or pass in the lua
  auto load(this LuaSystem& self, const std::filesystem::path& path, const ox::option<std::string> script = nullopt)
    -> void;
  auto reload(this LuaSystem& self) -> void;
  // a fresh copy of the asset, for a script that was cooked again since this system loaded it
  auto reload(this LuaSystem& self, const LuaScript& script) -> void;

  auto reset_functions(this LuaSystem& self) -> void;

  auto display_name() const -> std::string override { return script_.path.filename().string(); }

  auto on_add(Scene* scene) -> void override;
  auto on_remove(Scene* scene) -> void override;

  auto on_scene_start(Scene* scene) -> void override;
  auto on_scene_stop(Scene* scene) -> void override;
  auto on_scene_update(Scene* scene, f32 delta_time) -> void override;
  auto on_scene_fixed_update(Scene* scene, f32 delta_time) -> void override;
  auto on_scene_render(Scene* scene, vuk::Extent3D extent) -> void override;

  auto on_contact_added(
    Scene* scene,
    const JPH::Body& body1,
    const JPH::Body& body2,
    const JPH::ContactManifold& manifold,
    const JPH::ContactSettings& settings
  ) -> void override;
  auto on_contact_persisted(
    Scene* scene,
    const JPH::Body& body1,
    const JPH::Body& body2,
    const JPH::ContactManifold& manifold,
    const JPH::ContactSettings& settings
  ) -> void override;
  auto on_contact_removed(Scene* scene, const JPH::SubShapeIDPair& sub_shape_pair) -> void override;
  auto on_body_activated(Scene* scene, const JPH::BodyID& body_id, u64 body_user_data) -> void override;
  auto on_body_deactivated(Scene* scene, const JPH::BodyID& body_id, u64 body_user_data) -> void override;

  auto get_path() const -> const std::filesystem::path& { return script_.path; }

private:
  LuaScript script_ = {};

  std::unique_ptr<sol::environment> environment = nullptr;

  std::unique_ptr<sol::protected_function> on_add_func = nullptr;
  std::unique_ptr<sol::protected_function> on_remove_func = nullptr;

  std::unique_ptr<sol::protected_function> on_scene_start_func = nullptr;
  std::unique_ptr<sol::protected_function> on_scene_stop_func = nullptr;
  std::unique_ptr<sol::protected_function> on_scene_update_func = nullptr;
  std::unique_ptr<sol::protected_function> on_scene_fixed_update_func = nullptr;
  std::unique_ptr<sol::protected_function> on_scene_render_func = nullptr;

  std::unique_ptr<sol::protected_function> on_contact_added_func = nullptr;
  std::unique_ptr<sol::protected_function> on_contact_persisted_func = nullptr;
  std::unique_ptr<sol::protected_function> on_contact_removed_func = nullptr;
  std::unique_ptr<sol::protected_function> on_body_activated_func = nullptr;
  std::unique_ptr<sol::protected_function> on_body_deactivated_func = nullptr;

  auto init_script(this LuaSystem& self, LuaScript script) -> void;
  static void check_result(const sol::protected_function_result& result, const char* func_name);
};
} // namespace ox
