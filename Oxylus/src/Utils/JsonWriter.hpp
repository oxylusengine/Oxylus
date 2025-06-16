#pragma once

#include <sstream>

#include "Utils/StringUtils.hpp"

namespace ox {
struct JsonWriter {
  std::stringstream stream;
  constexpr static std::string_view indent_str = "  ";
  u32 depth = 0;
  bool first_element = true;
  bool has_key = false;

  void indent() {
    for (u32 i = 0; i < depth; ++i) {
      stream << indent_str;
    }
  }

  void new_element() {
    if (has_key) {
      has_key = false;
      return;
    }

    if (!first_element) {
      stream << ',';
    }

    if (depth > 0) {
      stream << '\n';
      indent();
    }

    first_element = false;
  }

  JsonWriter& key(std::string_view key) {
    new_element();
    stream << '"' << key << "\": ";
    has_key = true;
    return *this;
  }

  JsonWriter& operator[](std::string_view key_) { return key(key_); }

  void begin_obj() {
    new_element();
    stream << '{';
    ++depth;
    first_element = true;
  }

  void end_obj() {
    --depth;
    if (!first_element) {
      stream << '\n';
      indent();
    }
    stream << '}';
    first_element = false;
  }

  void begin_array() {
    new_element();
    stream << '[';
    ++depth;
    first_element = true;
  }

  void end_array() {
    --depth;
    if (!first_element) {
      stream << '\n';
      indent();
    }

    stream << ']';
    first_element = false;
  }

  void str(std::string_view v) {
    new_element();
    stream << '"' << StringUtils::escape_str(v) << '"';
  }

  template <typename T>
  JsonWriter& operator=(std::span<T> span) {
    begin_array();
    for (usize i = 0; i < span.size(); i++) {
      new_value(span[i]);
    }
    end_array();

    return *this;
  }

  template <glm::length_t N, typename T>
  JsonWriter& operator=(const glm::vec<N, T>& vec) {
    constexpr static std::string_view components[] = {"x", "y", "z", "w"};
    begin_obj();
    for (auto i = 0; i < N; i++) {
      key(components[i]);
      new_value(vec[i]);
    }
    end_obj();
    return *this;
  }

  JsonWriter& operator=(const glm::quat& quat) {
    constexpr static std::string_view components[] = {"x", "y", "z", "w"};
    begin_obj();
    for (usize i = 0; i < ox::count_of(components); i++) {
      key(components[i]);
      new_value(quat[static_cast<i32>(i)]);
    }
    end_obj();
    return *this;
  }

  JsonWriter& operator=(::fs::path& v) {
    str(v.string());
    return *this;
  }

  JsonWriter& operator=(std::string_view v) {
    str(v);
    return *this;
  }

  JsonWriter& operator=(const c8* v) {
    str(v);
    return *this;
  }

  JsonWriter& operator=(char c) {
    str({&c, 1ull});
    return *this;
  }

  template <typename T>
  void new_value(T&& value) {
    new_element();
    stream << std::forward<T>(value);
  }

  JsonWriter& operator=(f64 value) {
    new_value(value);
    return *this;
  }

  JsonWriter& operator=(f32 value) {
    new_value(value);
    return *this;
  }

  JsonWriter& operator=(i64 value) {
    new_value(value);
    return *this;
  }

  JsonWriter& operator=(i32 value) {
    new_value(value);
    return *this;
  }

  JsonWriter& operator=(i16 value) {
    new_value(value);
    return *this;
  }

  JsonWriter& operator=(i8 value) {
    new_value(value);
    return *this;
  }

  JsonWriter& operator=(u64 value) {
    new_value(value);
    return *this;
  }

  JsonWriter& operator=(u32 value) {
    new_value(value);
    return *this;
  }

  JsonWriter& operator=(u16 value) {
    new_value(value);
    return *this;
  }

  JsonWriter& operator=(u8 value) {
    new_value(value);
    return *this;
  }

  JsonWriter& operator=(bool value) {
    new_value(value ? "true" : "false");
    return *this;
  }

  JsonWriter& operator=(std::nullptr_t) {
    new_value("null");
    return *this;
  }

  template <typename T>
  void operator<<(T&& t) {
    this->operator=(std::forward<T>(t));
  }
};
} // namespace ox
