#include "UI/RuntimeConsole.hpp"

#include <misc/cpp/imgui_stdlib.h>

#include "Core/App.hpp"
#include "Utils/CVars.hpp"

namespace ox {
static ImVec4 get_color(const loguru::Verbosity verb) {
  switch (verb) {
    case loguru::Verbosity_INFO   : return {0, 1, 0, 1};
    case loguru::Verbosity_WARNING: return {0.9f, 0.6f, 0.2f, 1};
    case loguru::Verbosity_ERROR  : return {1, 0, 0, 1};
    default                       : return {1, 1, 1, 1};
  }
}

static const char* get_level_icon(const loguru::Verbosity level) {
  switch (level) {
    case loguru::Verbosity_INFO   : return "[I]";
    case loguru::Verbosity_WARNING: return "[W]";
    case loguru::Verbosity_ERROR  : return "[E]";
    default                       :;
  }

  return "?";
}

RuntimeConsole::RuntimeConsole() {
  Log::add_callback(
    "runtime_console",
    [](void* user_data, const loguru::Message& message) {
      const auto console = reinterpret_cast<RuntimeConsole*>(user_data);
      console->add_log(message.message, message.verbosity);
    },
    this,
    loguru::Verbosity_INFO
  );

  // Default commands
  register_command("quit", "", [](const ParsedCommandValue&) { App::get()->stop(); });
  register_command("clear", "", [this](const ParsedCommandValue&) { clear_log(); });
  register_command("help", "", [this](const ParsedCommandValue& value) { help_command(value); });

  request_scroll_to_bottom = true;
}

RuntimeConsole::~RuntimeConsole() { Log::remove_callback("runtime_console"); }

void RuntimeConsole::register_command(
  const std::string& command,
  const std::string& on_succes_log,
  const std::function<void(const ParsedCommandValue& value)>& action
) {
  command_map.emplace(command, ConsoleCommand{nullptr, nullptr, nullptr, action, on_succes_log});
}

void RuntimeConsole::register_command(const std::string& command, const std::string& on_succes_log, int32_t* value) {
  command_map.emplace(command, ConsoleCommand{value, nullptr, nullptr, nullptr, on_succes_log});
}

void RuntimeConsole::register_command(
  const std::string& command, const std::string& on_succes_log, std::string* value
) {
  command_map.emplace(command, ConsoleCommand{nullptr, value, nullptr, nullptr, on_succes_log});
}

void RuntimeConsole::register_command(const std::string& command, const std::string& on_succes_log, bool* value) {
  command_map.emplace(command, ConsoleCommand{nullptr, nullptr, value, nullptr, on_succes_log});
}

void RuntimeConsole::add_log(const char* fmt, loguru::Verbosity verb) {
  if ((uint32_t)text_buffer.size() >= MAX_TEXT_BUFFER_SIZE)
    text_buffer.erase(text_buffer.begin());
  text_buffer.emplace_back(fmt, verb);
  request_scroll_to_bottom = true;
}

void RuntimeConsole::clear_log() { text_buffer.clear(); }

void RuntimeConsole::render(this RuntimeConsole& self) {
  if (ImGui::IsKeyPressed(ImGuiKey_GraveAccent, false)) {
    self.visible = !self.visible;
    self.request_keyboard_focus = true;
  }
  if (self.visible) {
    constexpr auto animation_duration = 0.5f;
    constexpr auto animation_speed = 3.0f;

    self.animation_counter += (float)App::get_timestep().get_seconds() * animation_speed;
    self.animation_counter = std::clamp(self.animation_counter, 0.0f, animation_duration);

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->WorkPos, ImGuiCond_Always);
    ImVec2 size = {ImGui::GetMainViewport()->WorkSize.x, ImGui::GetMainViewport()->WorkSize.y * self.animation_counter};
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);

    constexpr ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoDecoration |
                                             ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoCollapse;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    // ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.000f, 0.000f, 0.000f, 1.000f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.000f, 0.000f, 0.000f, 0.784f));
    // ImGui::PushStyleColor(ImGuiCol_MenuBarBg, ImVec4(0.100f, 0.100f, 0.100f, 1.000f));

