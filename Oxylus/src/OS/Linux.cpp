#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <unistd.h>

#include "OS.hpp"

namespace ox {
auto os::mem_page_size() -> u64 { return sysconf(_SC_PAGESIZE); }

auto os::mem_reserve(u64 size) -> void* {
  ZoneScoped;

  return mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
}

auto os::mem_release(void* data, u64 size) -> void {
  ZoneScoped;

  munmap(data, size);
}

auto os::mem_commit(void* data, u64 size) -> bool {
  ZoneScoped;

  return mprotect(data, size, PROT_READ | PROT_WRITE);
}

auto os::mem_decommit(void* data, u64 size) -> void {
  ZoneScoped;

  madvise(data, size, MADV_DONTNEED);
  mprotect(data, size, PROT_NONE);
}
} // namespace ox
