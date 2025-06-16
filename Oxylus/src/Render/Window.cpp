#include "Window.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <array>
#include <ranges>
#include <stb_image.h>

#include "Core/Handle.hpp"
#include "Memory/Stack.hpp"
#include "Utils/Log.hpp"

namespace ox {
template <>
struct Handle<Window>::Impl {
  u32 width = {};
  u32 height = {};

  WindowCursor current_cursor = WindowCursor::Arrow;
  glm::uvec2 cursor_position = {};

  SDL_Window* handle = nullptr;
  u32 monitor_id = {};
  std::array<SDL_Cursor*, static_cast<usize>(WindowCursor::Count)> cursors = {};
  f32 content_scale = {};
  f32 refresh_rate = {};
};

Window Window::create(const WindowInfo& info) {
  ZoneScoped;

  if (!SDL_Init(SDL_INIT_EVENTS | SDL_INIT_VIDEO)) {
    OX_LOG_ERROR("Failed to initialize SDL! {}", SDL_GetError());
    return Handle(nullptr);
  }

  const auto display = display_at(info.monitor);
  if (!display.has_value()) {
    OX_LOG_ERROR("No available displays!");
    return Handle(nullptr);
  }

  i32 new_pos_y = SDL_WINDOWPOS_UNDEFINED;
  i32 new_pos_x = SDL_WINDOWPOS_UNDEFINED;
  i32 new_width = static_cast<i32>(info.width);
  i32 new_height = static_cast<i32>(info.height);

  if (info.flags & WindowFlag::WorkAreaRelative) {
    new_pos_x = display->work_area.x;
    new_pos_y = display->work_area.y;
    new_width = display->work_area.z;
    new_height = display->work_area.w;
  } else if (info.flags & WindowFlag::Centered) {
    new_pos_x = SDL_WINDOWPOS_CENTERED;
    new_pos_y = SDL_WINDOWPOS_CENTERED;
  }

  u32 window_flags = SDL_WINDOW_VULKAN;
  if (info.flags & WindowFlag::Resizable) {
    window_flags |= SDL_WINDOW_RESIZABLE;
  }

  if (info.flags & WindowFlag::Borderless) {
    window_flags |= SDL_WINDOW_BORDERLESS;
  }

  if (info.flags & WindowFlag::Maximized) {
    window_flags |= SDL_WINDOW_MAXIMIZED;
  }

  const auto impl = new Impl;
  impl->width = static_cast<u32>(new_width);
  impl->height = static_cast<u32>(new_height);
  impl->monitor_id = info.monitor;
  impl->content_scale = display->content_scale;
  impl->refresh_rate = display->refresh_rate;

  const auto window_properties = SDL_CreateProperties();
  SDL_SetStringProperty(window_properties, SDL_PROP_WINDOW_CREATE_TITLE_STRING, info.title.c_str());
  SDL_SetNumberProperty(window_properties, SDL_PROP_WINDOW_CREATE_X_NUMBER, new_pos_x);
  SDL_SetNumberProperty(window_properties, SDL_PROP_WINDOW_CREATE_Y_NUMBER, new_pos_y);
  SDL_SetNumberProperty(window_properties, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, new_width);
  SDL_SetNumberProperty(window_properties, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, new_height);
  SDL_SetNumberProperty(window_properties, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, window_flags);
  impl->handle = SDL_CreateWindowWithProperties(window_properties);
  SDL_DestroyProperties(window_properties);

  impl->cursors = {
      SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT),
      SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT),
      SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_MOVE),
      SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NS_RESIZE),
      SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_EW_RESIZE),
      SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NESW_RESIZE),
      SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NWSE_RESIZE),
      SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER),
      SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NOT_ALLOWED),
      SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR),
  };

  void* image_data = nullptr;
  int width = {}, height = {}, channels = {};
  if (info.icon.data != nullptr && info.icon.data_length > 0) {
    image_data = stbi_load_from_memory(
        info.icon.data, static_cast<int>(info.icon.data_length), &width, &height, &channels, 4);
  } else if (!info.icon.path.empty()) {
    image_data = stbi_load(info.icon.path.c_str(), &width, &height, &channels, 4);
  }
  if (image_data != nullptr) {
    const auto surface = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_ABGR8888, image_data, width * 4);
    if (!SDL_SetWindowIcon(impl->handle, surface)) {
      OX_LOG_ERROR("Couldn't set window icon!");
    }
    SDL_DestroySurface(surface);
    stbi_image_free(image_data);
  }

  i32 real_width;
  i32 real_height;
  SDL_GetWindowSizeInPixels(impl->handle, &real_width, &real_height);
  SDL_StartTextInput(impl->handle);

  impl->width = real_width;
  impl->height = real_height;

  const auto self = Window(impl);
  self.set_cursor(WindowCursor::Arrow);
  return self;
}

void Window::destroy() const {
  ZoneScoped;

  SDL_StopTextInput(impl->handle);
  SDL_DestroyWindow(impl->handle);
}