    self.id = fmt::format(" {} \t\t###", self.panel_name);
    if (ImGui::Begin(self.id.c_str(), nullptr, windowFlags)) {
      if (ImGui::BeginMenuBar()) {
        if (ImGui::MenuItem("Clear")) {
          self.clear_log();
        }
        if (
          ImGui::MenuItem(get_level_icon(loguru::Verbosity_INFO), nullptr, self.text_filter == loguru::Verbosity_INFO)
        ) {
          self.text_filter = self.text_filter == loguru::Verbosity_INFO ? loguru::Verbosity_OFF
                                                                        : loguru::Verbosity_INFO;
        }
        if (
          ImGui::MenuItem(
            get_level_icon(loguru::Verbosity_WARNING),
            nullptr,
            self.text_filter == loguru::Verbosity_WARNING
          )
        ) {
          self.text_filter = self.text_filter == loguru::Verbosity_WARNING ? loguru::Verbosity_OFF
                                                                           : loguru::Verbosity_WARNING;
        }
        if (
          ImGui::MenuItem(get_level_icon(loguru::Verbosity_ERROR), nullptr, self.text_filter == loguru::Verbosity_ERROR)
        ) {
          self.text_filter = self.text_filter == loguru::Verbosity_ERROR ? loguru::Verbosity_OFF
                                                                         : loguru::Verbosity_ERROR;
        }

        ImGui::EndMenuBar();
      }

      ImGui::Separator();

      float width = 0;
      if (ImGui::BeginChild("TextTable", ImVec2(0, -35))) {
        width = ImGui::GetWindowSize().x;
        // ImGui::PushFont(ImGuiLayer::bold_font);
        for (int32_t i = 0; i < (int32_t)self.text_buffer.size(); i++) {
          if (self.text_filter != loguru::Verbosity_OFF && self.text_filter != self.text_buffer[i].verbosity)
            continue;
          self.render_console_text(self.text_buffer[i].text, i, self.text_buffer[i].verbosity);
        }

        // ImGui::PopFont();
        if (self.request_scroll_to_bottom || (self.auto_scroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())) {
          ImGui::SetScrollHereY(1.0f);
          self.request_scroll_to_bottom = false;
        }
      }
      ImGui::EndChild();

      ImGui::Separator();
      ImGui::PushItemWidth(width);
      constexpr ImGuiInputTextFlags input_flags = ImGuiInputTextFlags_EnterReturnsTrue |
                                                  ImGuiInputTextFlags_CallbackHistory |
                                                  ImGuiInputTextFlags_CallbackCompletion |
                                                  ImGuiInputTextFlags_EscapeClearsAll;
      // ImGui::PushFont(ImGuiLayer::bold_font);

      auto callback = [](ImGuiInputTextCallbackData* data) {
        const auto panel = (RuntimeConsole*)data->UserData;
        return panel->input_text_callback(data);
      };

      if (self.request_keyboard_focus) {
        ImGui::SetKeyboardFocusHere();
        self.request_keyboard_focus = false;
      }
      std::string input_buf = {};
      if (ImGui::InputText("##", &input_buf, input_flags, callback, &self)) {
        self.history_position = -1;
        self.process_command(input_buf);
        self.input_log.emplace_back(input_buf);
        self.request_keyboard_focus = true;
      }

      // ImGui::PopFont();
      ImGui::PopItemWidth();
    }
    ImGui::End();

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(1);
  } else {
    self.animation_counter = 0.0f;
  }
}

void RuntimeConsole::render_console_text(const std::string& text_, const int32_t id_, loguru::Verbosity verb_) {
  ImGui::PushStyleColor(ImGuiCol_Text, get_color(verb_));
  const auto level_icon = get_level_icon(verb_);
  ImGui::TextWrapped("%s %s", level_icon, text_.c_str());
  ImGui::PopStyleColor();

  const auto sid = fmt::format("{}", id_);
  if (ImGui::BeginPopupContextItem(sid.c_str(), ImGuiPopupFlags_MouseButtonRight)) {
    if (ImGui::MenuItem("Copy"))
      ImGui::SetClipboardText(text_.c_str());

    ImGui::EndPopup();
  }
}

template <typename T>
void log_cvar_change(RuntimeConsole* console, const char* cvar_name, T current_value, bool changed) {
  const std::string log_text = changed ? fmt::format("Changed {} to {}", cvar_name, current_value)
                                       : fmt::format("{} {}", cvar_name, current_value);
  console->add_log(log_text.c_str(), loguru::Verbosity_INFO);
}

