#pragma once

#include <string_view>
#include <thread>

#include "Core/Types.hpp"

namespace ox {
namespace os {
//  ── MEMORY ──────────────────────────────────────────────────────────
auto mem_page_size() -> u64;
auto mem_reserve(u64 size) -> void*;
auto mem_release(void* data, u64 size = 0) -> void;
auto mem_commit(void* data, u64 size) -> bool;
auto mem_decommit(void* data, u64 size) -> void;

//  ── THREADS ─────────────────────────────────────────────────────────
auto thread_id() -> i64;
auto set_thread_name(std::string_view name) -> void;
auto set_thread_name(std::thread::native_handle_type thread, std::string_view name) -> void;
} // namespace os
} // namespace ox
