#pragma once

namespace ox {
namespace os {
//  ── MEMORY ──────────────────────────────────────────────────────────
auto mem_page_size() -> u64;
auto mem_reserve(u64 size) -> void*;
auto mem_release(void* data, u64 size = 0) -> void;
auto mem_commit(void* data, u64 size) -> bool;
auto mem_decommit(void* data, u64 size) -> void;
} // namespace os
} // namespace ox