auto RuntimeConsole::process_command(this RuntimeConsole& self, const std::string& command) -> void {
  const auto parsed_command = self.parse_command(command);
  const auto value = self.parse_value(command);

  bool is_cvar_variable = false;

  const std::array cvar_systems = {
    &App::get_rendercontext().context_cvar.system,
    self.scene_cvar_system,
  };

  const std::hash<std::string> hasher = {};
  const auto hashed = hasher(parsed_command);

  CVarParameter* cvar = nullptr;
  CVarSystem* cvar_system = nullptr;
  for (auto* s : cvar_systems) {
    cvar = s->get_cvar(hashed);
    cvar_system = s;
    if (cvar) {
      break;
    }
  }

  if (cvar) {
    is_cvar_variable = true;
    switch (cvar->type) {
      case CVarType::INT: {
        auto current_value = cvar_system->int_cvars.at(cvar->array_index).current;
        bool changed = false;
        if (!value.str_value.empty()) {
          const auto parsed = value.as<int32_t>();
          if (parsed.has_value()) {
            cvar_system->set_int_cvar(hasher(cvar->name.c_str()), *parsed);
            current_value = *parsed;
            changed = true;
          }
        }
        log_cvar_change<int32_t>(&self, cvar->name.c_str(), current_value, changed);
        break;
      }
      case CVarType::FLOAT: {
        auto current_value = cvar_system->float_cvars.at(cvar->array_index).current;
        bool changed = false;
        if (!value.str_value.empty()) {
          const auto parsed = value.as<float>();
          if (parsed.has_value()) {
            cvar_system->set_float_cvar(hasher(cvar->name.c_str()), *parsed);
            current_value = *parsed;
            changed = true;
          }
        }
        log_cvar_change<float>(&self, cvar->name.c_str(), current_value, changed);
        break;
      }
      case CVarType::STRING: {
        auto current_value = cvar_system->string_cvars.at(cvar->array_index).current;
        bool changed = false;
        if (!value.str_value.empty()) {
          cvar_system->set_string_cvar(hasher(cvar->name.c_str()), value.str_value.c_str());
          current_value = value.str_value;
          changed = true;
        }
        log_cvar_change<std::string>(&self, cvar->name.c_str(), current_value, changed);
        break;
      }
    }
  }

  // commands registered with register_command()
  if (self.command_map.contains(parsed_command)) {
    const auto& c = self.command_map[parsed_command];
    if (c.action != nullptr) {
      c.action(value);
    }
    if (!value.str_value.empty()) {
      if (c.str_value != nullptr) {
        *c.str_value = value.str_value;
      } else if (c.int_value != nullptr) {
        const auto v = value.as<int32_t>();
        if (v.has_value()) {
          *c.int_value = v.value();
        }
      } else if (c.bool_value != nullptr) {
        const auto v = value.as<int32_t>();
        if (v.has_value()) {
          *c.bool_value = v.value();
        }
      }
    }
    if (!c.on_succes_log.empty())
      self.add_log(c.on_succes_log.c_str(), loguru::Verbosity_INFO);
  } else {
    if (!is_cvar_variable)
      self.add_log("Non existent command.", loguru::Verbosity_ERROR);
  }
}

RuntimeConsole::ParsedCommandValue RuntimeConsole::parse_value(const std::string& command) {
  if (command.find(' ') == std::string::npos)
    return RuntimeConsole::ParsedCommandValue("");
  const auto offset = command.find(' ');
  const auto value = command.substr(offset + 1, command.size() - offset);
  return RuntimeConsole::ParsedCommandValue(value);
}

std::string RuntimeConsole::parse_command(const std::string& command) { return command.substr(0, command.find(' ')); }

// cross-platform _strnicmp
int compare_ci(const std::string_view s1, const std::string_view s2, const size_t n) {
  const size_t len1 = std::min(s1.size(), n);
  const size_t len2 = std::min(s2.size(), n);

  for (size_t i = 0; i < std::min(len1, len2); ++i) {
    const char c1 = (char)std::tolower((unsigned char)s1[i]);
    const char c2 = (char)std::tolower((unsigned char)s2[i]);

    if (c1 != c2)
      return c1 - c2;
  }

  return (len1 < len2) ? -1 : (len1 > len2) ? 1 : 0;
}

