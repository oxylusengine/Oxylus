#include "Scripting/DotNetScripting.hpp"

#include <SDL3/SDL_filesystem.h>
#include <algorithm>
#include <flecs.h>
#include <fmt/format.h>
#include <vector>

#include "Core/App.hpp"
#include "DotNetHosts.hpp"
#include "Scene/Components.hpp"
#include "Scene/Scene.hpp"
#include "Scripting/ManagedSceneScript.hpp"
#include "Utils/Log.hpp"
#include "Utils/Timestep.hpp"

namespace ox {
static auto published_scripts(const std::filesystem::path& scripts_path) -> DotNetLoadInfo {
  const auto* base_path = SDL_GetBasePath();
  auto path = (base_path ? std::filesystem::path(base_path) : std::filesystem::path()) / scripts_path;
#if defined(OX_PLATFORM_WINDOWS)
  path += ".dll";
#elif defined(OX_PLATFORM_MACOSX)
  path += ".dylib";
#else
  path += ".so";
#endif

  return {.kind = DotNetHostKind::Aot, .path = std::move(path)};
}

static auto attach_managed(const OxManagedApi& managed_api, const DotNetScripting::AttachedWorld& attached)
  -> std::expected<void, std::string> {
  ZoneScoped;

  set_component_db(attached.world, attached.component_db);
  if (const auto status = managed_api.attach_world(attached.world); status != OxStatus::Ok) {
    set_component_db(attached.world, nullptr);
    return std::unexpected(fmt::format("Couldn't attach C# scripts to a world: {}", status_message(status)));
  }

  return {};
}

static auto is_complete(const OxManagedApi& api) -> bool {
  return api.size == sizeof(OxManagedApi) && api.shutdown && api.attach_world && api.detach_world &&
         api.allocated_bytes && api.find_script_type && api.list_script_types && api.script_create &&
         api.script_destroy && api.behaviour_add && api.behaviour_remove && api.world_start && api.world_stop &&
         api.world_update && api.world_fixed_update && api.world_render && api.world_contacts &&
         api.world_body_activation && api.ui_event && api.ui_model_get && api.ui_model_set && api.ui_model_event &&
         api.net_rpc && api.net_event && api.net_snapshot && api.window_resized;
}

static auto create_scene_script(const OxManagedApi& managed_api, ecs_world_t* world, ManagedSceneScript& script)
  -> void {
  const auto type_name = OxStringView{.data = script.type_name.data(), .length = script.type_name.size()};
  if (const auto status = managed_api.script_create(world, type_name, &script.handle); status != OxStatus::Ok) {
    OX_LOG_ERROR("Couldn't create C# scene behaviour {}: {}", script.type_name, status_message(status));
    script.handle = 0;
  }
}

static auto create_behaviours(const OxManagedApi& managed_api, ecs_world_t* world) -> void {
  ZoneScoped;

  // collected first, a behaviour's OnCreate can change the tables the query walks
  auto cpp_world = flecs::world(world);
  auto entities = std::vector<flecs::entity_t>();
  cpp_world.query_builder<const ScriptComponent>().build().each([&entities](flecs::entity e, const ScriptComponent&) {
    entities.push_back(e);
  });

  const auto script_component = cpp_world.id<ScriptComponent>();
  for (const auto entity : entities) {
    const auto* component = static_cast<const ScriptComponent*>(ecs_get_id(world, entity, script_component));
    if (!component) {
      continue;
    }

    const auto type_name = OxStringView{.data = component->type_name.data(), .length = component->type_name.size()};
    if (const auto status = managed_api.behaviour_add(world, entity, type_name); status != OxStatus::Ok) {
      OX_LOG_ERROR("Couldn't create C# behaviour {}: {}", component->type_name, status_message(status));
    }
  }
}

auto DotNetScripting::init(this DotNetScripting& self) -> std::expected<void, std::string> {
  ZoneScoped;

  self.scene_host = make_scene_host(self);
  Scene::set_script_host(self.scene_host.get());

  if (!self.startup.has_value() && App::get() && !App::get()->get_scripts_path().empty()) {
    self.startup = published_scripts(App::get()->get_scripts_path());
  }

  if (!self.startup.has_value()) {
    return {};
  }

  return self.load(*self.startup);
}

auto DotNetScripting::deinit(this DotNetScripting& self) -> std::expected<void, std::string> {
  ZoneScoped;

  Scene::set_script_host(nullptr);
  self.unload();

  return {};
}

auto DotNetScripting::update(this DotNetScripting&, const Timestep& timestep) -> void {
  auto& time = native_time();
  time.delta_time = static_cast<f32>(timestep.get_seconds());
  time.elapsed += timestep.get_seconds();
  time.frame++;

  tick_network(timestep);
}

auto DotNetScripting::load(this DotNetScripting& self, const DotNetLoadInfo& info, const OxNativeApi& native_api)
  -> std::expected<void, std::string> {
  ZoneScoped;

  self.unload();

  // size starts at zero so the check below proves managed code actually filled it in
  auto managed_api = OxManagedApi{.size = 0};
  auto result = info.kind == DotNetHostKind::HostFxr
                  ? hostfxr_load(info.path, info.scripting_directory, native_api, managed_api, info.abi_version)
                  : aot_load(info.path, native_api, managed_api, info.abi_version);
  if (!result) {
    return result;
  }

  if (!is_complete(managed_api)) {
    return std::unexpected(fmt::format("{} filled in a malformed OxManagedApi", to_utf8(info.path)));
  }

  self.managed_api = managed_api;
  set_managed_api(&self.managed_api);
  subscribe_window_events();

  const auto type_count = managed_api.list_script_types(nullptr, 0);
  auto type_infos = std::vector<OxScriptTypeInfo>(static_cast<usize>(std::max(type_count, 0)));
  managed_api.list_script_types(type_infos.data(), static_cast<i32>(type_infos.size()));
  for (const auto& type_info : type_infos) {
    self.script_types.push_back(
      {.name = std::string(type_info.name.data, type_info.name.length), .kind = type_info.kind}
    );
  }

  std::ranges::sort(self.script_types, {}, &DotNetScriptType::name);

  for (auto& attached : self.attached_worlds) {
    if (auto attach_result = attach_managed(self.managed_api, attached); !attach_result) {
      OX_LOG_ERROR("{}", attach_result.error());
      continue;
    }

    for (auto* script : attached.scripts) {
      create_scene_script(self.managed_api, attached.world, *script);
    }

    if (attached.is_running) {
      create_behaviours(self.managed_api, attached.world);
      self.managed_api.world_start(attached.world);
    }
  }

  return {};
}

auto DotNetScripting::unload(this DotNetScripting& self) -> void {
  ZoneScoped;

  if (!self.is_loaded()) {
    return;
  }

  // stopped and torn down the way a scene stop and a script removal would, so the scripts see it happen. Running
  // worlds stay marked running and are started again by the next load
  for (auto& attached : self.attached_worlds) {
    if (attached.is_running) {
      self.managed_api.world_stop(attached.world);
    }

    for (auto* script : attached.scripts) {
      if (script->handle != 0) {
        self.managed_api.script_destroy(script->handle);
        script->handle = 0;
      }
    }

    set_component_db(attached.world, nullptr);
  }

  // the scripts' listeners, data models, servers and clients call into managed code, so they go before it does
  release_ui();
  release_network();
  release_window_events();

  // shutdown detaches every world on the managed side, which drops their systems
  if (const auto status = self.managed_api.shutdown(); status != OxStatus::Ok) {
    OX_LOG_ERROR("Managed scripts didn't shut down cleanly: {}", status_message(status));
  }

  set_managed_api(nullptr);
  self.managed_api = {};
  self.script_types.clear();
}

auto DotNetScripting::is_loaded(this const DotNetScripting& self) -> bool {
  return self.managed_api.shutdown != nullptr;
}

auto DotNetScripting::attach_world(this DotNetScripting& self, ecs_world_t* world, ComponentDB* component_db)
  -> std::expected<void, std::string> {
  ZoneScoped;

  if (self.find_world(world)) {
    return {};
  }

  self.attached_worlds.push_back({.world = world, .component_db = component_db});
  if (!self.is_loaded()) {
    return {};
  }

  return attach_managed(self.managed_api, self.attached_worlds.back());
}

auto DotNetScripting::detach_world(this DotNetScripting& self, ecs_world_t* world) -> void {
  ZoneScoped;

  const auto it = std::ranges::find(self.attached_worlds, world, &AttachedWorld::world);
  if (it == self.attached_worlds.end()) {
    return;
  }

  self.attached_worlds.erase(it);
  release_ui(world);
  ox::set_ui_context(world, nullptr);
  if (!self.is_loaded()) {
    return;
  }

  if (const auto status = self.managed_api.detach_world(world); status != OxStatus::Ok) {
    OX_LOG_ERROR("Couldn't detach C# scripts from a world: {}", status_message(status));
  }

  set_component_db(world, nullptr);
}

auto DotNetScripting::set_ui_context(this DotNetScripting&, ecs_world_t* world, Rml::Context* context) -> void {
  ox::set_ui_context(world, context);
}

auto DotNetScripting::find_world(this DotNetScripting& self, ecs_world_t* world) -> AttachedWorld* {
  const auto it = std::ranges::find(self.attached_worlds, world, &AttachedWorld::world);
  return it == self.attached_worlds.end() ? nullptr : &*it;
}

auto DotNetScripting::start_world(this DotNetScripting& self, ecs_world_t* world) -> void {
  ZoneScoped;

  auto* attached = self.find_world(world);
  if (!attached || attached->is_running) {
    return;
  }

  attached->is_running = true;
  if (!self.is_loaded()) {
    return;
  }

  create_behaviours(self.managed_api, world);
  self.managed_api.world_start(world);
}

auto DotNetScripting::stop_world(this DotNetScripting& self, ecs_world_t* world) -> void {
  ZoneScoped;

  auto* attached = self.find_world(world);
  if (!attached || !attached->is_running) {
    return;
  }

  if (self.is_loaded()) {
    self.managed_api.world_stop(world);
  }

  attached->is_running = false;
}

auto DotNetScripting::add_scene_script(this DotNetScripting& self, ecs_world_t* world, ManagedSceneScript& script)
  -> void {
  ZoneScoped;

  auto* attached = self.find_world(world);
  if (!attached) {
    OX_LOG_ERROR("C# scene behaviour {} was added to a world the scripts aren't attached to", script.type_name);
    return;
  }

  attached->scripts.push_back(&script);
  if (self.is_loaded()) {
    create_scene_script(self.managed_api, world, script);
  }
}

auto DotNetScripting::remove_scene_script(this DotNetScripting& self, ecs_world_t* world, ManagedSceneScript& script)
  -> void {
  ZoneScoped;

  if (script.handle != 0 && self.is_loaded()) {
    self.managed_api.script_destroy(script.handle);
  }

  script.handle = 0;
  auto* attached = self.find_world(world);
  if (!attached) {
    return;
  }

  if (const auto it = std::ranges::find(attached->scripts, &script); it != attached->scripts.end()) {
    attached->scripts.erase(it);
  }
}
} // namespace ox
