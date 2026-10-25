#pragma once

#include <ankerl/svector.h>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Core/Option.hpp"
#include "Scripting/DotNetAbi.hpp"
#include "Scripting/SceneScript.hpp"

namespace ox {
struct ComponentDB;
class ManagedSceneScript;
class Timestep;

enum class DotNetHostKind : u8 {
  // CoreCLR through hostfxr, `path` is the game's managed assembly. Oxylus.Scripting and its runtimeconfig sit next
  // to it, and the game loads into a collectible context so it can be unloaded and rebuilt
  HostFxr,
  // the game published with NativeAOT as a shared library, `path` is that library
  Aot,
};

struct DotNetLoadInfo {
  DotNetHostKind kind = DotNetHostKind::Aot;
  std::filesystem::path path = {};
  // hostfxr only, where Oxylus.Scripting and its runtimeconfig are. Empty means next to `path`, the editor points it
  // at its own copy so a project only references it
  std::filesystem::path scripting_directory = {};
  u32 abi_version = OX_DOTNET_ABI_VERSION;
};

struct DotNetScriptType {
  std::string name = {};
  OxScriptKind kind = OxScriptKind::None;
};

auto make_native_api() -> OxNativeApi;

struct DotNetScripting {
  constexpr static auto MODULE_NAME = "DotNetScripting";

  // loaded at init when set, a game points this at its published scripts, the editor loads its project later
  option<DotNetLoadInfo> startup = nullopt;
  OxManagedApi managed_api = {};

  struct AttachedWorld {
    ecs_world_t* world = nullptr;
    ComponentDB* component_db = nullptr;
    // set by a scene's runtime between runtime_start and runtime_stop, behaviours only live while it is
    bool is_running = false;
    ankerl::svector<ManagedSceneScript*, 4> scripts = {};
  };

  // kept across an unload, a reload attaches them again and recreates their scripts and behaviours
  ankerl::svector<AttachedWorld, 4> attached_worlds = {};
  // every scene script and behaviour class of what is loaded, sorted by name, empty while nothing is
  std::vector<DotNetScriptType> script_types = {};
  std::unique_ptr<SceneScriptHost> scene_host = nullptr;

  DotNetScripting() = default;
  explicit DotNetScripting(DotNetLoadInfo startup_info) : startup(std::move(startup_info)) {}

  auto init(this DotNetScripting& self) -> std::expected<void, std::string>;
  auto deinit(this DotNetScripting& self) -> std::expected<void, std::string>;
  // advances the clock C# reads as Time, before the scenes the modules after this one update
  auto update(this DotNetScripting& self, const Timestep& timestep) -> void;

  auto load(this DotNetScripting& self, const DotNetLoadInfo& info, const OxNativeApi& native_api = make_native_api())
    -> std::expected<void, std::string>;
  auto unload(this DotNetScripting& self) -> void;
  auto is_loaded(this const DotNetScripting& self) -> bool;

  // resolves the C# components in `world`, checks their layouts and registers the C# systems. Detach before the world
  // is destroyed. `component_db` is where components defined in C# register, so a scene serializes them. Scenes do
  // this themselves through their runtime
  auto attach_world(this DotNetScripting& self, ecs_world_t* world, ComponentDB* component_db = nullptr)
    -> std::expected<void, std::string>;
  auto detach_world(this DotNetScripting& self, ecs_world_t* world) -> void;
  auto find_world(this DotNetScripting& self, ecs_world_t* world) -> AttachedWorld*;

  // creates a behaviour for every ScriptComponent in the world, then starts the scene scripts and behaviours
  auto start_world(this DotNetScripting& self, ecs_world_t* world) -> void;
  auto stop_world(this DotNetScripting& self, ecs_world_t* world) -> void;

  auto add_scene_script(this DotNetScripting& self, ecs_world_t* world, ManagedSceneScript& script) -> void;
  auto remove_scene_script(this DotNetScripting& self, ecs_world_t* world, ManagedSceneScript& script) -> void;
};
} // namespace ox
