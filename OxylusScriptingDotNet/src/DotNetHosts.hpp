#pragma once

#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "Scripting/DotNetAbi.hpp"

namespace Rml {
class Context;
}

namespace ox {
struct ComponentDB;
struct DotNetScripting;
class Scene;
class Timestep;

inline auto status_message(OxStatus status) -> std::string_view {
  switch (status) {
    case OxStatus::Ok             : return "ok";
    case OxStatus::AbiMismatch    : return "the managed side was built against a different ABI version";
    case OxStatus::Exception      : return "managed code threw, see the log";
    case OxStatus::NotFound       : return "not found, see the log";
    case OxStatus::LayoutMismatch : return "a C# component doesn't match its native layout, see the log";
    case OxStatus::InvalidArgument: return "invalid argument, see the log";
  }

  return "unknown status";
}

inline auto to_utf8(const std::filesystem::path& path) -> std::string {
  const auto u8 = path.u8string();
  return std::string(reinterpret_cast<const c8*>(u8.data()), u8.size());
}

// which ComponentDB a component defined from C# registers in, null forgets the world
auto set_component_db(ecs_world_t* world, ComponentDB* component_db) -> void;

// what OxNativeApi::time points at
auto native_time() -> OxTime&;

// which scene a world belongs to, for the physics calls, null forgets the world
auto set_scene(ecs_world_t* world, Scene* scene) -> void;

auto scene_of(ecs_world_t* world) -> Scene*;

auto fill_input_api(OxNativeApi& api) -> void;
auto fill_physics_api(OxNativeApi& api) -> void;
auto fill_content_api(OxNativeApi& api) -> void;
auto fill_ui_api(OxNativeApi& api) -> void;
auto fill_net_api(OxNativeApi& api) -> void;
auto fill_app_api(OxNativeApi& api) -> void;

// what the UI and network callbacks call into, null while nothing is loaded
auto set_managed_api(const OxManagedApi* managed_api) -> void;
auto managed_api() -> const OxManagedApi*;

auto set_ui_context(ecs_world_t* world, Rml::Context* context) -> void;
// drops the listeners and data models the scripts made, in one world or in all of them
auto release_ui(ecs_world_t* world) -> void;
auto release_ui() -> void;

// forwards the app's WindowResizeEvent to the scripts, a no-op without an App. Released with the scripts
auto subscribe_window_events() -> void;
auto release_window_events() -> void;

// ticks what the scripts opened, closing it all on release
auto tick_network(const Timestep& timestep) -> void;
auto release_network() -> void;

auto make_scene_host(DotNetScripting& module) -> std::unique_ptr<SceneScriptHost>;

// `scripting_directory` empty means the game assembly's own directory
auto hostfxr_load(
  const std::filesystem::path& game_assembly,
  const std::filesystem::path& scripting_directory,
  const OxNativeApi& native_api,
  OxManagedApi& managed_api,
  u32 abi_version
) -> std::expected<void, std::string>;

auto aot_load(
  const std::filesystem::path& library, const OxNativeApi& native_api, OxManagedApi& managed_api, u32 abi_version
) -> std::expected<void, std::string>;
} // namespace ox
