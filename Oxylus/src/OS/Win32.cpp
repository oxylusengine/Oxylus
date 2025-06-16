#include "OS.hpp"

#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace ox {
auto os::mem_page_size() -> u64 {
  ZoneScoped;

  SYSTEM_INFO sys_info = {};
  GetSystemInfo(&sys_info);
  return sys_info.dwPageSize;
}

auto os::mem_reserve(u64 size) -> void* {
  ZoneScoped;

  return VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_READWRITE);
}

auto os::mem_release(void* data, [[maybe_unused]] u64 size) -> void {
  ZoneScoped;
  TracyFree(data);
  VirtualFree(data, 0, MEM_RELEASE);
}

auto os::mem_commit(void* data, u64 size) -> bool {
  ZoneScoped;
  TracyAllocN(data, size, "Virtual Alloc");
  return VirtualAlloc(data, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

auto os::mem_decommit(void* data, [[maybe_unused]] u64 size) -> void {
  ZoneScoped;

  VirtualFree(data, 0, MEM_DECOMMIT | MEM_RELEASE);
}
} // namespace ox
