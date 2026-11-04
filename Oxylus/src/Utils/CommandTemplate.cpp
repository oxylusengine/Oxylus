#include "Utils/CommandTemplate.hpp"

#include <algorithm>

namespace ox {
static auto path_to_utf8(const std::filesystem::path& path) -> std::string {
  const auto utf8 = path.u8string();
  return std::string(reinterpret_cast<const c8*>(utf8.data()), utf8.size());
}

static auto split_command(std::string_view command) -> option<std::vector<std::string>> {
  std::vector<std::string> tokens = {};
  std::string current = {};
  bool is_in_token = false;
  c8 quote = '\0';

  for (const auto c : command) {
    if (quote != '\0') {
      if (c == quote) {
        quote = '\0';
      } else {
        current += c;
      }

      continue;
    }

    if (c == '"' || c == '\'') {
      quote = c;
      is_in_token = true;
      continue;
    }

    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      if (is_in_token) {
        tokens.push_back(std::move(current));
        current.clear();
        is_in_token = false;
      }

      continue;
    }

    current += c;
    is_in_token = true;
  }

  if (quote != '\0') {
    return nullopt;
  }

  if (is_in_token) {
    tokens.push_back(std::move(current));
  }

  return tokens;
}

static auto replace_all(std::string& text, std::string_view from, std::string_view to) -> void {
  usize pos = 0;
  while ((pos = text.find(from, pos)) != std::string::npos) {
    text.replace(pos, from.size(), to);
    pos += to.size();
  }
}

auto expand_command_template(std::string_view command, const CommandTemplateArgs& args)
  -> std::expected<std::vector<std::string>, std::string> {
  ZoneScoped;

  auto tokens = split_command(command);
  if (!tokens) {
    return std::unexpected("external editor command has invalid quoting");
  }

  if (tokens->empty()) {
    return std::unexpected("external editor command is empty");
  }

  const auto has_text = [&](std::string_view text) {
    return std::ranges::any_of(*tokens, [&](const std::string& token) { return token.contains(text); });
  };
  if (!has_text("{file}")) {
    return std::unexpected("external editor command must contain {file}");
  }

  if (!args.root && has_text("{root}")) {
    return std::unexpected("external editor command uses {root}, but no project is open");
  }

  const auto line = std::to_string(args.line);
  const auto column = std::to_string(args.column);
  const auto root = args.root ? path_to_utf8(*args.root) : std::string();
  const auto file = path_to_utf8(args.file);

  for (auto& token : *tokens) {
    replace_all(token, "{line}", line);
    replace_all(token, "{column}", column);
    replace_all(token, "{root}", root);
    replace_all(token, "{file}", file);
  }

  return std::move(*tokens);
}
} // namespace ox
