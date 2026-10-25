#pragma once

#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "Scripting/DotNetAbi.hpp"

namespace ox {
struct ComponentDB;
struct DotNetScripting;
class Scene;

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

auto fill_input_api(OxNativeApi& api) -> void;
auto fill_physics_api(OxNativeApi& api) -> void;

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
