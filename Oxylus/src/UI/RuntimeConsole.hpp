#pragma once

#include <charconv>
#include <imgui.h>

namespace ox {
class RuntimeConsole {
public:
  struct ParsedCommandValue {
    std::string str_value;

    explicit ParsedCommandValue(std::string str) noexcept : str_value(std::move(str)) {}

    const std::string& as_string() const noexcept { return str_value; }

    template <typename T = int32_t>
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

  void register_command(const std::string& command,
                        const std::string& on_succes_log,
                        const std::function<void(const ParsedCommandValue& value)>& action);
  void register_command(const std::string& command, const std::string& on_succes_log, int32_t* value);
  void register_command(const std::string& command, const std::string& on_succes_log, std::string* value);
  void register_command(const std::string& command, const std::string& on_succes_log, bool* value);

  void add_log(const char* fmt, loguru::Verbosity verb);
  void clear_log();

  void on_imgui_render();

private:
  struct ConsoleText {
    std::string text = {};
    loguru::Verbosity verbosity = {};
  };

  void render_console_text(const std::string& text, int32_t id, loguru::Verbosity verb);

  struct ConsoleCommand {
    int32_t* int_value = nullptr;
    std::string* str_value = nullptr;
    bool* bool_value = nullptr;
    std::function<void(const ParsedCommandValue& value)> action = nullptr;
    std::string on_succes_log = {};
  };

  // Commands
  ankerl::unordered_dense::map<std::string, ConsoleCommand> command_map;
  void process_command(const std::string& command);

  void help_command(const ParsedCommandValue& value);
  std::vector<std::string> get_available_commands();

  ParsedCommandValue parse_value(const std::string& command);
  std::string parse_command(const std::string& command);

  // Input field
  static constexpr uint32_t MAX_TEXT_BUFFER_SIZE = 32;
  int32_t history_position = -1;
  std::vector<ConsoleText> text_buffer = {};
  std::vector<std::string> input_log = {};
  bool request_scroll_to_bottom = true;
  bool request_keyboard_focus = true;
  bool auto_scroll = true;
  int input_text_callback(ImGuiInputTextCallbackData* data);

  loguru::Verbosity text_filter = loguru::Verbosity_OFF;
  float animation_counter = 0.0f;
};
} // namespace ox
