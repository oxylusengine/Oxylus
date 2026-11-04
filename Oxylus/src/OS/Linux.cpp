#include <SDL3/SDL_process.h>
#include <SDL3/SDL_properties.h>
#include <fmt/format.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <unistd.h>

#include "Memory/Stack.hpp"
#include "OS/OS.hpp"
#include "Utils/Log.hpp"

namespace ox {
static auto path_to_utf8(const std::filesystem::path& path) -> std::string {
  const auto utf8 = path.u8string();
  return std::string(reinterpret_cast<const c8*>(utf8.data()), utf8.size());
}

static auto percent_encode_path(std::string_view path) -> std::string {
  std::string result = {};
  result.reserve(path.size());
  for (const auto c : path) {
    const auto is_unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                               c == '.' || c == '_' || c == '~' || c == '/';
    if (is_unreserved) {
      result += c;
      continue;
    }

    result += fmt::format("%{:02X}", static_cast<u8>(c));
  }

  return result;
}

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

auto os::thread_id() -> i64 {
  ZoneScoped;

  return syscall(__NR_gettid);
}

auto os::set_thread_name(std::string_view name) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  pthread_setname_np(pthread_self(), stack.null_terminate_cstr(name));
}

auto os::set_thread_name(std::thread::native_handle_type thread, std::string_view name) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  pthread_setname_np(thread, stack.null_terminate_cstr(name));
}

auto os::open_folder_select_file(const std::filesystem::path& path) -> void {
  ZoneScoped;

  const auto absolute = std::filesystem::absolute(path).lexically_normal();
  const auto uri_arg = "array:string:file://" + percent_encode_path(path_to_utf8(absolute));

  const c8* args[] = {
    "dbus-send",
    "--session",
    "--dest=org.freedesktop.FileManager1",
    "--type=method_call",
    "/org/freedesktop/FileManager1",
    "org.freedesktop.FileManager1.ShowItems",
    uri_arg.c_str(),
    "string:",
    nullptr,
  };

  const auto properties = SDL_CreateProperties();
  SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, static_cast<void*>(args));
  SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
  SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_NULL);
  SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);

  auto* process = SDL_CreateProcessWithProperties(properties);
  SDL_DestroyProperties(properties);

  i32 exit_code = -1;
  if (process) {
    SDL_WaitProcess(process, true, &exit_code);
    SDL_DestroyProcess(process);
  }

  if (!process || exit_code != 0) {
    const auto parent = path_to_utf8(absolute.parent_path());
    const std::string_view fallback[] = {"xdg-open", parent};
    if (auto result = spawn_detached(fallback); !result) {
      OX_LOG_WARN("{}", result.error());
    }
  }
}

auto os::open_file_externally(const std::filesystem::path& path) -> void {
  ZoneScoped;

  const auto file = path_to_utf8(path);
  const std::string_view args[] = {"xdg-open", file};
  if (auto result = spawn_detached(args); !result) {
    OX_LOG_WARN("{}", result.error());
  }
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

  i32 file = open64(path.c_str(), flags, S_IRUSR | S_IWUSR);
  if (file < 0) {
    switch (errno) {
      case EACCES: return std::unexpected(FileError::NoAccess);
      case ENOENT: return std::unexpected(FileError::NotFound);
      case EEXIST: return std::unexpected(FileError::Exists);
      case EISDIR: return std::unexpected(FileError::IsDir);
      case EBUSY : return std::unexpected(FileError::InUse);
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

    // end of file: read() keeps returning 0, so asking for more than is left would spin forever
    if (cur_read_size == 0_iptr) {
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

  lseek64(static_cast<i32>(file), offset, SEEK_SET);
}

void os::file_stdout(std::string_view str) {
  ZoneScoped;

  write(STDOUT_FILENO, str.data(), str.length());
}

void os::file_stderr(std::string_view str) {
  ZoneScoped;

  write(STDERR_FILENO, str.data(), str.length());
}

auto os::file_map(FileDescriptor file, usize size) -> std::expected<void*, FileError> {
  ZoneScoped;

  auto* data = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, static_cast<i32>(file), 0);
  if (data == MAP_FAILED) {
    return std::unexpected(FileError::MapFailed);
  }

  madvise(data, size, MADV_SEQUENTIAL);
  madvise(data, size, MADV_WILLNEED);

  return data;
}

auto os::file_unmap(FileDescriptor, void* data, usize size) -> void {
  ZoneScoped;

  os::mem_release(data, size);
}
} // namespace ox
