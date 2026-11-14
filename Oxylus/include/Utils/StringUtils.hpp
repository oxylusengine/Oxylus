#pragma once

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