int RuntimeConsole::input_text_callback(ImGuiInputTextCallbackData* data) {
  switch (data->EventFlag) {
    case ImGuiInputTextFlags_CallbackCompletion: {
      // Locate beginning of current word
      const char* word_end = data->Buf + data->CursorPos;
      const char* word_start = word_end;
      while (word_start > data->Buf) {
        const char c = word_start[-1];
        if (c == ' ' || c == '\t' || c == ',' || c == ';')
          break;
        word_start--;
      }

      const auto available_commands = get_available_commands();

      // Build a list of candidates
      std::vector<const char*> candidates;
      for (const auto& available_command : available_commands)
        if (compare_ci(available_command, word_start, (int)(word_end - word_start)) == 0)
          candidates.push_back(available_command.c_str());

      if (candidates.empty()) {
        add_log("No match", loguru::Verbosity_WARNING);
      } else if (candidates.size() == 1) {
        // Single match. Delete the beginning of the word and replace it entirely so we've got nice casing.
        data->DeleteChars((int)(word_start - data->Buf), (int)(word_end - word_start));
        data->InsertChars(data->CursorPos, candidates[0]);
        data->InsertChars(data->CursorPos, " ");
      } else {
        // Multiple matches. Complete as much as we can..
        // So inputing "C"+Tab will complete to "CL" then display "CLEAR" and "CLASSIFY" as matches.
        int match_len = (int)(word_end - word_start);
        for (;;) {
          int c = 0;
          bool all_candidates_matches = true;
          for (int i = 0; i < (int)candidates.size() && all_candidates_matches; i++)
            if (i == 0)
              c = toupper(candidates[i][match_len]);
            else if (c == 0 || c != toupper(candidates[i][match_len]))
              all_candidates_matches = false;
          if (!all_candidates_matches)
            break;
          match_len++;
        }

        if (match_len > 0) {
          data->DeleteChars((int)(word_start - data->Buf), (int)(word_end - word_start));
          data->InsertChars(data->CursorPos, candidates[0], candidates[0] + match_len);
        }

        // List matches
        std::string possible_matches = "Possible matches:\n";
        for (auto& candidate : candidates)
          possible_matches.append(fmt::format("  {} \n", candidate));
        add_log(possible_matches.c_str(), loguru::Verbosity_INFO);
      }
      break;
    }
    case ImGuiInputTextFlags_CallbackHistory: {
      const int prev_history_pos = history_position;
      if (data->EventKey == ImGuiKey_UpArrow) {
        if (history_position == -1)
          history_position = (int32_t)input_log.size() - 1;
        else if (history_position > 0)
          history_position--;
      } else if (data->EventKey == ImGuiKey_DownArrow) {
        if (history_position != -1)
          if (++history_position >= (int32_t)input_log.size())
            history_position = -1;
      }

      if (prev_history_pos != history_position) {
        const std::string& history_str = history_position >= 0 ? input_log[history_position] : "";
        data->DeleteChars(0, data->BufTextLen);
        data->InsertChars(0, history_str.c_str());
      }
      break;
    }
    default:;
  }

  return 0;
}

auto RuntimeConsole::help_command(this RuntimeConsole& self, const ParsedCommandValue& value) -> void {
  if (!value.as_string().empty()) {
    const std::array cvar_systems = {
      &App::get_rendercontext().context_cvar.system,
      self.scene_cvar_system,
    };
    const std::hash<std::string> hasher = {};
    const auto hashed = hasher(value.as_string());
    CVarParameter* cvar = nullptr;
    for (auto* s : cvar_systems) {
      cvar = s->get_cvar(hashed);
      if (cvar) {
        break;
      }
    }
    if (cvar) {
      const auto cvar_description = fmt::format("CVar Description: {}", cvar->description);
      self.add_log(cvar_description.c_str(), loguru::Verbosity_INFO);
    }
  } else {
    const auto available_commands = self.get_available_commands();
    std::string t = "Available commands: \n";
    for (const auto& c : available_commands)
      t.append(fmt::format("\t {} \n", c));

    self.add_log(t.c_str(), loguru::Verbosity_INFO);
  }
}

auto RuntimeConsole::get_available_commands(this RuntimeConsole& self) -> std::vector<std::string> {
  auto available_commands = std::vector<std::string>{};
  for (auto& [commandStr, command] : self.command_map) {
    available_commands.emplace_back(commandStr);
  }

  const std::array cvar_systems = {
    &App::get_rendercontext().context_cvar.system,
    self.scene_cvar_system,
  };

  for (const auto* s : cvar_systems) {
    for (const auto& var : s->int_cvars) {
      available_commands.emplace_back(var.parameter->name);
    }

    for (const auto& var : s->float_cvars) {
      available_commands.emplace_back(var.parameter->name);
    }
  }

  return available_commands;
}
} // namespace ox
