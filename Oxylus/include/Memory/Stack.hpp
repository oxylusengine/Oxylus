#pragma once

#include <fmt/core.h>
#include <memory>
#include <span>
#include <string_view>

#include "Core/Types.hpp"

namespace ox::memory {
struct ThreadStack {
  u8* ptr = nullptr;

  ThreadStack();
  ~ThreadStack();
};

auto get_thread_stack() -> ThreadStack&;

struct ScopedStack {
  u8* ptr = nullptr;

  ScopedStack();
  ScopedStack(const ScopedStack&) = delete;
  ScopedStack(ScopedStack&&) = delete;
  ~ScopedStack();

  auto operator=(const ScopedStack&) -> ScopedStack& = delete;
  auto operator=(ScopedStack&&) -> ScopedStack& = delete;

  template <typename T>
  auto alloc() -> T* {
    auto& stack = get_thread_stack();
    stack.ptr = ox::align_up(stack.ptr, alignof(T));

    auto* v = reinterpret_cast<T*>(stack.ptr);
    stack.ptr += sizeof(T);

    std::uninitialized_default_construct(v);

    return v;
  }

  template <typename T>
  auto alloc(usize count) -> std::span<T> {
    auto& stack = get_thread_stack();
    stack.ptr = ox::align_up(stack.ptr, alignof(T));

    auto* v = reinterpret_cast<T*>(stack.ptr);
    stack.ptr += sizeof(T) * count;

    std::uninitialized_default_construct_n(v, count);

    return {v, count};
  }

  template <typename T, typename... ArgsT>
  auto alloc_n(ArgsT&&... args) -> std::span<T> {
    usize count = sizeof...(ArgsT);
    std::span<T> spn = alloc<T>(count);
    std::construct_at(reinterpret_cast<T*>(spn.data()), std::forward<ArgsT>(args)...);

    return spn;
  }

  template <typename... ArgsT>
  auto format(const fmt::format_string<ArgsT...> fmt, ArgsT&&... args) -> std::string_view {
    auto& stack = get_thread_stack();
    c8* begin = reinterpret_cast<c8*>(stack.ptr);
    c8* end = fmt::vformat_to(begin, fmt.get(), fmt::make_format_args(args...));
    *end = '\0';
    stack.ptr = reinterpret_cast<u8*>(end + 1);

    return {begin, end};
  }

  template <typename... ArgsT>
  auto format_char(const fmt::format_string<ArgsT...> fmt, ArgsT&&... args) -> const c8* {
    auto& stack = get_thread_stack();
    c8* begin = reinterpret_cast<c8*>(stack.ptr);
    c8* end = fmt::vformat_to(begin, fmt.get(), fmt::make_format_args(args...));
    *end = '\0';
    stack.ptr = reinterpret_cast<u8*>(end + 1);

    return begin;
  }

  auto to_utf32(std::string_view str) -> std::u32string_view;
  auto to_utf16(std::string_view str) -> std::u16string_view;
  auto to_utf8(std::u32string_view str) -> std::string_view;
  auto to_utf8(std::u16string_view str) -> std::string_view;
  auto to_utf8(c32 str) -> std::string_view;
  auto to_upper(std::string_view str) -> std::string_view;
  auto to_lower(std::string_view str) -> std::string_view;
  auto null_terminate(std::string_view str) -> std::string_view;
  auto null_terminate_cstr(std::string_view str) -> const c8*;
};
} // namespace ox::memory
