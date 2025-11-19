#include "Scripting/LuaApplicationBindings.hpp"

#include <sol/state.hpp>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Core/Input.hpp"
#include "Networking/NetworkManager.hpp"
#include "Physics/Physics.hpp"
#include "Render/RendererConfig.hpp"
#include "Scripting/LuaHelpers.hpp"
#include "Scripting/LuaManager.hpp"

namespace ox {
#define APP_MOD(m) if (App::has_mod<m>()) mod_table.set(#m, std::ref(App::mod<m>()))

class LuaScopedSubscription {
public:
  std::function<void()> unsubscriber;
  bool active = true;

  LuaScopedSubscription(std::function<void()> unsub) : unsubscriber(std::move(unsub)) {}

  ~LuaScopedSubscription() { unsubscribe(); }

  LuaScopedSubscription(LuaScopedSubscription&& other) noexcept
      : unsubscriber(std::move(other.unsubscriber)),
        active(other.active) {

    // Disarm the 'other' (temporary) object so its
    // destructor does nothing.
    other.active = false;
    other.unsubscriber = nullptr;
  }

  LuaScopedSubscription& operator=(LuaScopedSubscription&& other) noexcept {
    if (this != &other) {
      unsubscribe();

      // Move the new handle's data
      unsubscriber = std::move(other.unsubscriber);
      active = other.active;

      // Disarm the 'other' object
      other.active = false;
      other.unsubscriber = nullptr;
    }
    return *this;
  }

  LuaScopedSubscription(const LuaScopedSubscription&) = delete;
  LuaScopedSubscription& operator=(const LuaScopedSubscription&) = delete;

  void unsubscribe() {
    if (active && unsubscriber) {
      unsubscriber();
    }
    active = false;
    unsubscriber = nullptr;
  }

  bool is_active() const { return active && (unsubscriber != nullptr); }
};

template <Event EventType>
sol::object lua_subscribe_helper(EventSystem& system, sol::function callback) {
  sol::state_view lua = callback.lua_state();

  if (!callback.valid()) {
    OX_LOG_ERROR("Lua event subscription failed: callback function is nil or invalid.");
    return sol::make_object(lua, sol::lua_nil);
  }

  auto handler = [callback](const EventType& event) {
    auto result = callback(event);

    if (!result.valid()) {
      sol::error err = result;
      OX_LOG_ERROR("Lua event handler failed: {}", err.what());
    }
  };

  auto scoped_sub_option = make_scoped_subscription<EventType>(
    system,
    std::function<void(const EventType&)>(std::move(handler))
  );

  if (scoped_sub_option) {
    auto sub_ptr = std::make_shared<ScopedSubscription<EventType>>(std::move(*scoped_sub_option));

    std::function<void()> unsub_func = [sub_ptr]() {
      sub_ptr->unsubscribe();
    };

    return sol::make_object(lua, LuaScopedSubscription(std::move(unsub_func)));
  }

  return sol::make_object(lua, sol::lua_nil);
}

auto AppBinding::bind(sol::state* state) -> void {
  auto app = state->create_table("App");

  auto mod_table = state->create_table("Mod");
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
  SET_TYPE_FUNCTION(app, App, get_event_system);

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

  state->new_usertype<WindowResizeEvent>(
    "WindowResizeEvent",
    sol::call_constructor,
    sol::no_constructor,
    "width",
    &WindowResizeEvent::width,
    "height",
    &WindowResizeEvent::height
  );

  state->new_usertype<LuaScopedSubscription>(
    "ScopedSubscription",
    sol::call_constructor,
    sol::no_constructor,
    "unsubscribe",
    &LuaScopedSubscription::unsubscribe,
    "is_active",
    &LuaScopedSubscription::is_active,
    "__tostring",
    [](const LuaScopedSubscription& self) {
      return self.is_active() ? "ScopedSubscription(active)" : "ScopedSubscription(inactive)";
    }
  );

  state->new_usertype<EventSystem>(
    "EventSystem",

    "subscribe_window_resize_event",
    &lua_subscribe_helper<WindowResizeEvent>
  );
}
} // namespace ox
