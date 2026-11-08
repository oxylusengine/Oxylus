#include "Utils/SourceReference.hpp"

#include <cctype>
#include <charconv>

#include "Core/Option.hpp"

namespace ox {
static auto trim(std::string_view text) -> std::string_view {
  const auto start = text.find_first_not_of(" \t\r");
  if (start == std::string_view::npos) {
    return {};
  }

  const auto end = text.find_last_not_of(" \t\r");
  return text.substr(start, end - start + 1);
}

// the digits at the start of `text`, and how many characters they took
static auto read_number(std::string_view text, usize& length) -> option<u32> {
  auto value = 0_u32;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  length = static_cast<usize>(end - text.data());
  if (error != std::errc() || length == 0) {
    return nullopt;
  }

  return value;
}

// a file name ends in an extension, which is what tells `File.cs(12,5)` apart from prose with a number in brackets
static auto has_extension(std::string_view path) -> bool {
  const auto dot = path.rfind('.');
  const auto separator = path.find_last_of("/\\");
  if (dot == std::string_view::npos || dot + 1 == path.size()) {
    return false;
  }

  if (separator != std::string_view::npos && dot < separator) {
    return false;
  }

  for (const auto c : path.substr(dot + 1)) {
    if (!std::isalnum(static_cast<u8>(c))) {
      return false;
    }
  }

  return true;
}

// `   at Game.Logic.Update(Single dt) in C:\game\Logic.cs:line 12`
static auto read_stack_frame(std::string_view line) -> option<SourceReference> {
  constexpr auto LINE_MARKER = std::string_view(":line ");
  constexpr auto IN_MARKER = std::string_view(" in ");

  const auto trimmed = trim(line);
  if (!trimmed.starts_with("at ")) {
    return nullopt;
  }

  const auto marker = trimmed.rfind(LINE_MARKER);
  if (marker == std::string_view::npos) {
    return nullopt;
  }

  const auto in = trimmed.rfind(IN_MARKER, marker);
  if (in == std::string_view::npos) {
    return nullopt;
  }

  const auto digits = trimmed.substr(marker + LINE_MARKER.size());
  auto length = usize{0};
  const auto line_number = read_number(digits, length);
  if (!line_number || length != digits.size()) {
    return nullopt;
  }

  const auto path = trimmed.substr(in + IN_MARKER.size(), marker - in - IN_MARKER.size());
  if (path.empty()) {
    return nullopt;
  }

  return SourceReference{.path = path, .line = *line_number};
}

// `C:\game\Logic.cs(12,5): error CS0103: ...`, the column is optional
static auto read_msbuild(std::string_view line) -> option<SourceReference> {
  const auto trimmed = trim(line);
  for (auto paren = trimmed.find('('); paren != std::string_view::npos; paren = trimmed.find('(', paren + 1)) {
    const auto path = trimmed.substr(0, paren);
    if (!has_extension(path)) {
      continue;
    }

    auto rest = trimmed.substr(paren + 1);
    auto length = usize{0};
    const auto line_number = read_number(rest, length);
    if (!line_number) {
      continue;
    }

    rest.remove_prefix(length);
    auto column = 0_u32;
    if (rest.starts_with(',')) {
      rest.remove_prefix(1);
      const auto column_number = read_number(rest, length);
      if (!column_number) {
        continue;
      }

      column = *column_number;
      rest.remove_prefix(length);
    }

    if (!rest.starts_with("):")) {
      continue;
    }

    return SourceReference{.path = path, .line = *line_number, .column = column};
  }

  return nullopt;
}

auto find_source_references(std::string_view text) -> ankerl::svector<SourceReference, 4> {
  ZoneScoped;

  auto references = ankerl::svector<SourceReference, 4>();
  while (!text.empty()) {
    const auto end = text.find('\n');
    const auto line = text.substr(0, end);
    text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);

    if (auto reference = read_stack_frame(line)) {
      references.push_back(*reference);
    } else if (auto msbuild_reference = read_msbuild(line)) {
      references.push_back(*msbuild_reference);
    }
  }

  return references;
}
} // namespace ox
