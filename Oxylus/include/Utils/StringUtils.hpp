#pragma once

#include <algorithm>
#include <charconv>
#include <string>
#include <string_view>

#include "Core/Option.hpp"
#include "Core/Types.hpp"

namespace ox {
namespace StringUtils {
constexpr std::string escape_str(std::string_view str) {
  std::string r;
  r.reserve(str.size());

  for (c8 c : str) {
    switch (c) {
      case '\"': r += "\\\""; break;
      case '\\': r += "\\\\"; break;
      case '\b': r += "\\b"; break;
      case '\f': r += "\\f"; break;
      case '\n': r += "\\n"; break;
      case '\r': r += "\\r"; break;
      case '\t': r += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          constexpr std::string_view hex = "0123456789abcdef";
          r += "\\u00";
          r += hex[(static_cast<unsigned char>(c) >> 4) & 0xf];
          r += hex[static_cast<unsigned char>(c) & 0xf];
        } else {
          r += c;
        }
        break;
    }
  }

  return r;
}

constexpr auto ascii_lower(const c8 c) -> c8 { return c >= 'A' && c <= 'Z' ? static_cast<c8>(c - 'A' + 'a') : c; }

// ASCII only
constexpr auto iequals(std::string_view a, std::string_view b) -> bool {
  return std::ranges::equal(a, b, {}, ascii_lower, ascii_lower);
}

// ASCII only, an empty needle is always found
constexpr auto icontains(std::string_view haystack, std::string_view needle) -> bool {
  return needle.empty() || !std::ranges::search(haystack, needle, {}, ascii_lower, ascii_lower).empty();
}

// the whole text has to be the number
template <typename T>
auto parse_integer(std::string_view text) -> option<T> {
  auto value = T{};
  const auto* end = text.data() + text.size();
  const auto [ptr, error] = std::from_chars(text.data(), end, value);
  if (text.empty() || error != std::errc() || ptr != end) {
    return nullopt;
  }

  return value;
}

inline void replace_string(std::string& subject, std::string_view search, std::string_view replace) {
  size_t pos = 0;
  while ((pos = subject.find(search, pos)) != std::string::npos) {
    subject.replace(pos, search.length(), replace);
    pos += replace.length();
  }
}

inline const char* from_char8_t(const char8_t* c) { return reinterpret_cast<const char*>(c); }
} // namespace StringUtils
} // namespace ox
