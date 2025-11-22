#pragma once

#include <filesystem>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <span>
#include <vulkan/vulkan_core.h>

#include "Core/Enum.hpp"
#include "Core/Handle.hpp"
#include "Core/Option.hpp"
#include "Utils/Timestep.hpp"

namespace ox {
enum class WindowCursor {
  ForceRedraw, // Force cursor to be redrawn
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
  Progress,
  Wait,

  Count,
};

enum class WindowFlag : u32 {
  None = 0,
  Centered = 1 << 0,
  Resizable = 1 << 1,
  Borderless = 1 << 2,
  Maximized = 1 << 3,
  WorkAreaRelative = 1 << 4, // Width and height of the window will be relative to available work area size
  HighPixelDensity = 1 << 5,
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
  void (*on_key)(void* user_data, u32 key_code, u32 scan_code, u16 mods, bool down, bool repeat) = nullptr;
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
  std::filesystem::path default_path = {};
  std::span<FileDialogFilter> filters = {};
  bool multi_select = false;
};

struct WindowInfo {
  struct Icon {
    struct Loaded {
      void* data = nullptr;
      u32 width = 0;
      u32 height = 0;
    };
    option<Loaded> loaded = nullopt;

    option<std::string> path = nullopt;
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
  static auto create(const WindowInfo& info) -> Window;
  auto destroy() const -> void;

  auto update(const Timestep& timestep) -> void;
  auto poll(const WindowCallbacks& callbacks) const -> void;

  static auto display_at(u32 monitor_id = WindowInfo::USE_PRIMARY_MONITOR) -> option<SystemDisplay>;

  auto show_dialog(const ShowDialogInfo& info) const -> void;

  auto set_cursor(WindowCursor cursor) const -> void;
  auto set_cursor_override(WindowCursor cursor) const -> void;
  auto get_cursor() const -> WindowCursor;
  auto show_cursor(bool show) const -> void;

  auto get_surface(VkInstance instance) const -> VkSurfaceKHR;

  auto get_size_in_pixels() const -> glm::ivec2;
  auto get_logical_width() const -> u32;
  auto get_logical_height() const -> u32;

  auto get_handle() const -> void*;

  auto get_display_content_scale() const -> f32;
  auto get_window_content_scale() const -> f32;

  auto get_refresh_rate() const -> f32;
};
} // namespace ox
