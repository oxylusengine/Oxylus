#pragma once

#include <ankerl/unordered_dense.h>
#include <charconv>
#include <functional>
#include <imgui.h>

#include "Utils/CVars.hpp"
#include "Utils/Log.hpp"

namespace ox {
class RuntimeConsole {
public:
  struct ParsedCommandValue {
    std::string str_value;

    explicit ParsedCommandValue(std::string str) noexcept : str_value(std::move(str)) {}

    const std::string& as_string() const noexcept { return str_value; }

    template <typename T = i32>
    [[nodiscard]]
    std::optional<T> as() const noexcept {
      if constexpr (std::is_same_v<T, std::string>) {
        return str_value;
      } else if constexpr (std::is_same_v<T, bool>) {
        if (str_value == "true")
          return true;
        if (str_value == "false")
          return false;
      }

      T value{};
      const auto* begin = str_value.data();
      const auto* end = begin + str_value.size();

      auto [ptr, ec] = std::from_chars(begin, end, value);

      if (ec == std::errc{} && ptr == end) {
        return value;
      }

      return std::nullopt;
    }

    explicit operator std::string() const noexcept { return str_value; }
    explicit operator std::string_view() const noexcept { return str_value; }
  };

  bool set_focus_to_keyboard_always = false;
  const char* panel_name = "RuntimeConsole";
  bool visible = false;
  std::string id = {};

  RuntimeConsole();
  ~RuntimeConsole();

  auto register_command(
    const std::string& command,
    const std::string& on_succes_log,
    const std::function<void(const ParsedCommandValue& value)>& action
  ) -> void;
  auto register_command(const std::string& command, const std::string& on_succes_log, i32* value) -> void;
  auto register_command(const std::string& command, const std::string& on_succes_log, std::string* value) -> void;
  auto register_command(const std::string& command, const std::string& on_succes_log, bool* value) -> void;

  auto add_log(const char* fmt, loguru::Verbosity verb) -> void;
  auto clear_log() -> void;

  auto set_scene_cvar_system(this RuntimeConsole& self, CVarSystem* system) -> void { self.scene_cvar_system = system; }

  auto render(this RuntimeConsole& self) -> void;

private:
  struct ConsoleText {
    std::string text = {};
    loguru::Verbosity verbosity = {};
  };

  void render_console_text(const std::string& text, i32 id, loguru::Verbosity verb);

  struct ConsoleCommand {
    i32* int_value = nullptr;
    std::string* str_value = nullptr;
    bool* bool_value = nullptr;
    std::function<void(const ParsedCommandValue& value)> action = nullptr;
    std::string on_succes_log = {};
  };

  CVarSystem* scene_cvar_system = nullptr;

  // Commands
  ankerl::unordered_dense::map<std::string, ConsoleCommand> command_map;
  auto process_command(this RuntimeConsole& self, const std::string& command) -> void;

  auto help_command(this RuntimeConsole& self, const ParsedCommandValue& value) -> void;
  auto get_available_commands(this RuntimeConsole& self) -> std::vector<std::string>;

  auto parse_value(const std::string& command) -> ParsedCommandValue;
  auto parse_command(const std::string& command) -> std::string;

  // Input field
  static constexpr uint32_t MAX_TEXT_BUFFER_SIZE = 32;
  i32 history_position = -1;
  std::vector<ConsoleText> text_buffer = {};
  std::vector<std::string> input_log = {};
  bool request_scroll_to_bottom = true;
  bool request_keyboard_focus = true;
  bool auto_scroll = true;
  i32 input_text_callback(ImGuiInputTextCallbackData* data);

  loguru::Verbosity text_filter = loguru::Verbosity_OFF;
  f32 animation_counter = 0.0f;
};
} // namespace ox
