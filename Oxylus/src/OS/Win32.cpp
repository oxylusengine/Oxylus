#include <string>

#include "OS/OS.hpp"
#include "Utils/Log.hpp"

#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#include <ShlObj_core.h>
#include <Windows.h>
#include <comdef.h>
#include <shellapi.h>

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

auto os::thread_id() -> i64 {
  ZoneScoped;

  return GetCurrentThreadId();
}

auto os::set_thread_name(std::string_view name) -> void {
  ZoneScoped;

  auto wide_name = std::wstring(name.begin(), name.end());
  SetThreadDescription(GetCurrentThread(), wide_name.c_str());
}

auto os::set_thread_name(std::thread::native_handle_type thread, std::string_view name) -> void {
  ZoneScoped;

  auto wide_name = std::wstring(name.begin(), name.end());
  SetThreadDescription(thread, wide_name.c_str());
}

auto os::open_folder_select_file(const std::filesystem::path& path) -> void {
  ZoneScoped;

  if (const auto pidl = ILCreateFromPath(path.c_str())) {
    SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0); // TODO: check for result
    ILFree(pidl);
  }
}

auto os::open_file_externally(const std::filesystem::path& path) -> void {
  ZoneScoped;

  ShellExecute(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_RESTORE);
}

auto os::file_open(const std::filesystem::path& path, FileAccess access) -> std::expected<FileDescriptor, FileError> {
  ZoneScoped;

  SetLastError(0);

  DWORD flags = 0;
  DWORD creation_flags = 0;
  DWORD share_flags = FILE_SHARE_READ;

  switch (access) {
    case FileAccess::Read:
      flags = GENERIC_READ;
      creation_flags = OPEN_EXISTING;
      break;

    case FileAccess::Write:
      flags = GENERIC_WRITE;
      creation_flags = CREATE_ALWAYS;
      share_flags = 0;
      break;

    case FileAccess::ReadWrite:
      flags = GENERIC_READ | GENERIC_WRITE;
      creation_flags = CREATE_ALWAYS;
      break;
  }

  HANDLE file_handle = CreateFileW(
    path.c_str(),          // File path (wide string)
    flags,                 // Desired access (R, W, or RW)
    share_flags,           // Share mode
    nullptr,               // Security attributes
    creation_flags,        // Creation disposition
    FILE_ATTRIBUTE_NORMAL, // Flags and attributes
    nullptr
  );                       // Template file

  if (file_handle == INVALID_HANDLE_VALUE) {
    DWORD error = GetLastError();
    switch (error) {
      case ERROR_FILE_NOT_FOUND   : return std::unexpected(FileError::Exists);
      case ERROR_PATH_NOT_FOUND   : return std::unexpected(FileError::Exists);
      case ERROR_ACCESS_DENIED    : return std::unexpected(FileError::NoAccess);
      case ERROR_SHARING_VIOLATION: return std::unexpected(FileError::InUse);
      case ERROR_DIRECTORY        : return std::unexpected(FileError::IsDir);
      default                     : return std::unexpected(FileError::Unknown);
    }
  }

  return static_cast<FileDescriptor>(reinterpret_cast<iptr>(file_handle));
}

auto os::file_close(FileDescriptor file) -> void {
  ZoneScoped;

  HANDLE file_handle = reinterpret_cast<HANDLE>(file);
  CloseHandle(file_handle);
}

auto os::file_size(FileDescriptor file) -> std::expected<usize, FileError> {
  ZoneScoped;

  LARGE_INTEGER li = {};
  GetFileSizeEx(reinterpret_cast<HANDLE>(file), &li);

  return static_cast<usize>(li.QuadPart);
}

auto os::file_read(FileDescriptor file, void* data, usize size) -> usize {
  ZoneScoped;

  auto file_handle = reinterpret_cast<HANDLE>(file);

  u64 read_bytes_size = 0;
  u64 target_size = size;
  while (read_bytes_size < target_size) {
    auto remainder_size = static_cast<DWORD>(target_size - read_bytes_size);
    u8* cur_data = reinterpret_cast<u8*>(data) + read_bytes_size;

    DWORD cur_read_size = 0;
    OVERLAPPED overlapped = {};
    overlapped.Offset = read_bytes_size & 0x00000000ffffffffull;
    overlapped.OffsetHigh = (read_bytes_size & 0xffffffff00000000ull) >> 32u;
    ReadFile(file_handle, cur_data, remainder_size, &cur_read_size, &overlapped);
    if (cur_read_size < 0) {
      OX_LOG_TRACE("File read interrupted! {}", cur_read_size);
      break;
    }

    read_bytes_size += cur_read_size;
  }

  return read_bytes_size;
}

auto os::file_write(FileDescriptor file, const void* data, usize size) -> usize {
  ZoneScoped;

  HANDLE file_handle = reinterpret_cast<HANDLE>(file);
  u64 written_bytes_size = 0;
  u64 target_size = size;
  while (written_bytes_size < target_size) {
    auto remainder_size = static_cast<DWORD>(target_size - written_bytes_size);
    const u8* cur_data = reinterpret_cast<const u8*>(data) + written_bytes_size;
    DWORD cur_written_size = 0;
    OVERLAPPED overlapped = {};
    overlapped.Offset = written_bytes_size & 0x00000000ffffffffull;
    overlapped.OffsetHigh = (written_bytes_size & 0xffffffff00000000ull) >> 32;
    if (WriteFile(file_handle, cur_data, remainder_size, &cur_written_size, &overlapped) == 0) {
      OX_LOG_TRACE("File write interrupted! {}", cur_written_size);
      break;
    }

    written_bytes_size += cur_written_size;
  }

  return written_bytes_size;
}

auto os::file_seek(FileDescriptor file, i64 offset) -> void {
  ZoneScoped;

  LARGE_INTEGER li = {};
  li.QuadPart = offset;
  SetFilePointerEx(reinterpret_cast<HANDLE>(file), li, nullptr, FILE_BEGIN);
}

auto os::file_stdout(std::string_view str) -> void {
  ZoneScoped;

  static auto stdout_hnd = GetStdHandle(STD_OUTPUT_HANDLE);

  DWORD written = 0;
  WriteFile(stdout_hnd, str.data(), str.length(), &written, nullptr);
}

auto os::file_stderr(std::string_view str) -> void {
  ZoneScoped;

  static auto stdout_hnd = GetStdHandle(STD_ERROR_HANDLE);

  DWORD written = 0;
  WriteFile(stdout_hnd, str.data(), str.length(), &written, nullptr);
}

} // namespace ox