void Window::poll(const WindowCallbacks& callbacks) const {
  ZoneScoped;

  SDL_Event e = {};
  while (SDL_PollEvent(&e) != 0) {
    switch (e.type) {
      case SDL_EVENT_WINDOW_RESIZED: {
        if (callbacks.on_resize) {
          callbacks.on_resize(callbacks.user_data, {e.window.data1, e.window.data2});
        }
      } break;
      case SDL_EVENT_MOUSE_MOTION: {
        if (callbacks.on_mouse_pos) {
          callbacks.on_mouse_pos(callbacks.user_data, {e.motion.x, e.motion.y}, {e.motion.xrel, e.motion.yrel});
        }
      } break;
      case SDL_EVENT_MOUSE_BUTTON_DOWN:
      case SDL_EVENT_MOUSE_BUTTON_UP  : {
        if (callbacks.on_mouse_button) {
          const auto state = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
          callbacks.on_mouse_button(callbacks.user_data, e.button.button, state);
        }
      } break;
      case SDL_EVENT_MOUSE_WHEEL: {
        if (callbacks.on_mouse_scroll) {
          callbacks.on_mouse_scroll(callbacks.user_data, {e.wheel.x, e.wheel.y});
        }
      } break;
      case SDL_EVENT_KEY_DOWN:
      case SDL_EVENT_KEY_UP  : {
        if (callbacks.on_key) {
          const auto state = e.type == SDL_EVENT_KEY_DOWN;
          callbacks.on_key(callbacks.user_data, e.key.key, e.key.scancode, e.key.mod, state, e.key.repeat);
        }
      } break;
      case SDL_EVENT_TEXT_INPUT: {
        if (callbacks.on_text_input) {
          callbacks.on_text_input(callbacks.user_data, e.text.text);
        }
      } break;
      case SDL_EVENT_QUIT: {
        if (callbacks.on_close) {
          callbacks.on_close(callbacks.user_data);
        }
      } break;
      default:;
    }
  }
}

option<SystemDisplay> Window::display_at(const u32 monitor_id) {
  i32 display_count = 0;
  auto* display_ids = SDL_GetDisplays(&display_count);
  OX_DEFER(&) { SDL_free(display_ids); };

  if (display_count == 0 || display_ids == nullptr) {
    return nullopt;
  }

  const auto checking_display = display_ids[monitor_id];
  const char* monitor_name = SDL_GetDisplayName(checking_display);
  const auto* display_mode = SDL_GetDesktopDisplayMode(checking_display);
  if (display_mode == nullptr) {
    return nullopt;
  }

  SDL_Rect position_bounds = {};
  if (!SDL_GetDisplayBounds(checking_display, &position_bounds)) {
    return nullopt;
  }

  SDL_Rect work_bounds = {};
  if (!SDL_GetDisplayUsableBounds(checking_display, &work_bounds)) {
    return nullopt;
  }

  const auto scale = SDL_GetDisplayContentScale(display_ids[monitor_id]);
  if (scale == 0) {
    OX_LOG_ERROR("{}", SDL_GetError());
  }

  return SystemDisplay{
      .name = monitor_name,
      .position = {position_bounds.x, position_bounds.y},
      .work_area = {work_bounds.x, work_bounds.y, work_bounds.w, work_bounds.h},
      .resolution = {display_mode->w, display_mode->h},
      .refresh_rate = display_mode->refresh_rate,
      .content_scale = scale,
  };
}

void Window::show_dialog(const ShowDialogInfo& info) const {
  memory::ScopedStack stack;

  auto sdl_filters = stack.alloc<SDL_DialogFileFilter>(info.filters.size());
  for (const auto& [filter, sdl_filter] : std::views::zip(info.filters, sdl_filters)) {
    sdl_filter.name = stack.null_terminate_cstr(filter.name);
    sdl_filter.pattern = stack.null_terminate_cstr(filter.pattern);
  }

  const auto props = SDL_CreateProperties();
  OX_DEFER(&) { SDL_DestroyProperties(props); };

  SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, sdl_filters.data());
  SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, static_cast<i32>(sdl_filters.size()));
  SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, impl->handle);
  SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, info.default_path.string().c_str());
  SDL_SetBooleanProperty(props, SDL_PROP_FILE_DIALOG_MANY_BOOLEAN, info.multi_select);
  SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, stack.null_terminate_cstr(info.title));

  switch (info.kind) {
    case DialogKind::OpenFile:
      SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFILE, info.callback, info.user_data, props);
      break;
    case DialogKind::SaveFile:
      SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_SAVEFILE, info.callback, info.user_data, props);
      break;
    case DialogKind::OpenFolder:
      SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFOLDER, info.callback, info.user_data, props);
      break;
  }
}

void Window::set_cursor(WindowCursor cursor) const {
  ZoneScoped;

  impl->current_cursor = cursor;
  SDL_SetCursor(impl->cursors[static_cast<usize>(cursor)]);
}

WindowCursor Window::get_cursor() const { return impl->current_cursor; }

void Window::show_cursor(bool show) const {
  ZoneScoped;
  show ? SDL_ShowCursor() : SDL_HideCursor();
}

VkSurfaceKHR Window::get_surface(VkInstance instance) const {
  VkSurfaceKHR surface = {};
  if (!SDL_Vulkan_CreateSurface(impl->handle, instance, nullptr, &surface)) {
    OX_LOG_ERROR("{}", SDL_GetError());
    return nullptr;
  }
  return surface;
}

u32 Window::get_width() const { return impl->width; }

u32 Window::get_height() const { return impl->height; }

void* Window::get_handle() const { return impl->handle; }

float Window::get_content_scale() const { return impl->content_scale; }

float Window::get_refresh_rate() const { return impl->refresh_rate; }

void Window::set_mouse_position(const glm::vec2 position) const {
  ZoneScoped;
  SDL_WarpMouseInWindow(impl->handle, position.x, position.y);
}
} // namespace ox
