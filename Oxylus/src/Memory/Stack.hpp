#pragma once

#include <simdutf.h>

namespace ox::memory {
struct ThreadStack {
  u8* ptr = nullptr;

  ThreadStack();
  ~ThreadStack();
};

inline ThreadStack& get_thread_stack() {
  thread_local ThreadStack stack;
  return stack;
}

struct ScopedStack {
  u8* ptr = nullptr;

  ScopedStack();
  ScopedStack(const ScopedStack&) = delete;
  ScopedStack(ScopedStack&&) = delete;
  ~ScopedStack();

  auto operator=(const ScopedStack&) = delete;
  auto operator=(ScopedStack&&) = delete;

  template <typename T>
  T* alloc() {
    ZoneScoped;

    auto& stack = get_thread_stack();
    T* v = reinterpret_cast<T*>(stack.ptr);
    stack.ptr = ox::align_up(stack.ptr + sizeof(T), alignof(T));

    return v;
  }

  template <typename T>
  std::span<T> alloc(usize count) {
    ZoneScoped;

    auto& stack = get_thread_stack();
    T* v = reinterpret_cast<T*>(stack.ptr);
    stack.ptr = ox::align_up(stack.ptr + sizeof(T) * count, alignof(T));

    return {v, count};
  }

  template <typename T, typename... ArgsT>
  std::span<T> alloc_n(ArgsT&&... args) {
    ZoneScoped;

    usize count = sizeof...(ArgsT);
    std::span<T> spn = alloc<T>(count);
    std::construct_at(reinterpret_cast<T*>(spn.data()), std::forward<ArgsT>(args)...);

    return spn;
  }

  template <typename... ArgsT>
  std::string_view format(const fmt::format_string<ArgsT...> fmt, ArgsT&&... args) {
    ZoneScoped;

    auto& stack = get_thread_stack();
    c8* begin = reinterpret_cast<c8*>(stack.ptr);
    c8* end = fmt::vformat_to(begin, fmt.get(), fmt::make_format_args(args...));
    *end = '\0';
    stack.ptr = ox::align_up(reinterpret_cast<u8*>(end + 1), 8);

    return {begin, end};
  }

  template <typename... ArgsT>
  const c8* format_char(const fmt::format_string<ArgsT...> fmt, ArgsT&&... args) {
    ZoneScoped;

    auto& stack = get_thread_stack();
    c8* begin = reinterpret_cast<c8*>(stack.ptr);
    c8* end = fmt::vformat_to(begin, fmt.get(), fmt::make_format_args(args...));
    *end = '\0';
    stack.ptr = ox::align_up(reinterpret_cast<u8*>(end + 1), 8);

    return begin;
  }

  std::u32string_view to_utf32(std::string_view str) {
    ZoneScoped;

    auto& stack = get_thread_stack();
    auto* begin = reinterpret_cast<c32*>(stack.ptr);
    usize size = simdutf::convert_utf8_to_utf32(str.data(), str.length(), begin);
    begin[size] = L'\0';
    stack.ptr = ox::align_up(stack.ptr + (size + 1) * sizeof(c32), 8);

    return {reinterpret_cast<c32*>(begin), size};
  }

  std::u16string_view to_utf16(std::string_view str) {
    ZoneScoped;

    auto& stack = get_thread_stack();
    c16* begin = reinterpret_cast<c16*>(stack.ptr);
    usize size = simdutf::convert_utf8_to_utf16(str.data(), str.length(), begin);
    begin[size] = L'\0';
    stack.ptr = ox::align_up(stack.ptr + (size + 1) * sizeof(c16), 8);

    return {reinterpret_cast<c16*>(begin), size};
  }

  std::string_view to_utf8(std::u32string_view str) {
    ZoneScoped;

    auto& stack = get_thread_stack();
    auto* begin = reinterpret_cast<c8*>(stack.ptr);
    usize size = simdutf::convert_utf32_to_utf8(str.data(), str.length(), begin);
    begin[size] = '\0';
    stack.ptr = ox::align_up(stack.ptr + size + 1, 8);

    return {begin, size};
  }

  std::string_view to_utf8(std::u16string_view str) {
    ZoneScoped;

    auto& stack = get_thread_stack();
    auto* begin = reinterpret_cast<c8*>(stack.ptr);
    usize size = simdutf::convert_utf16_to_utf8(str.data(), str.length(), begin);
    begin[size] = '\0';
    stack.ptr = ox::align_up(stack.ptr + size + 1, 8);

    return {begin, size};
  }

  std::string_view to_utf8(c32 str) {
    ZoneScoped;

    return to_utf8({&str, 1});
  }

  std::string_view to_upper(std::string_view str) {
    ZoneScoped;

    auto& stack = get_thread_stack();
    auto* begin = reinterpret_cast<c8*>(stack.ptr);
    std::ranges::copy(str, begin);
    c8* end = reinterpret_cast<c8*>(stack.ptr + str.length());
    stack.ptr = ox::align_up(reinterpret_cast<u8*>(end + 1), 8);

    std::transform(begin, end, begin, ::toupper);
    *end = '\0';

    return {begin, end};
  }

  std::string_view to_lower(std::string_view str) {
    ZoneScoped;

    auto& stack = get_thread_stack();
    auto* begin = reinterpret_cast<c8*>(stack.ptr);
    std::ranges::copy(str, begin);
    auto* end = reinterpret_cast<c8*>(stack.ptr + str.length());
    stack.ptr = ox::align_up(reinterpret_cast<u8*>(end + 1), 8);

    std::transform(begin, end, begin, ::tolower);
    *end = '\0';

    return {begin, end};
  }

  std::string_view null_terminate(std::string_view str) {
    ZoneScoped;

    auto& stack = get_thread_stack();
    auto* begin = reinterpret_cast<c8*>(stack.ptr);
    std::ranges::copy(str, begin);
    auto* end = reinterpret_cast<c8*>(stack.ptr + str.length());
    stack.ptr = ox::align_up(reinterpret_cast<u8*>(end + 1), 8);

    *end = '\0';

    return {begin, end};
  }

  const c8* null_terminate_cstr(std::string_view str) {
    ZoneScoped;

    auto& stack = get_thread_stack();
    auto* begin = reinterpret_cast<c8*>(stack.ptr);
    std::ranges::copy(str, begin);
    auto* end = reinterpret_cast<c8*>(stack.ptr + str.length());
    stack.ptr = ox::align_up(reinterpret_cast<u8*>(end + 1), 8);

    *end = '\0';

    return begin;
  }
};
} // namespace ox::memory
