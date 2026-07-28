#include "Memory/Stack.hpp"

#include <algorithm>
#include <simdutf.h>

#include "OS/OS.hpp"

namespace ox::memory {
ThreadStack& get_thread_stack() {
  thread_local ThreadStack stack;
  return stack;
}

ThreadStack::ThreadStack() {
  constexpr static usize stack_size = ox::mib_to_bytes(32);
  ptr = static_cast<u8*>(os::mem_reserve(stack_size));
  os::mem_commit(ptr, stack_size);
}

ThreadStack::~ThreadStack() { os::mem_release(ptr); }

ScopedStack::ScopedStack() {
  auto& stack = get_thread_stack();
  ptr = stack.ptr;
}

ScopedStack::~ScopedStack() {
  auto& stack = get_thread_stack();
  stack.ptr = ptr;
}

auto ScopedStack::to_utf32(std::string_view str) -> std::u32string_view {
  auto& stack = get_thread_stack();
  stack.ptr = ox::align_up(stack.ptr, alignof(c32));

  auto* begin = reinterpret_cast<c32*>(stack.ptr);
  usize size = simdutf::convert_utf8_to_utf32(str.data(), str.length(), begin);
  begin[size] = L'\0';
  stack.ptr = reinterpret_cast<u8*>(begin + size + 1);

  return {begin, size};
}

auto ScopedStack::to_utf16(std::string_view str) -> std::u16string_view {
  auto& stack = get_thread_stack();
  stack.ptr = ox::align_up(stack.ptr, alignof(c16));

  auto* begin = reinterpret_cast<c16*>(stack.ptr);
  usize size = simdutf::convert_utf8_to_utf16(str.data(), str.length(), begin);
  begin[size] = L'\0';
  stack.ptr = reinterpret_cast<u8*>(begin + size + 1);

  return {begin, size};
}

auto ScopedStack::to_utf8(std::u32string_view str) -> std::string_view {
  auto& stack = get_thread_stack();
  auto* begin = reinterpret_cast<c8*>(stack.ptr);
  usize size = simdutf::convert_utf32_to_utf8(str.data(), str.length(), begin);
  begin[size] = '\0';
  stack.ptr = reinterpret_cast<u8*>(begin + size + 1);

  return {begin, size};
}

auto ScopedStack::to_utf8(std::u16string_view str) -> std::string_view {
  auto& stack = get_thread_stack();
  auto* begin = reinterpret_cast<c8*>(stack.ptr);
  usize size = simdutf::convert_utf16_to_utf8(str.data(), str.length(), begin);
  begin[size] = '\0';
  stack.ptr = reinterpret_cast<u8*>(begin + size + 1);

  return {begin, size};
}

auto ScopedStack::to_utf8(c32 str) -> std::string_view { return to_utf8({&str, 1}); }

auto ScopedStack::to_upper(std::string_view str) -> std::string_view {
  auto& stack = get_thread_stack();
  auto* begin = reinterpret_cast<c8*>(stack.ptr);
  std::ranges::copy(str, begin);
  c8* end = reinterpret_cast<c8*>(stack.ptr + str.length());
  stack.ptr = reinterpret_cast<u8*>(end + 1);

  std::transform(begin, end, begin, ::toupper);
  *end = '\0';

  return {begin, end};
}

auto ScopedStack::to_lower(std::string_view str) -> std::string_view {
  auto& stack = get_thread_stack();
  auto* begin = reinterpret_cast<c8*>(stack.ptr);
  std::ranges::copy(str, begin);
  auto* end = reinterpret_cast<c8*>(stack.ptr + str.length());
  stack.ptr = reinterpret_cast<u8*>(end + 1);

  std::transform(begin, end, begin, ::tolower);
  *end = '\0';

  return {begin, end};
}

auto ScopedStack::null_terminate(std::string_view str) -> std::string_view {
  auto& stack = get_thread_stack();
  auto* begin = reinterpret_cast<c8*>(stack.ptr);
  std::ranges::copy(str, begin);
  auto* end = reinterpret_cast<c8*>(stack.ptr + str.length());
  stack.ptr = reinterpret_cast<u8*>(end + 1);

  *end = '\0';

  return {begin, end};
}

auto ScopedStack::null_terminate_cstr(std::string_view str) -> const c8* {
  auto& stack = get_thread_stack();
  auto* begin = reinterpret_cast<c8*>(stack.ptr);
  std::ranges::copy(str, begin);
  auto* end = reinterpret_cast<c8*>(stack.ptr + str.length());
  stack.ptr = reinterpret_cast<u8*>(end + 1);

  *end = '\0';

  return begin;
}

} // namespace ox::memory
