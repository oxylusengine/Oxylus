#include "Scripting/LuaApplicationBindings.hpp"

#include <sol/state.hpp>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Core/EventSystem.hpp"
#include "Core/Input.hpp"
#include "Core/JobManager.hpp"
#include "Networking/NetworkManager.hpp"
#include "Physics/Physics.hpp"
#include "Render/RendererConfig.hpp"
#include "Scripting/LuaHelpers.hpp"
#include "Scripting/LuaManager.hpp"

namespace ox {
#define APP_MOD(m) mod_table.set(#m, std::ref(App::mod<m>()))

auto AppBinding::bind(sol::state* state) -> void {
  auto app = state->create_table("App");

  auto mod_table = state->create_table("Mod");
  APP_MOD(EventSystem);
  APP_MOD(JobManager);
  APP_MOD(AssetManager);
  APP_MOD(AudioEngine);
  APP_MOD(LuaManager);
  APP_MOD(Renderer);
  APP_MOD(RendererConfig);
  APP_MOD(Physics);
  APP_MOD(Input);
  APP_MOD(NetworkManager);
  APP_MOD(DebugRenderer);
  app.set("mod", mod_table);

  SET_TYPE_FUNCTION(app, App, get_vfs);

  auto timestep = state->new_usertype<Timestep>(
    "Timestep",

    "get_millis",
    [](const Timestep* ts) { return ts->get_millis(); },

    "get_elapsed_millis",
    [](const Timestep* ts) { return ts->get_elapsed_millis(); },

    "get_seconds",
    [](const Timestep* ts) { return ts->get_seconds(); },

    "get_elapsed_seconds",
    [](const Timestep* ts) { return ts->get_elapsed_seconds(); },

    "get_max_frame_time",
    [](const Timestep* ts) { return ts->get_max_frame_time(); },

    "set_max_frame_time",
    [](Timestep* ts, f64 value) { return ts->set_max_frame_time(value); },

    "reset_max_frame_time",
    [](Timestep* ts) { return ts->reset_max_frame_time(); }
  );

  app.set_function("get_timestep", []() -> const Timestep* { return &App::get_timestep(); });
}
} // namespace ox
