#include <utility>

#include "Core/App.hpp"
#include "DotNetHosts.hpp"
#include "Render/Window.hpp"
#include "Utils/Timestep.hpp"

namespace ox {
static auto resize_handler = HandlerId{};
static auto is_subscribed = false;

static auto window_size(OxWindowSize* size) -> void {
  *size = {};
  if (!App::get() || !App::has_window()) {
    return;
  }

  const auto& window = App::get_window();
  const auto pixels = window.get_size_in_pixels();
  const auto logical = window.get_logical_size();
  *size = OxWindowSize{.pixels_x = pixels.x, .pixels_y = pixels.y, .logical_x = logical.x, .logical_y = logical.y};
}

static auto time_max_frame_time() -> f64 { return App::get() ? App::get_timestep().get_max_frame_time() : -1.0; }

// the app owns its timestep mutable, only the accessor hands it out const
static auto time_set_max_frame_time(f64 millis) -> void {
  if (App::get()) {
    const_cast<Timestep&>(App::get_timestep()).set_max_frame_time(millis);
  }
}

static auto time_reset_max_frame_time() -> void {
  if (App::get()) {
    const_cast<Timestep&>(App::get_timestep()).reset_max_frame_time();
  }
}

static auto app_quit() -> void {
  if (App::get()) {
    App::get()->should_stop();
  }
}

auto subscribe_window_events() -> void {
  if (is_subscribed || !App::get()) {
    return;
  }

  const auto handler = App::get_event_system().subscribe<WindowResizeEvent>([](const WindowResizeEvent& event) {
    if (const auto* managed = managed_api()) {
      managed->window_resized(event.width, event.height);
    }
  });
  if (handler) {
    resize_handler = *handler;
    is_subscribed = true;
  }
}

auto release_window_events() -> void {
  if (!is_subscribed || !App::get()) {
    return;
  }

  std::ignore = App::get_event_system().unsubscribe<WindowResizeEvent>(resize_handler);
  is_subscribed = false;
}

auto fill_app_api(OxNativeApi& api) -> void {
  api.window_size = window_size;
  api.time_max_frame_time = time_max_frame_time;
  api.time_set_max_frame_time = time_set_max_frame_time;
  api.time_reset_max_frame_time = time_reset_max_frame_time;
  api.app_quit = app_quit;
}
} // namespace ox
