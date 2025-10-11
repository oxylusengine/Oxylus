#include <libproc.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include "Memory/Stack.hpp"
#include "OS/OS.hpp"
#include "Utils/Log.hpp"

namespace ox {
auto os::mem_page_size() -> u64 {
  ZoneScoped;
  return sysconf(_SC_PAGESIZE);
}

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
  return mprotect(data, size, PROT_READ | PROT_WRITE) == 0;
}

auto os::mem_decommit(void* data, u64 size) -> void {
  ZoneScoped;
  // https://github.com/chromium/chromium/blob/master/base/memory/discardable_shared_memory.cc#L410

  madvise(data, size, MADV_FREE_REUSABLE);
  mprotect(data, size, PROT_NONE);
}

auto os::thread_id() -> i64 {
  ZoneScoped;

  uint64_t thread_id;
  pthread_threadid_np(pthread_self(), &thread_id);
  return static_cast<i64>(thread_id);
}

auto os::set_thread_name(std::string_view name) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  pthread_setname_np(stack.null_terminate_cstr(name));
}

auto os::set_thread_name(std::thread::native_handle_type thread, std::string_view name) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  // NOTE: On macOS, you can only set the current thread's name.
  // Setting another thread's name requires a different approach.
  if (pthread_equal(thread, pthread_self())) {
    pthread_setname_np(stack.null_terminate_cstr(name));
  } else {
    OX_LOG_WARN("Setting another thread's name is not implemented on this platform!");
  }
}
} // namespace ox
