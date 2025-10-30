#include <libproc.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
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

auto os::open_folder_select_file(const std::filesystem::path& path) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  auto* command = stack.format_char("open -R \"{}\"", path);
  int result = system(command);

  if (result != 0) {
    OX_LOG_WARN("Failed to open folder and select file: {}", path);
  }
}

auto os::open_file_externally(const std::filesystem::path& path) -> void {
  ZoneScoped;

  OX_LOG_WARN("Not implemented on this platform.");
}

auto os::file_open(const std::filesystem::path& path, FileAccess access) -> std::expected<FileDescriptor, FileError> {
  ZoneScoped;

  errno = 0;
  i32 flags = 0;

  switch (access) {
    case FileAccess::Read     : flags = O_RDONLY; break;
    case FileAccess::Write    : flags = O_WRONLY | O_CREAT | O_TRUNC; break;
    case FileAccess::ReadWrite: flags = O_RDWR | O_CREAT | O_TRUNC; break;
  }

  i32 file = open(path.c_str(), flags, S_IRUSR | S_IWUSR);

  if (file < 0) {
    switch (errno) {
      case EACCES: return std::unexpected(FileError::NoAccess);
      case EPERM : return std::unexpected(FileError::NoAccess);
      case EEXIST: return std::unexpected(FileError::Exists);
      case EISDIR: return std::unexpected(FileError::IsDir);
      case EBUSY : return std::unexpected(FileError::InUse);
      case ENOENT: return std::unexpected(FileError::Exists);
      default    : return std::unexpected(FileError::Unknown);
    }
  }

  return static_cast<FileDescriptor>(file);
}

auto os::file_close(FileDescriptor file) -> void {
  ZoneScoped;

  close(static_cast<i32>(file));
}

auto os::file_size(FileDescriptor file) -> std::expected<usize, FileError> {
  ZoneScoped;

  errno = 0;

  struct stat st = {};
  fstat(static_cast<i32>(file), &st);
  if (errno != 0) {
    return std::unexpected(FileError::Unknown);
  }

  return st.st_size;
}

auto os::file_read(FileDescriptor file, void* data, usize size) -> usize {
  ZoneScoped;

  u64 read_bytes_size = 0;
  u64 target_size = size;
  while (read_bytes_size < target_size) {
    u64 remainder_size = target_size - read_bytes_size;
    u8* cur_data = reinterpret_cast<u8*>(data) + read_bytes_size;

    errno = 0;
    iptr cur_read_size = read(static_cast<i32>(file), cur_data, remainder_size);
    if (cur_read_size < 0_iptr) {
      OX_LOG_TRACE("File read interrupted! {}", cur_read_size);
      break;
    }

    read_bytes_size += cur_read_size;
  }

  return read_bytes_size;
}

auto os::file_write(FileDescriptor file, const void* data, usize size) -> usize {
  ZoneScoped;

  u64 written_bytes_size = 0;
  u64 target_size = size;
  while (written_bytes_size < target_size) {
    u64 remainder_size = target_size - written_bytes_size;
    const u8* cur_data = reinterpret_cast<const u8*>(data) + written_bytes_size;

    errno = 0;
    iptr cur_written_size = write(static_cast<i32>(file), cur_data, remainder_size);
    if (cur_written_size < 0_iptr) {
      break;
    }

    written_bytes_size += cur_written_size;
  }

  return written_bytes_size;
}

auto os::file_seek(FileDescriptor file, i64 offset) -> void {
  ZoneScoped;

  lseek(static_cast<i32>(file), offset, SEEK_SET);
}

void os::file_stdout(std::string_view str) {
  ZoneScoped;

  write(STDOUT_FILENO, str.data(), str.length());
}

void os::file_stderr(std::string_view str) {
  ZoneScoped;

  write(STDERR_FILENO, str.data(), str.length());
}

} // namespace ox
