#pragma once

#include <SDL3/SDL_keycode.h>
#include <vulkan/vulkan_core.h>

#include "Core/Handle.hpp"
#include "Core/Option.hpp"

namespace ox {
enum class WindowCursor {
  Arrow,
  TextInput,
  ResizeAll,
  ResizeNS,
  ResizeEW,
  ResizeNESW,
  ResizeNWSE,
  Hand,
  NotAllowed,
  Crosshair,

  Count,
};

enum class WindowFlag : u32 {
  None = 0,
  Centered = 1 << 0,
  Resizable = 1 << 1,
  Borderless = 1 << 2,
  Maximized = 1 << 3,
  WorkAreaRelative = 1 << 4, // Width and height of the window will be relative to available work area size
};
consteval void enable_bitmask(WindowFlag);

struct SystemDisplay {
  std::string name = {};

  glm::ivec2 position = {};
  glm::ivec4 work_area = {};
  glm::ivec2 resolution = {};
  f32 refresh_rate = 30.0f;
  f32 content_scale = 1.0f;
};

struct WindowCallbacks {
  void* user_data = nullptr;
  void (*on_resize)(void* user_data, glm::uvec2 size) = nullptr;
  void (*on_mouse_pos)(void* user_data, glm::vec2 position, glm::vec2 relative) = nullptr;
  void (*on_mouse_button)(void* user_data, u8 button, bool down) = nullptr;
  void (*on_mouse_scroll)(void* user_data, glm::vec2 offset) = nullptr;
  void (*on_text_input)(void* user_data, const c8* text) = nullptr;
  void (*on_key)(void* user_data, SDL_Keycode key_code, SDL_Scancode scan_code, u16 mods, bool down, bool repeat) =
      nullptr;
  void (*on_close)(void* user_data) = nullptr;
};

enum class DialogKind : u32 {
  OpenFile = 0,
  SaveFile,
  OpenFolder,
};

struct FileDialogFilter {
  std::string_view name = {};
  std::string_view pattern = {};
};

struct ShowDialogInfo {
  DialogKind kind = DialogKind::OpenFile;
  void* user_data = nullptr;
  void (*callback)(void* user_data, const c8* const* files, i32 filter) = nullptr;
  std::string_view title = {};
  ::fs::path default_path = {};
  std::span<FileDialogFilter> filters = {};
  bool multi_select = false;
};

struct WindowInfo {
  // fill either data and data_length or just path
  struct Icon {
    u8* data = nullptr;
    u32 data_length = 0;

    std::string path = {};
  };

  constexpr static i32 USE_PRIMARY_MONITOR = 0;

  std::string title = {};
  Icon icon = {};
  i32 monitor = USE_PRIMARY_MONITOR;
  u32 width = 0;
  u32 height = 0;
  WindowFlag flags = WindowFlag::None;
};

struct Window : Handle<Window> {
  static Window create(const WindowInfo& info);
  void destroy() const;

  void poll(const WindowCallbacks& callbacks) const;

  static option<SystemDisplay> display_at(u32 monitor_id = WindowInfo::USE_PRIMARY_MONITOR);

  void show_dialog(const ShowDialogInfo& info) const;

  void set_cursor(WindowCursor cursor) const;
  WindowCursor get_cursor() const;
  void show_cursor(bool show) const;

  VkSurfaceKHR get_surface(VkInstance instance) const;

  u32 get_width() const;
  u32 get_height() const;

  void* get_handle() const;

  float get_content_scale() const;

  float get_refresh_rate() const;

  void set_mouse_position(glm::vec2 position) const;
};
} // namespace ox
